// In-memory backend: reference implementation of the storage_backend.h
// contract. One std::mutex per MemoryStorage is the owner lock, acquired by
// bounded try_lock polling (contract item 1) like the POSIX flock loop.
// std::timed_mutex is deliberately avoided: libstdc++ implements
// try_lock_for() with pthread_mutex_clocklock, which ThreadSanitizer does not
// intercept, so consumer suites run under TSan would report false races.
// Replacing a std::string under the lock is atomic for every other session
// (item 2); durability (item 3) is "until the last handle is gone"; there is
// no crash residue (item 7).
//
// Read-only sessions take no owner lock (item 2 makes that safe on disk),
// but the in-memory maps are not thread-safe on their own, so every access
// additionally holds the short-lived data mutex.
#include "memory_backend.h"

#include "aowis/config/memory_storage.h"
#include "aowis/config/owner_id.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace aowis::config::detail {

struct MemoryState {
    std::mutex owner_lock; // Contract item 1: held for a whole read-write session.
    std::mutex data;       // Guards the maps for single accesses, never held across calls.
    std::map<std::string, std::map<std::string, std::string>> collections;
};

namespace {
Result<bool> readOnlyWrite()
{
    return Result<bool>::failure({ErrorCode::permission_denied, "Configuration store is opened read-only"});
}

class MemoryCollection final : public CollectionSession {
public:
    MemoryCollection(std::shared_ptr<MemoryState> state, std::string name, BackendMode mode)
        : state_(std::move(state)), name_(std::move(name)), mode_(mode) {}

    [[nodiscard]] Result<std::vector<std::string>> entryNames() const override
    {
        const std::lock_guard<std::mutex> guard(this->state_->data);
        std::vector<std::string> names;
        const std::map<std::string, std::map<std::string, std::string>>::const_iterator found =
            this->state_->collections.find(this->name_);
        if (found != this->state_->collections.end()) {
            for (const std::pair<const std::string, std::string> &entry : found->second) {
                if (entry.first.ends_with(".json")) names.push_back(entry.first);
            }
        }
        return Result<std::vector<std::string>>::success(std::move(names));
    }

    [[nodiscard]] Result<std::string> readEntry(const ObjectId &id) const override
    {
        const std::lock_guard<std::mutex> guard(this->state_->data);
        const std::map<std::string, std::map<std::string, std::string>>::const_iterator collection =
            this->state_->collections.find(this->name_);
        if (collection == this->state_->collections.end()) {
            return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        const std::map<std::string, std::string>::const_iterator found = collection->second.find(id.value() + ".json");
        if (found == collection->second.end()) {
            return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        if (found->second.size() > max_record_bytes) {
            return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
        }
        return Result<std::string>::success(found->second);
    }

    [[nodiscard]] Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) override
    {
        if (this->mode_ == BackendMode::read_only) return readOnlyWrite();
        const std::lock_guard<std::mutex> guard(this->state_->data);
        std::map<std::string, std::string> &entries = this->state_->collections[this->name_];
        const std::string name = id.value() + ".json";
        if (create_only && entries.contains(name)) {
            return Result<bool>::failure({ErrorCode::conflict, "Object ID already exists"});
        }
        entries[name] = std::string(bytes);
        return Result<bool>::success(true);
    }

    [[nodiscard]] Result<bool> removeEntry(const ObjectId &id) override
    {
        if (this->mode_ == BackendMode::read_only) return readOnlyWrite();
        const std::lock_guard<std::mutex> guard(this->state_->data);
        std::map<std::string, std::string> &entries = this->state_->collections[this->name_];
        if (entries.erase(id.value() + ".json") == 0) {
            return Result<bool>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        return Result<bool>::success(true);
    }

private:
    std::shared_ptr<MemoryState> state_;
    std::string name_;
    BackendMode mode_;
};

class MemoryOwnerSession final : public OwnerSessionImpl {
public:
    MemoryOwnerSession(std::shared_ptr<MemoryState> state, std::unique_lock<std::mutex> held, BackendMode mode)
        : state_(std::move(state)), held_(std::move(held)), mode_(mode) {}

    [[nodiscard]] Result<std::unique_ptr<CollectionSession>> collection(const std::string &name, bool recover) override
    {
        static_cast<void>(recover); // No crash residue in memory.
        if (this->mode_ == BackendMode::read_write) {
            const std::lock_guard<std::mutex> guard(this->state_->data);
            this->state_->collections[name]; // A read-write open creates the collection, as on disk.
        }
        return Result<std::unique_ptr<CollectionSession>>::success(
            std::make_unique<MemoryCollection>(this->state_, name, this->mode_));
    }

private:
    std::shared_ptr<MemoryState> state_;
    std::unique_lock<std::mutex> held_; // Owns owner_lock in read-write mode only.
    BackendMode mode_;
};

class MemoryBackend final : public OwnerBackend {
public:
    MemoryBackend(std::shared_ptr<MemoryState> state, BackendMode mode, std::chrono::milliseconds lock_timeout)
        : state_(std::move(state)), mode_(mode), lock_timeout_(lock_timeout) {}

    [[nodiscard]] Result<std::unique_ptr<OwnerSessionImpl>> lock() const override
    {
        std::unique_lock<std::mutex> held(this->state_->owner_lock, std::defer_lock);
        if (this->mode_ == BackendMode::read_write) {
            const std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::now() + std::max(this->lock_timeout_, std::chrono::milliseconds(0));
            while (!held.try_lock()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return Result<std::unique_ptr<OwnerSessionImpl>>::failure(
                        {ErrorCode::lock_timeout, "Configuration is locked by another session; retry later"});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return Result<std::unique_ptr<OwnerSessionImpl>>::success(
            std::make_unique<MemoryOwnerSession>(this->state_, std::move(held), this->mode_));
    }

    [[nodiscard]] BackendMode mode() const noexcept override { return this->mode_; }

private:
    std::shared_ptr<MemoryState> state_;
    BackendMode mode_;
    std::chrono::milliseconds lock_timeout_;
};

Result<std::string> checkedCollection(std::string_view collection)
{
    Result<OwnerId> checked = OwnerId::parse(collection);
    if (!checked) return Result<std::string>::failure({ErrorCode::invalid_collection, "Invalid collection name"});
    return Result<std::string>::success(checked.value().value());
}
} // namespace

std::shared_ptr<const OwnerBackend> openMemoryBackend(
    std::shared_ptr<MemoryState> state, BackendMode mode, std::chrono::milliseconds lock_timeout)
{
    return std::make_shared<const MemoryBackend>(std::move(state), mode, lock_timeout);
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
    const std::lock_guard<std::mutex> guard(this->state_->data);
    this->state_->collections[checked.value()][std::string(entry_name)] = std::string(bytes);
    return Result<bool>::success(true);
}

Result<std::string> MemoryStorage::readRawEntry(std::string_view collection, std::string_view entry_name) const
{
    Result<std::string> checked = detail::checkedCollection(collection);
    if (!checked) return Result<std::string>::failure(checked.error());
    const std::lock_guard<std::mutex> guard(this->state_->data);
    const std::map<std::string, std::map<std::string, std::string>>::const_iterator found =
        this->state_->collections.find(checked.value());
    if (found != this->state_->collections.end()) {
        const std::map<std::string, std::string>::const_iterator entry = found->second.find(std::string(entry_name));
        if (entry != found->second.end()) return Result<std::string>::success(entry->second);
    }
    return Result<std::string>::failure({ErrorCode::not_found, "Raw entry does not exist"});
}

} // namespace aowis::config
