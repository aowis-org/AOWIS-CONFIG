#pragma once

#include "aowis/config/object_store.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aowis::config {

// The validator checks the portable INI-like structure, not whether the
// running AOWIS-NETWORK backend supports each directive or can reach peers.
enum class WireGuardIssueSeverity { warning, error };
enum class WireGuardIssueCode {
    empty_configuration,
    invalid_encoding,
    invalid_line,
    missing_interface,
    duplicate_interface,
    unexpected_section,
    directive_outside_section,
    empty_value,
    unknown_directive,
    requires_wg_quick,
    external_command,
    external_reference,
    missing_private_key,
    missing_peer_public_key,
};

struct WireGuardIssue {
    WireGuardIssueSeverity severity;
    WireGuardIssueCode code;
    std::size_t line; // 1-based line number, or zero for whole-file issues.
    std::string message; // Never embeds configuration text or key material.
};

struct WireGuardValidation {
    std::vector<WireGuardIssue> issues;
    bool has_errors = false; // Includes errors after the issue-list cap.
    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] WireGuardValidation validateWireGuardConfiguration(std::string_view text);

struct WireGuardProfileInput {
    std::string name;
    std::string description;
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
    std::vector<WireGuardIssue> issues; // Warnings for runtime support and portability.
};

// The owning application must query its authoritative configured-service
// catalogue. A missing/failed resolver blocks deletion; a nonzero count
// blocks deletion. Cross-collection catalog edits are NOT atomic with erase.
using WireGuardReferenceCount = std::function<Result<std::size_t>(const ObjectId &)>;

class WireGuardProfiles final {
public:
    [[nodiscard]] static Result<WireGuardProfiles> open(const ConfigRoot &root);

    [[nodiscard]] Result<std::vector<WireGuardProfileSummary>> list() const;
    [[nodiscard]] Result<WireGuardProfile> read(const ObjectId &id) const;
    [[nodiscard]] Result<WireGuardProfile> importText(const WireGuardProfileInput &input) const;
    [[nodiscard]] Result<WireGuardProfile> update(const ObjectId &id,
                                                   std::string_view expected_revision,
                                                   const WireGuardProfileInput &input) const;
    [[nodiscard]] Result<bool> erase(const ObjectId &id,
                                     std::string_view expected_revision,
                                     const WireGuardReferenceCount &reference_count) const;
    [[nodiscard]] Result<std::string> exportText(const ObjectId &id) const;

private:
    explicit WireGuardProfiles(ObjectCollection collection)
        : collection_(std::move(collection)) {}

    ObjectCollection collection_;
};

} // namespace aowis::config
