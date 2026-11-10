#pragma once

#include "aowis/config/config_root.h"
#include "aowis/config/object_id.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::config {

// Opaque JSON objects are stored as one complete, versioned, mode-0600 file.
// A later secret-bearing schema can keep metadata and its secret content in
// this same atomic record, avoiding a partially updated two-file profile.
struct ObjectInfo {
    ObjectId id;
    std::uint32_t schema_version;
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
    std::string filename; // Entry name inside the collection directory; never payload text.
    std::optional<ObjectId> id; // Present when the filename is a valid UUIDv7 object name.
    Error error;
};

struct ObjectListing {
    std::vector<ObjectInfo> objects; // Sorted by ID.
    std::vector<UnreadableObject> unreadable; // Sorted by filename.
};

struct ObjectSnapshot {
    std::vector<StoredObject> objects; // Sorted by ID; all read under one lock.
    std::vector<UnreadableObject> unreadable; // Sorted by filename.
};

// Upper bound for waiting on another process holding the collection lock.
// Every call is synchronous, so a GUI must still not block its event loop on
// a long timeout; a zero timeout performs exactly one non-blocking attempt.
inline constexpr std::chrono::milliseconds default_lock_timeout{5000};

class ObjectCollection final {
public:
    // Creates private owner/collection directories if absent. Ancestors above
    // the owner directory are trusted and may be symlinks (e.g. /home ->
    // /var/home); the owner directory, collection directory and records must
    // not be symlinks and must be private to the effective user.
    [[nodiscard]] static Result<ObjectCollection> open(
        const ConfigRoot &root, std::string_view collection,
        std::chrono::milliseconds lock_timeout = default_lock_timeout);

    [[nodiscard]] Result<ObjectListing> list() const;
    [[nodiscard]] Result<ObjectSnapshot> readAll() const;
    [[nodiscard]] Result<StoredObject> read(const ObjectId &id) const;
    [[nodiscard]] Result<StoredObject> create(std::string_view json, std::uint32_t schema_version = 1) const;
    [[nodiscard]] Result<StoredObject> update(const ObjectId &id, std::string_view json,
                                              std::string_view expected_revision,
                                              std::uint32_t schema_version = 1) const;
    [[nodiscard]] Result<bool> erase(const ObjectId &id, std::string_view expected_revision) const;

    // Deletes a record only while it is still corrupt (ErrorCode::corrupt_object
    // under the collection lock). A readable record yields conflict; other
    // failures (unsafe_path, unsupported_schema, ...) are returned unchanged
    // and nothing is deleted. Corrupt records have no trustworthy revision.
    [[nodiscard]] Result<bool> eraseCorrupt(const ObjectId &id) const;

    [[nodiscard]] const std::string &name() const noexcept { return this->name_; }

private:
    ObjectCollection(std::filesystem::path root_path, std::string name,
                     std::chrono::milliseconds lock_timeout)
        : root_path_(std::move(root_path)), name_(std::move(name)), lock_timeout_(lock_timeout) {}

    std::filesystem::path root_path_;
    std::string name_;
    std::chrono::milliseconds lock_timeout_;
};

} // namespace aowis::config
