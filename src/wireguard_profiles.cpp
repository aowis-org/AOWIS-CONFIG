#include "aowis/config/wireguard_profiles.h"

#include "aowis/config/json.h"
#include "json_internal.h"
#include "json_string_internal.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace aowis::config {
namespace {
constexpr std::size_t max_config_bytes = 2U * 1024U * 1024U;
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

bool existsIn(std::string_view key, std::initializer_list<std::string_view> names)
{
    return std::find(names.begin(), names.end(), key) != names.end();
}

bool externalReference(std::string_view value)
{
    value = trim(value);
    return value.starts_with('/') || value.starts_with("./") || value.starts_with("../")
        || value.starts_with('~') || value.starts_with("file:") || value.starts_with('@')
        || value.starts_with("$HOME") || value.starts_with("${HOME}");
}

void add(WireGuardValidation &result, WireGuardIssueSeverity severity,
         WireGuardIssueCode code, std::size_t line, const char *message)
{
    if (severity == WireGuardIssueSeverity::error) result.has_errors = true;
    if (result.issues.size() < 512) {
        result.issues.push_back({severity, code, line, message});
    }
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

Result<bool> validInput(const WireGuardProfileInput &input)
{
    Result<bool> metadata = validateMetadata(input);
    if (!metadata) return metadata;
    const WireGuardValidation validation = validateWireGuardConfiguration(input.configuration);
    if (!validation.valid()) {
        return Result<bool>::failure({ErrorCode::invalid_wireguard_profile,
                                      "WireGuard configuration has validation errors; inspect the validation issues"});
    }
    return Result<bool>::success(true);
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
    WireGuardProfileInput input{decoded_name.value(), decoded_description.value(), decoded_configuration.value()};
    if (!validInput(input)) {
        return Result<WireGuardProfile>::failure({ErrorCode::corrupt_object, "WireGuard profile failed validation"});
    }
    WireGuardValidation validation = validateWireGuardConfiguration(decoded_configuration.value());
    return Result<WireGuardProfile>::success({{record.info, std::move(decoded_name.value()),
                                               std::move(decoded_description.value())},
                                               std::move(decoded_configuration.value()),
                                               std::move(validation.issues)});
}
} // namespace

bool WireGuardValidation::valid() const noexcept
{
    return !this->has_errors;
}

WireGuardValidation validateWireGuardConfiguration(std::string_view text)
{
    WireGuardValidation output;
    if (text.empty() || text.size() > max_config_bytes) {
        add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::empty_configuration, 0,
            "WireGuard configuration is empty or exceeds the 2 MiB size limit");
        return output;
    }
    const std::string document = "{\"text\":" + detail::quoteJsonString(text) + "}";
    if (text.find('\0') != std::string_view::npos || !validateJsonObject(document)) {
        add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::invalid_encoding, 0,
            "WireGuard configuration must be valid UTF-8 text");
        return output;
    }

    enum class Section { none, interface, peer };
    Section section = Section::none;
    bool has_interface = false;
    bool interface_key = false;
    bool peer_key = true;
    bool has_peer = false;
    std::size_t line_number = 0;
    std::size_t position = 0;
    while (position < text.size()) {
        ++line_number;
        const std::size_t end = text.find('\n', position);
        const std::size_t limit = end == std::string_view::npos ? text.size() : end;
        std::string_view line = trim(text.substr(position, limit - position));
        position = end == std::string_view::npos ? text.size() : end + 1;
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf")) {
            line.remove_prefix(3); // UTF-8 BOM is retained in stored text.
        }
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.starts_with('[')) {
            if (line == "[Interface]") {
                if (has_interface) {
                    add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::duplicate_interface,
                        line_number, "Only one [Interface] section is supported");
                }
                has_interface = true;
                section = Section::interface;
            } else if (line == "[Peer]") {
                if (has_peer && !peer_key) {
                    add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::missing_peer_public_key,
                        line_number, "Previous [Peer] has no PublicKey");
                }
                has_peer = true;
                peer_key = false;
                section = Section::peer;
            } else {
                section = Section::none;
                add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::unexpected_section,
                    line_number, "Unrecognized WireGuard section");
            }
            continue;
        }
        const std::size_t equal = line.find('=');
        if (equal == std::string_view::npos) {
            add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::invalid_line,
                line_number, "Expected a key = value directive");
            continue;
        }
        const std::string_view key = trim(line.substr(0, equal));
        std::string_view value = trim(line.substr(equal + 1));
        if (section == Section::none) {
            add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::directive_outside_section,
                line_number, "WireGuard directive must be inside [Interface] or [Peer]");
            continue;
        }
        if (key.empty() || value.empty()) {
            add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::empty_value,
                line_number, "Directive key and value must be nonempty");
            continue;
        }
        if (section == Section::interface) {
            if (key == "PrivateKey") interface_key = true;
            if (existsIn(key, {"PreUp", "PostUp", "PreDown", "PostDown"})) {
                add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::external_command,
                    line_number, "External hook commands may not work in embedded clients or after relocation");
            } else if (existsIn(key, {"Address", "DNS", "MTU", "Table", "SaveConfig"})) {
                add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::requires_wg_quick,
                    line_number, "wg-quick directive requires runtime capability verification");
            } else if (!existsIn(key, {"PrivateKey", "ListenPort", "FwMark"})) {
                add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::unknown_directive,
                    line_number, "Unrecognized interface directive; runtime support is not guaranteed");
            }
            if (key == "PrivateKey" && externalReference(value)) {
                add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::external_reference,
                    line_number, "Private key looks like an external file reference and may not be portable");
            }
        } else {
            if (key == "PublicKey") peer_key = true;
            if (!existsIn(key, {"PublicKey", "PresharedKey", "AllowedIPs", "Endpoint", "PersistentKeepalive"})) {
                add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::unknown_directive,
                    line_number, "Unrecognized peer directive; runtime support is not guaranteed");
            }
            if (key == "PresharedKey" && externalReference(value)) {
                add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::external_reference,
                    line_number, "Preshared key looks like an external file reference and may not be portable");
            }
        }
        if (key == "Include" || key == "PrivateKeyFile" || key == "PresharedKeyFile") {
            add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::external_reference,
                line_number, "External include or key-file directives may not be portable");
        }
    }
    if (!has_interface) {
        add(output, WireGuardIssueSeverity::error, WireGuardIssueCode::missing_interface,
            0, "WireGuard configuration must have an [Interface] section");
    } else if (!interface_key) {
        add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::missing_private_key,
            0, "[Interface] has no PrivateKey; the runtime may be unable to start the tunnel");
    }
    if (has_peer && !peer_key) {
        add(output, WireGuardIssueSeverity::warning, WireGuardIssueCode::missing_peer_public_key,
            line_number, "Last [Peer] has no PublicKey");
    }
    return output;
}

