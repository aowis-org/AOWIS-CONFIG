#pragma once

#include "aowis/config/config_root.h"
#include "aowis/config/memory_storage.h"
#include "aowis/config/object_id.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::config {

namespace detail {
class OwnerBackend;
class OwnerSessionImpl;
}

// Opaque JSON objects are stored as one complete record each. The record
// envelope is internal and versioned by the library; payloads carry their own
// schema version field if they need one (e.g. WireGuard's profile_version).
struct ObjectInfo {
    ObjectId id;
    std::string revision; // Opaque optimistic-concurrency token, not a secret.
};

struct StoredObject {
    ObjectInfo info;
    std::string json; // Component-owned JSON object, never printed in errors.
};

// A collection entry which could not be loaded. Listing never fails as a
// whole because of one damaged record: the readable records are returned and
// every damaged entry is reported here so a UI can show and repair it.
struct UnreadableObject {
    std::string filename; // Entry name inside the collection; never payload text.
    std::optional<ObjectId> id; // Present when the filename is a valid UUIDv7 object name.
    Error error;
};

struct ObjectListing {
    std::vector<ObjectInfo> objects; // Sorted by ID.
    std::vector<UnreadableObject> unreadable; // Sorted by filename.
};

struct ObjectSnapshot {
    std::vector<StoredObject> objects; // Sorted by ID; all read in one session.
    std::vector<UnreadableObject> unreadable; // Sorted by filename.
};

// Upper bound for waiting on another process holding the owner lock. Every
// call is synchronous, so a GUI must still not block its event loop on a long
// timeout; a zero timeout performs exactly one non-blocking attempt.
inline constexpr std::chrono::milliseconds default_lock_timeout{5000};

enum class AccessMode {
    // Normal application store: private to the effective user, created on
    // demand, one exclusive lock per owner for every write and read.
    read_write,
    // Administrator-managed configuration (e.g. /etc/aowis/server-dvmdb):
    // nothing is created, locked or written, writes fail with
    // permission_denied, a missing collection reads as empty. Entries may be
    // owned by root and group-readable (0640 root:aowis) but never
    // group/other-writable. Safe without a lock because every write is an
    // atomic whole-record replacement.
    read_only,
};

struct OpenOptions {
    AccessMode mode = AccessMode::read_write;
    std::chrono::milliseconds lock_timeout = default_lock_timeout;
};

class OwnerStore;
class ObjectCollection;

// Holds the owner lock (read_write) across several operations on any
// collections of the same OwnerStore, e.g. "count references in connections,
// then delete a WireGuard profile" without another instance interleaving.
// Not a crash-atomic transaction: order writes so each intermediate state is
// valid. Use it on the thread that created it; release it promptly. Calling
// the session-less overloads while holding a session on the same thread
// would wait on the lock held by that session and fail with lock_timeout.
class OwnerSession final {
public:
    OwnerSession(OwnerSession &&other) noexcept;
    OwnerSession &operator=(OwnerSession &&other) noexcept;
    OwnerSession(const OwnerSession &) = delete;
    OwnerSession &operator=(const OwnerSession &) = delete;
    ~OwnerSession();

private:
    OwnerSession(std::shared_ptr<const detail::OwnerBackend> backend,
                 std::unique_ptr<detail::OwnerSessionImpl> impl);

    std::shared_ptr<const detail::OwnerBackend> backend_;
    std::unique_ptr<detail::OwnerSessionImpl> impl_;

    friend class OwnerStore;
    friend class ObjectCollection;
};

// One owner root (gui, gui-mobile, server-dvmdb, ...) on one backend.
class OwnerStore final {
public:
    // Persistent storage of the current platform. Native POSIX read_write:
    // creates private owner directories if absent; ancestors above the owner
    // directory are trusted and may be symlinks (e.g. /home -> /var/home);
    // nothing at or below the owner directory may be a symlink. read_only:
    // the owner root must exist (not_found otherwise). Windows/Emscripten:
    // unsupported_platform until their backends exist.
    [[nodiscard]] static Result<OwnerStore> open(const ConfigRoot &root, OpenOptions options = {});

    // Non-persistent store in a MemoryStorage; every platform, same semantics.
    [[nodiscard]] static Result<OwnerStore> openInMemory(const MemoryStorage &storage, OpenOptions options = {});

    // Validates the collection name; read_write also creates/verifies the
    // collection and removes crash residue now, so problems surface here.
    [[nodiscard]] Result<ObjectCollection> collection(std::string_view name) const;

    [[nodiscard]] Result<OwnerSession> lock() const;

    [[nodiscard]] AccessMode mode() const noexcept;

private:
    explicit OwnerStore(std::shared_ptr<const detail::OwnerBackend> backend) : backend_(std::move(backend)) {}

    std::shared_ptr<const detail::OwnerBackend> backend_;

    friend class ObjectCollection;
};

// The record envelope, validation, revisions and listing rules live here and
// are identical for every storage backend; backends only store bytes. Each
// operation without an OwnerSession takes and releases the owner lock itself;
// the overloads with a session run inside the caller's held session and fail
// with session_mismatch for a session of another OwnerStore.
class ObjectCollection final {
public:
    [[nodiscard]] Result<ObjectListing> list() const;
    [[nodiscard]] Result<ObjectSnapshot> readAll() const;
    [[nodiscard]] Result<StoredObject> read(const ObjectId &id) const;
    [[nodiscard]] Result<StoredObject> create(std::string_view json) const;
    [[nodiscard]] Result<StoredObject> update(const ObjectId &id, std::string_view json,
                                              std::string_view expected_revision) const;
    [[nodiscard]] Result<bool> erase(const ObjectId &id, std::string_view expected_revision) const;

    // Deletes a record only while it is still corrupt (ErrorCode::corrupt_object
    // inside the session). A readable record yields conflict; other failures
    // (unsafe_path, unsupported_schema, ...) are returned unchanged and nothing
    // is deleted. Corrupt records have no trustworthy revision.
    [[nodiscard]] Result<bool> eraseCorrupt(const ObjectId &id) const;

    [[nodiscard]] Result<ObjectListing> list(const OwnerSession &session) const;
    [[nodiscard]] Result<ObjectSnapshot> readAll(const OwnerSession &session) const;
    [[nodiscard]] Result<StoredObject> read(const OwnerSession &session, const ObjectId &id) const;
    [[nodiscard]] Result<StoredObject> create(const OwnerSession &session, std::string_view json) const;
    [[nodiscard]] Result<StoredObject> update(const OwnerSession &session, const ObjectId &id,
                                              std::string_view json, std::string_view expected_revision) const;
    [[nodiscard]] Result<bool> erase(const OwnerSession &session, const ObjectId &id,
                                     std::string_view expected_revision) const;
    [[nodiscard]] Result<bool> eraseCorrupt(const OwnerSession &session, const ObjectId &id) const;

    [[nodiscard]] const std::string &name() const noexcept { return this->name_; }

    // The store this collection belongs to, e.g. to take a session for it.
    [[nodiscard]] OwnerStore store() const { return OwnerStore(this->backend_); }

private:
    ObjectCollection(std::shared_ptr<const detail::OwnerBackend> backend, std::string name)
        : backend_(std::move(backend)), name_(std::move(name)) {}

    std::shared_ptr<const detail::OwnerBackend> backend_;
    std::string name_;

    friend class OwnerStore;
};

} // namespace aowis::config
