#pragma once

#include "aowis/config/config_root.h"
#include "aowis/config/object_id.h"

#include <cstdint>
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

class ObjectCollection final {
public:
    // Creates private owner/collection directories if absent; this native
    // backend rejects symlink components and non-private owner directories.
    [[nodiscard]] static Result<ObjectCollection> open(
        const ConfigRoot &root, std::string_view collection);

    [[nodiscard]] Result<std::vector<ObjectInfo>> list() const;
    [[nodiscard]] Result<StoredObject> read(const ObjectId &id) const;
    [[nodiscard]] Result<StoredObject> create(std::string_view json, std::uint32_t schema_version = 1) const;
    [[nodiscard]] Result<StoredObject> update(const ObjectId &id, std::string_view json,
                                              std::string_view expected_revision,
                                              std::uint32_t schema_version = 1) const;
    [[nodiscard]] Result<bool> erase(const ObjectId &id, std::string_view expected_revision) const;

    [[nodiscard]] const std::string &name() const noexcept { return this->name_; }

private:
    ObjectCollection(std::filesystem::path root_path, std::string name)
        : root_path_(std::move(root_path)), name_(std::move(name)) {}

    std::filesystem::path root_path_;
    std::string name_;
};

} // namespace aowis::config
