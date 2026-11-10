#pragma once

#include "aowis/config/object_store.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::config {

// Storage of imported WireGuard configurations. This library deliberately
// does NOT interpret WireGuard syntax: the only authority on whether a
// configuration can run is the tunnel engine (AOWIS-NETWORK's
// parseWireGuardConf()), which the application calls on import and edit.
// Stored here is the exact original text plus name and description; the only
// checks are text integrity: nonempty, at most 2 MiB, valid UTF-8, no NUL.

inline constexpr std::size_t max_wireguard_configuration_bytes = 2U * 1024U * 1024U;

struct WireGuardProfileInput {
    std::string name; // Nonblank UTF-8, at most 256 bytes, no CR/LF/NUL.
    std::string description; // UTF-8, at most 4096 bytes, no NUL.
    std::string configuration; // Complete original .conf text, including private keys.
};

struct WireGuardProfileSummary {
    ObjectInfo info;
    std::string name;
    std::string description;
};

struct WireGuardProfile {
    WireGuardProfileSummary summary;
    std::string configuration;
};

struct WireGuardProfileListing {
    std::vector<WireGuardProfileSummary> profiles; // Sorted by ID.
    // Records whose envelope or profile payload cannot be decoded; remove them
    // with eraseUnreadable() after user confirmation.
    std::vector<UnreadableObject> unreadable;
};

// The owning application's authoritative count of configured services that
// reference the profile. It is called inside the same owner session as the
// deletion, so it must read its catalogue through that session (e.g.
// connections.list(session)); no other instance can add a reference between
// the count and the delete. A missing/failed callback or a nonzero count
// blocks deletion.
using WireGuardReferenceCount = std::function<Result<std::size_t>(const OwnerSession &, const ObjectId &)>;

class WireGuardProfiles final {
public:
    // Opens the owner's "wireguard" collection (any backend, any mode).
    [[nodiscard]] static Result<WireGuardProfiles> open(const OwnerStore &store);

    // One session over the collection; never fails because of one damaged record.
    [[nodiscard]] Result<WireGuardProfileListing> list() const;
    [[nodiscard]] Result<WireGuardProfile> read(const ObjectId &id) const;
    [[nodiscard]] Result<WireGuardProfile> importText(const WireGuardProfileInput &input) const;
    [[nodiscard]] Result<WireGuardProfile> update(const ObjectId &id, std::string_view expected_revision,
                                                   const WireGuardProfileInput &input) const;
    [[nodiscard]] Result<bool> erase(const ObjectId &id, std::string_view expected_revision,
                                     const WireGuardReferenceCount &reference_count) const;
    // Removes a record listed as unreadable (corrupt envelope or not a
    // decodable profile), with the same reference guard as erase(). A record
    // that decodes as a profile yields conflict; use erase().
    [[nodiscard]] Result<bool> eraseUnreadable(const ObjectId &id,
                                               const WireGuardReferenceCount &reference_count) const;
    [[nodiscard]] Result<std::string> exportText(const ObjectId &id) const;

    // The same operations inside a caller-held owner session.
    [[nodiscard]] Result<WireGuardProfileListing> list(const OwnerSession &session) const;
    [[nodiscard]] Result<WireGuardProfile> read(const OwnerSession &session, const ObjectId &id) const;
    [[nodiscard]] Result<WireGuardProfile> importText(const OwnerSession &session,
                                                       const WireGuardProfileInput &input) const;
    [[nodiscard]] Result<WireGuardProfile> update(const OwnerSession &session, const ObjectId &id,
                                                   std::string_view expected_revision,
                                                   const WireGuardProfileInput &input) const;
    [[nodiscard]] Result<bool> erase(const OwnerSession &session, const ObjectId &id,
                                     std::string_view expected_revision,
                                     const WireGuardReferenceCount &reference_count) const;
    [[nodiscard]] Result<bool> eraseUnreadable(const OwnerSession &session, const ObjectId &id,
                                               const WireGuardReferenceCount &reference_count) const;
    [[nodiscard]] Result<std::string> exportText(const OwnerSession &session, const ObjectId &id) const;

    [[nodiscard]] const ObjectCollection &collection() const noexcept { return this->collection_; }

private:
    explicit WireGuardProfiles(ObjectCollection collection) : collection_(std::move(collection)) {}

    ObjectCollection collection_;
};

} // namespace aowis::config