Result<WireGuardProfiles> WireGuardProfiles::open(const ConfigRoot &root)
{
    Result<ObjectCollection> collection = ObjectCollection::open(root, "wireguard");
    if (!collection) return Result<WireGuardProfiles>::failure(collection.error());
    return Result<WireGuardProfiles>::success(WireGuardProfiles(std::move(collection.value())));
}

Result<WireGuardProfile> WireGuardProfiles::read(const ObjectId &id) const
{
    Result<StoredObject> stored = this->collection_.read(id);
    if (!stored) return Result<WireGuardProfile>::failure(stored.error());
    return decodeProfile(stored.value());
}

Result<std::vector<WireGuardProfileSummary>> WireGuardProfiles::list() const
{
    Result<std::vector<ObjectInfo>> objects = this->collection_.list();
    if (!objects) return Result<std::vector<WireGuardProfileSummary>>::failure(objects.error());
    std::vector<WireGuardProfileSummary> profiles;
    profiles.reserve(objects.value().size());
    for (const ObjectInfo &info : objects.value()) {
        Result<WireGuardProfile> profile = this->read(info.id);
        if (!profile) return Result<std::vector<WireGuardProfileSummary>>::failure(profile.error());
        profiles.push_back(std::move(profile.value().summary));
    }
    return Result<std::vector<WireGuardProfileSummary>>::success(std::move(profiles));
}

Result<WireGuardProfile> WireGuardProfiles::importText(const WireGuardProfileInput &input) const
{
    Result<bool> validated = validInput(input);
    if (!validated) return Result<WireGuardProfile>::failure(validated.error());
    Result<StoredObject> stored = this->collection_.create(encodeProfile(input));
    if (!stored) return Result<WireGuardProfile>::failure(stored.error());
    return decodeProfile(stored.value());
}

Result<WireGuardProfile> WireGuardProfiles::update(const ObjectId &id,
                                                    std::string_view expected_revision,
                                                    const WireGuardProfileInput &input) const
{
    Result<bool> validated = validInput(input);
    if (!validated) return Result<WireGuardProfile>::failure(validated.error());
    // Reject a record which is not a correctly typed WG profile before update.
    Result<WireGuardProfile> existing = this->read(id);
    if (!existing) return existing;
    Result<StoredObject> stored = this->collection_.update(id, encodeProfile(input), expected_revision);
    if (!stored) return Result<WireGuardProfile>::failure(stored.error());
    return decodeProfile(stored.value());
}

Result<bool> WireGuardProfiles::erase(const ObjectId &id, std::string_view expected_revision,
                                      const WireGuardReferenceCount &reference_count) const
{
    Result<WireGuardProfile> existing = this->read(id);
    if (!existing) return Result<bool>::failure(existing.error());
    if (existing.value().summary.info.revision != expected_revision) {
        return Result<bool>::failure({ErrorCode::conflict, "WireGuard profile changed since it was read"});
    }
    if (!reference_count) {
        return Result<bool>::failure({ErrorCode::reference_unavailable,
                                      "WireGuard profile deletion requires a configured-service reference check"});
    }
    Result<std::size_t> count = reference_count(id);
    if (!count) return Result<bool>::failure(count.error());
    if (count.value() != 0) {
        return Result<bool>::failure({ErrorCode::profile_in_use,
                                      "WireGuard profile is used by configured services"});
    }
    return this->collection_.erase(id, expected_revision);
}

Result<std::string> WireGuardProfiles::exportText(const ObjectId &id) const
{
    Result<WireGuardProfile> profile = this->read(id);
    if (!profile) return Result<std::string>::failure(profile.error());
    return Result<std::string>::success(std::move(profile.value().configuration));
}

} // namespace aowis::config
