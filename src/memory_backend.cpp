// In-memory backend: reference implementation of the storage_backend.h
// contract. Each collection has a std::mutex acquired by bounded try_lock
// polling (contract item 1), mirroring the POSIX flock loop. std::timed_mutex
// is deliberately avoided: libstdc++ implements try_lock_for() with
// pthread_mutex_clocklock, which ThreadSanitizer does not intercept, so
// consumer test suites run under TSan would report false races and a map of entry name -> bytes; replacing a std::string under the
// mutex is atomic for every other session (item 2). Durability (item 3) is
// trivially "until the last MemoryStorage handle is gone". There is no crash
// residue (item 7).
#include "memory_backend.h"

#include "aowis/config/memory_storage.h"
#include "aowis/config/owner_id.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace aowis::config::detail {

struct MemoryCollection {
    std::mutex lock;
    std::map<std::string, std::string> entries;
};

struct MemoryState {
    std::mutex guard; // Protects the collections map itself, never held across a session.
    std::map<std::string, std::shared_ptr<MemoryCollection>> collections;

    std::shared_ptr<MemoryCollection> collection(const std::string &name)
    {
        const std::lock_guard<std::mutex> locked(this->guard);
        std::shared_ptr<MemoryCollection> &slot = this->collections[name];
        if (!slot) slot = std::make_shared<MemoryCollection>();
        return slot;
    }
};

namespace {
class MemorySession final : public CollectionSession {
public:
    MemorySession(std::shared_ptr<MemoryCollection> collection, std::unique_lock<std::mutex> held)
        : collection_(std::move(collection)), held_(std::move(held)) {}

    [[nodiscard]] Result<std::vector<std::string>> entryNames() const override
    {
        std::vector<std::string> names;
        for (const std::pair<const std::string, std::string> &entry : this->collection_->entries) {
            if (entry.first.ends_with(".json")) names.push_back(entry.first);
        }
        return Result<std::vector<std::string>>::success(std::move(names));
    }

    [[nodiscard]] Result<std::string> readEntry(const ObjectId &id) const override
    {
        const std::map<std::string, std::string>::const_iterator found =
            this->collection_->entries.find(id.value() + ".json");
        if (found == this->collection_->entries.end()) {
            return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        if (found->second.size() > max_record_bytes) {
            return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
        }
        return Result<std::string>::success(found->second);
    }

    [[nodiscard]] Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) override
    {
        const std::string name = id.value() + ".json";
        if (create_only && this->collection_->entries.contains(name)) {
            return Result<bool>::failure({ErrorCode::conflict, "Object ID already exists"});
        }
        this->collection_->entries[name] = std::string(bytes);
        return Result<bool>::success(true);
    }

    [[nodiscard]] Result<bool> removeEntry(const ObjectId &id) override
    {
        if (this->collection_->entries.erase(id.value() + ".json") == 0) {
            return Result<bool>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        return Result<bool>::success(true);
    }

private:
    std::shared_ptr<MemoryCollection> collection_;
    std::unique_lock<std::mutex> held_;
};

class MemoryBackend final : public CollectionBackend {
public:
    MemoryBackend(std::shared_ptr<MemoryState> state, std::string collection, std::chrono::milliseconds lock_timeout)
        : state_(std::move(state)), collection_(std::move(collection)), lock_timeout_(lock_timeout) {}

    [[nodiscard]] Result<std::unique_ptr<CollectionSession>> lock(bool recover) const override
    {
        static_cast<void>(recover); // No crash residue in memory.
        std::shared_ptr<MemoryCollection> collection = this->state_->collection(this->collection_);
        std::unique_lock<std::mutex> held(collection->lock, std::defer_lock);
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::max(this->lock_timeout_, std::chrono::milliseconds(0));
        while (!held.try_lock()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return Result<std::unique_ptr<CollectionSession>>::failure(
                    {ErrorCode::lock_timeout, "Collection is locked by another session; retry later"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return Result<std::unique_ptr<CollectionSession>>::success(
            std::make_unique<MemorySession>(std::move(collection), std::move(held)));
    }

private:
    std::shared_ptr<MemoryState> state_;
    std::string collection_;
    std::chrono::milliseconds lock_timeout_;
};

Result<std::string> checkedCollection(std::string_view collection)
{
    Result<OwnerId> checked = OwnerId::parse(collection);
    if (!checked) return Result<std::string>::failure({ErrorCode::invalid_collection, "Invalid collection name"});
    return Result<std::string>::success(checked.value().value());
}
} // namespace

std::shared_ptr<const CollectionBackend> openMemoryBackend(
    std::shared_ptr<MemoryState> state, const std::string &collection,
    std::chrono::milliseconds lock_timeout)
{
    return std::make_shared<const MemoryBackend>(std::move(state), collection, lock_timeout);
}

} // namespace aowis::config::detail

namespace aowis::config {

MemoryStorage::MemoryStorage() : state_(std::make_shared<detail::MemoryState>()) {}

Result<bool> MemoryStorage::writeRawEntry(std::string_view collection, std::string_view entry_name,
                                          std::string_view bytes) const
{
    Result<std::string> checked = detail::checkedCollection(collection);
    if (!checked) return Result<bool>::failure(checked.error());
    if (entry_name.empty() || entry_name.find('/') != std::string_view::npos
        || entry_name.find('\0') != std::string_view::npos) {
        return Result<bool>::failure({ErrorCode::unsafe_path, "Invalid raw entry name"});
    }
    std::shared_ptr<detail::MemoryCollection> target = this->state_->collection(checked.value());
    const std::lock_guard<std::mutex> held(target->lock);
    target->entries[std::string(entry_name)] = std::string(bytes);
    return Result<bool>::success(true);
}

Result<std::string> MemoryStorage::readRawEntry(std::string_view collection, std::string_view entry_name) const
{
    Result<std::string> checked = detail::checkedCollection(collection);
    if (!checked) return Result<std::string>::failure(checked.error());
    std::shared_ptr<detail::MemoryCollection> target = this->state_->collection(checked.value());
    const std::lock_guard<std::mutex> held(target->lock);
    const std::map<std::string, std::string>::const_iterator found = target->entries.find(std::string(entry_name));
    if (found == target->entries.end()) {
        return Result<std::string>::failure({ErrorCode::not_found, "Raw entry does not exist"});
    }
    return Result<std::string>::success(found->second);
}

} // namespace aowis::config
