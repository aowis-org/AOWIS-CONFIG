#include "aowis/config/wireguard_profiles.h"

#include "aowis/config/json.h"
#include "json_internal.h"
#include "json_string_internal.h"

#include <algorithm>
#include <utility>

namespace aowis::config {
namespace {
constexpr std::size_t max_name_bytes = 256;
constexpr std::size_t max_description_bytes = 4096;

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

Result<bool> validateMetadata(const WireGuardProfileInput &input)
{
    if (input.name.empty() || trim(input.name).empty() || input.name.size() > max_name_bytes
        || input.name.find('\n') != std::string::npos || input.name.find('\r') != std::string::npos
        || input.name.find('\0') != std::string::npos || input.description.size() > max_description_bytes
        || input.description.find('\0') != std::string::npos) {
        return Result<bool>::failure({ErrorCode::invalid_wireguard_profile, "Invalid profile name or description"});
    }
    // Reuse the core UTF-8 validator without exposing private text in errors.
    const std::string metadata = "{\"name\":" + detail::quoteJsonString(input.name)
        + ",\"description\":" + detail::quoteJsonString(input.description) + "}";
    Result<bool> valid = validateJsonObject(metadata);
    if (!valid) {
        return Result<bool>::failure({ErrorCode::invalid_wireguard_profile, "Profile metadata must be valid UTF-8"});
    }
    return Result<bool>::success(true);
}

// Text integrity only. WireGuard syntax and runtime support are judged by
// the tunnel engine (AOWIS-NETWORK parseWireGuardConf()), never here: one
// parser, one answer.
Result<bool> validConfigurationText(std::string_view text)
{
    if (text.empty() || text.size() > max_wireguard_configuration_bytes) {
        return Result<bool>::failure({ErrorCode::invalid_wireguard_profile,
                                      "WireGuard configuration is empty or exceeds the 2 MiB size limit"});
    }
    if (text.find('\0') != std::string_view::npos
        || !validateJsonObject("{\"text\":" + detail::quoteJsonString(text) + "}")) {
        return Result<bool>::failure({ErrorCode::invalid_wireguard_profile,
                                      "WireGuard configuration must be valid UTF-8 text without NUL"});
    }
    return Result<bool>::success(true);
}

Result<bool> validInput(const WireGuardProfileInput &input)
{
    Result<bool> metadata = validateMetadata(input);
    if (!metadata) return metadata;
    return validConfigurationText(input.configuration);
}

std::string encodeProfile(const WireGuardProfileInput &input)
{
    return "{\"type\":\"wireguard-profile\",\"profile_version\":1,\"name\":"
        + detail::quoteJsonString(input.name) + ",\"description\":"
        + detail::quoteJsonString(input.description) + ",\"configuration\":"
        + detail::quoteJsonString(input.configuration) + "}";
}

Result<WireGuardProfile> decodeProfile(const StoredObject &record)
{
    Result<std::vector<detail::JsonMember>> parsed = detail::parseJsonObject(record.json);
    if (!parsed) {
        return Result<WireGuardProfile>::failure({ErrorCode::corrupt_object, "WireGuard profile JSON is invalid"});
    }
    std::string_view type;
    std::string_view version;
    std::string_view name;
    std::string_view description;
    std::string_view configuration;
    for (const detail::JsonMember &member : parsed.value()) {
        std::string_view *field = nullptr;
        if (member.key == "type") field = &type;
        else if (member.key == "profile_version") field = &version;
        else if (member.key == "name") field = &name;
        else if (member.key == "description") field = &description;
        else if (member.key == "configuration") field = &configuration;
        if (field == nullptr || !field->empty()) {
            return Result<WireGuardProfile>::failure({ErrorCode::corrupt_object, "WireGuard profile fields are invalid or duplicated"});
        }
        *field = member.value;
    }
    if (type != "\"wireguard-profile\"") {
        return Result<WireGuardProfile>::failure({ErrorCode::corrupt_object, "Object is not a WireGuard profile"});
    }
    if (version != "1") {
        return Result<WireGuardProfile>::failure({ErrorCode::unsupported_schema, "Unsupported WireGuard profile version"});
    }
    Result<std::string> decoded_name = detail::unquoteJsonString(name);
    Result<std::string> decoded_description = detail::unquoteJsonString(description);
    Result<std::string> decoded_configuration = detail::unquoteJsonString(configuration);
    if (!decoded_name || !decoded_description || !decoded_configuration) {
        return Result<WireGuardProfile>::failure({ErrorCode::corrupt_object, "WireGuard profile has invalid string fields"});
    }
    // Reads never re-check input rules: tightening them in a later release
    // must not make stored profiles unreadable, or the user could not open
    // them to repair them.
    return Result<WireGuardProfile>::success({{record.info, std::move(decoded_name.value()),
                                               std::move(decoded_description.value())},
                                               std::move(decoded_configuration.value())});
}
Result<bool> checkedReferences(const OwnerSession &session, const ObjectId &id,
                               const WireGuardReferenceCount &reference_count)
{
    if (!reference_count) {
        return Result<bool>::failure({ErrorCode::reference_unavailable,
                                      "WireGuard profile deletion requires a configured-service reference check"});
    }
    Result<std::size_t> count = reference_count(session, id);
    if (!count) return Result<bool>::failure(count.error());
    if (count.value() != 0) {
        return Result<bool>::failure({ErrorCode::profile_in_use, "WireGuard profile is used by configured services"});
    }
    return Result<bool>::success(true);
}
} // namespace

Result<WireGuardProfiles> WireGuardProfiles::open(const OwnerStore &store)
{
    Result<ObjectCollection> collection = store.collection("wireguard");
    if (!collection) return Result<WireGuardProfiles>::failure(collection.error());
    return Result<WireGuardProfiles>::success(WireGuardProfiles(std::move(collection.value())));
}

// Session versions: the single implementation of every operation.

Result<WireGuardProfileListing> WireGuardProfiles::list(const OwnerSession &session) const
{
    Result<ObjectSnapshot> snapshot = this->collection_.readAll(session);
    if (!snapshot) return Result<WireGuardProfileListing>::failure(snapshot.error());
    WireGuardProfileListing listing;
    listing.unreadable = std::move(snapshot.value().unreadable);
    listing.profiles.reserve(snapshot.value().objects.size());
    for (const StoredObject &record : snapshot.value().objects) {
        Result<WireGuardProfile> profile = decodeProfile(record);
        if (!profile) {
            listing.unreadable.push_back({record.info.id.value() + ".json", record.info.id, profile.error()});
            continue;
        }
        listing.profiles.push_back(std::move(profile.value().summary));
    }
    std::sort(listing.unreadable.begin(), listing.unreadable.end(),
              [](const UnreadableObject &left, const UnreadableObject &right) {
        return left.filename < right.filename;
    });
    return Result<WireGuardProfileListing>::success(std::move(listing));
}

Result<WireGuardProfile> WireGuardProfiles::read(const OwnerSession &session, const ObjectId &id) const
{
    Result<StoredObject> stored = this->collection_.read(session, id);
    if (!stored) return Result<WireGuardProfile>::failure(stored.error());
    return decodeProfile(stored.value());
}

Result<WireGuardProfile> WireGuardProfiles::importText(const OwnerSession &session,
                                                        const WireGuardProfileInput &input) const
{
    Result<bool> validated = validInput(input);
    if (!validated) return Result<WireGuardProfile>::failure(validated.error());
    Result<StoredObject> stored = this->collection_.create(session, encodeProfile(input));
    if (!stored) return Result<WireGuardProfile>::failure(stored.error());
    return decodeProfile(stored.value());
}

Result<WireGuardProfile> WireGuardProfiles::update(const OwnerSession &session, const ObjectId &id,
                                                    std::string_view expected_revision,
                                                    const WireGuardProfileInput &input) const
{
    Result<bool> validated = validInput(input);
    if (!validated) return Result<WireGuardProfile>::failure(validated.error());
    // Reject a record which is not a correctly typed profile before update.
    Result<WireGuardProfile> existing = this->read(session, id);
    if (!existing) return existing;
    Result<StoredObject> stored = this->collection_.update(session, id, encodeProfile(input), expected_revision);
    if (!stored) return Result<WireGuardProfile>::failure(stored.error());
    return decodeProfile(stored.value());
}

Result<bool> WireGuardProfiles::erase(const OwnerSession &session, const ObjectId &id,
                                      std::string_view expected_revision,
                                      const WireGuardReferenceCount &reference_count) const
{
    Result<WireGuardProfile> existing = this->read(session, id);
    if (!existing) return Result<bool>::failure(existing.error());
    if (existing.value().summary.info.revision != expected_revision) {
        return Result<bool>::failure({ErrorCode::conflict, "WireGuard profile changed since it was read"});
    }
    // Same session as the delete: no reference can appear in between.
    Result<bool> unreferenced = checkedReferences(session, id, reference_count);
    if (!unreferenced) return unreferenced;
    return this->collection_.erase(session, id, expected_revision);
}

Result<bool> WireGuardProfiles::eraseUnreadable(const OwnerSession &session, const ObjectId &id,
                                                const WireGuardReferenceCount &reference_count) const
{
    Result<StoredObject> stored = this->collection_.read(session, id);
    if (!stored) {
        if (stored.error().code != ErrorCode::corrupt_object) return Result<bool>::failure(stored.error());
        Result<bool> unreferenced = checkedReferences(session, id, reference_count);
        if (!unreferenced) return unreferenced;
        return this->collection_.eraseCorrupt(session, id);
    }
    Result<WireGuardProfile> decoded = decodeProfile(stored.value());
    if (decoded) {
        return Result<bool>::failure({ErrorCode::conflict, "WireGuard profile is readable; erase it with its revision"});
    }
    // A newer profile_version belongs to a newer AOWIS release: never delete it.
    if (decoded.error().code != ErrorCode::corrupt_object) return Result<bool>::failure(decoded.error());
    Result<bool> unreferenced = checkedReferences(session, id, reference_count);
    if (!unreferenced) return unreferenced;
    return this->collection_.erase(session, id, stored.value().info.revision);
}

Result<std::string> WireGuardProfiles::exportText(const OwnerSession &session, const ObjectId &id) const
{
    Result<WireGuardProfile> profile = this->read(session, id);
    if (!profile) return Result<std::string>::failure(profile.error());
    return Result<std::string>::success(std::move(profile.value().configuration));
}

// Session-less versions: one short owner session each.

namespace {
template<typename T, typename Operation>
Result<T> inOwnSession(const ObjectCollection &collection, Operation operation)
{
    Result<OwnerSession> session = collection.store().lock();
    if (!session) return Result<T>::failure(session.error());
    return operation(session.value());
}
} // namespace

Result<WireGuardProfileListing> WireGuardProfiles::list() const
{
    return inOwnSession<WireGuardProfileListing>(this->collection_, [this](const OwnerSession &session) {
        return this->list(session);
    });
}

Result<WireGuardProfile> WireGuardProfiles::read(const ObjectId &id) const
{
    return inOwnSession<WireGuardProfile>(this->collection_, [this, &id](const OwnerSession &session) {
        return this->read(session, id);
    });
}

Result<WireGuardProfile> WireGuardProfiles::importText(const WireGuardProfileInput &input) const
{
    return inOwnSession<WireGuardProfile>(this->collection_, [this, &input](const OwnerSession &session) {
        return this->importText(session, input);
    });
}

Result<WireGuardProfile> WireGuardProfiles::update(const ObjectId &id, std::string_view expected_revision,
                                                    const WireGuardProfileInput &input) const
{
    return inOwnSession<WireGuardProfile>(this->collection_,
                                          [this, &id, expected_revision, &input](const OwnerSession &session) {
        return this->update(session, id, expected_revision, input);
    });
}

Result<bool> WireGuardProfiles::erase(const ObjectId &id, std::string_view expected_revision,
                                      const WireGuardReferenceCount &reference_count) const
{
    return inOwnSession<bool>(this->collection_,
                              [this, &id, expected_revision, &reference_count](const OwnerSession &session) {
        return this->erase(session, id, expected_revision, reference_count);
    });
}

Result<bool> WireGuardProfiles::eraseUnreadable(const ObjectId &id,
                                                const WireGuardReferenceCount &reference_count) const
{
    return inOwnSession<bool>(this->collection_, [this, &id, &reference_count](const OwnerSession &session) {
        return this->eraseUnreadable(session, id, reference_count);
    });
}

Result<std::string> WireGuardProfiles::exportText(const ObjectId &id) const
{
    return inOwnSession<std::string>(this->collection_, [this, &id](const OwnerSession &session) {
        return this->exportText(session, id);
    });
}

} // namespace aowis::config
