// OwnerBackend over a flat key-value store; the browser backend's logic,
// kept platform-independent so it runs through the native contract tests.
//
// Contract mapping (storage_backend.h):
//  1. Read-write sessions exclude each other inside this context with a
//     per-root mutex, polled until the deadline. Across contexts (browser
//     tabs) only the lease holder may write: writeEntry/removeEntry outside
//     the lease fail, so there is at most one writer context at a time and it
//     serializes its own writers with the mutex. Readers in other contexts
//     need no lock because of item 2.
//  2. One record = one key; a single put is atomic.
//  3. Durability is the store's own (browser: localStorage persistence).
//  7. No temporaries, so no crash residue.
//  8. Read-only mode never takes the mutex or the lease and never writes.
#include "key_value_backend.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace aowis::config::detail {

std::shared_ptr<std::mutex> KeyValueStore::ownerMutex(const std::string &root)
{
    const std::lock_guard<std::mutex> guard(this->guard_);
    std::shared_ptr<std::mutex> &slot = this->mutexes_[root];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}

namespace {
const std::string key_namespace = "aowis-config:";

Result<bool> refusedWrite(BackendMode mode, LeaseState lease)
{
    if (mode == BackendMode::read_only) {
        return Result<bool>::failure({ErrorCode::permission_denied, "Configuration store is opened read-only"});
    }
    if (lease == LeaseState::pending) {
        return Result<bool>::failure({ErrorCode::lock_timeout,
            "Configuration is open for writing in another browser tab, or the tab lock is not granted yet; retry later"});
    }
    return Result<bool>::failure({ErrorCode::permission_denied,
        "Cross-tab locking (Web Locks API) is unavailable in this context; configuration is read-only"});
}

class KeyValueCollection final : public CollectionSession {
public:
    KeyValueCollection(std::shared_ptr<KeyValueStore> store, std::string root, std::string prefix, BackendMode mode)
        : store_(std::move(store)), root_(std::move(root)), prefix_(std::move(prefix)), mode_(mode) {}

    [[nodiscard]] Result<std::vector<std::string>> entryNames() const override
    {
        Result<std::vector<std::string>> keys = this->store_->keys(this->prefix_);
        if (!keys) return keys;
        std::vector<std::string> names;
        for (const std::string &key : keys.value()) {
            const std::string name = key.substr(this->prefix_.size());
            // Nested keys are not entries of this collection.
            if (name.find('/') == std::string::npos && name.size() > 5
                && name.compare(name.size() - 5, 5, ".json") == 0) {
                names.push_back(name);
            }
        }
        return Result<std::vector<std::string>>::success(std::move(names));
    }

    [[nodiscard]] Result<std::string> readEntry(const ObjectId &id) const override
    {
        Result<std::optional<std::string>> value = this->store_->get(this->prefix_ + id.value() + ".json");
        if (!value) return Result<std::string>::failure(value.error());
        if (!value.value().has_value()) {
            return Result<std::string>::failure({ErrorCode::not_found, "Object does not exist"});
        }
        if (value.value()->size() > max_record_bytes) {
            return Result<std::string>::failure({ErrorCode::object_too_large, "Stored object exceeds size limit"});
        }
        return Result<std::string>::success(std::move(*value.value()));
    }

    [[nodiscard]] Result<bool> writeEntry(const ObjectId &id, std::string_view bytes, bool create_only) override
    {
        Result<bool> allowed = this->writable();
        if (!allowed) return allowed;
        return this->store_->put(this->prefix_ + id.value() + ".json", bytes, create_only);
    }

    [[nodiscard]] Result<bool> removeEntry(const ObjectId &id) override
    {
        Result<bool> allowed = this->writable();
        if (!allowed) return allowed;
        return this->store_->remove(this->prefix_ + id.value() + ".json");
    }

private:
    // Checked per write, not per session: the lease is granted asynchronously
    // (after the first event-loop turn in a browser), and reads must work
    // before that and in non-writer tabs.
    [[nodiscard]] Result<bool> writable() const
    {
        if (this->mode_ == BackendMode::read_only) return refusedWrite(this->mode_, LeaseState::held);
        const LeaseState lease = this->store_->lease(this->root_);
        if (lease != LeaseState::held) return refusedWrite(this->mode_, lease);
        return Result<bool>::success(true);
    }

    std::shared_ptr<KeyValueStore> store_;
    std::string root_;
    std::string prefix_;
    BackendMode mode_;
};

class KeyValueOwnerSession final : public OwnerSessionImpl {
public:
    KeyValueOwnerSession(std::shared_ptr<KeyValueStore> store, std::string root, BackendMode mode,
                         std::unique_lock<std::mutex> held)
        : store_(std::move(store)), root_(std::move(root)), mode_(mode), held_(std::move(held)) {}

    [[nodiscard]] Result<std::unique_ptr<CollectionSession>> collection(const std::string &name, bool recover) override
    {
        static_cast<void>(recover); // Single-key writes leave no residue.
        return Result<std::unique_ptr<CollectionSession>>::success(std::make_unique<KeyValueCollection>(
            this->store_, this->root_, key_namespace + this->root_ + "/" + name + "/", this->mode_));
    }

private:
    std::shared_ptr<KeyValueStore> store_;
    std::string root_;
    BackendMode mode_;
    std::unique_lock<std::mutex> held_; // Owns the per-root mutex in read-write mode only.
};

class KeyValueBackend final : public OwnerBackend {
public:
    KeyValueBackend(std::shared_ptr<KeyValueStore> store, std::string root, BackendMode mode,
                    std::chrono::milliseconds lock_timeout)
        : store_(std::move(store)), root_(std::move(root)), mode_(mode), lock_timeout_(lock_timeout)
    {
        // Request the cross-tab lease as early as possible; it is granted
        // asynchronously and only checked when something is written.
        if (this->mode_ == BackendMode::read_write) static_cast<void>(this->store_->lease(this->root_));
    }

    [[nodiscard]] Result<std::unique_ptr<OwnerSessionImpl>> lock() const override
    {
        std::unique_lock<std::mutex> held;
        if (this->mode_ == BackendMode::read_write) {
            held = std::unique_lock<std::mutex>(*this->store_->ownerMutex(this->root_), std::defer_lock);
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
            std::make_unique<KeyValueOwnerSession>(this->store_, this->root_, this->mode_, std::move(held)));
    }

    [[nodiscard]] BackendMode mode() const noexcept override { return this->mode_; }

private:
    std::shared_ptr<KeyValueStore> store_;
    std::string root_;
    BackendMode mode_;
    std::chrono::milliseconds lock_timeout_;
};
} // namespace

std::shared_ptr<const OwnerBackend> openKeyValueBackend(
    std::shared_ptr<KeyValueStore> store, std::string owner_root, BackendMode mode,
    std::chrono::milliseconds lock_timeout)
{
    // Normalize "/aowis/gui/" and "/aowis/gui" to one key namespace.
    while (owner_root.size() > 1 && owner_root.back() == '/') owner_root.pop_back();
    return std::make_shared<const KeyValueBackend>(std::move(store), std::move(owner_root), mode, lock_timeout);
}

} // namespace aowis::config::detail
