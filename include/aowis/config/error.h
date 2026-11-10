#pragma once

#include <string>

namespace aowis::config {

// Errors contain only contextual labels; never include secret payload text.
enum class ErrorCode {
    invalid_owner_id,
    invalid_root,
    missing_environment,
    unsupported_platform,
    invalid_collection,
    invalid_object_id,
    invalid_json,
    unsupported_schema,
    corrupt_object,
    not_found,
    conflict,
    permission_denied,
    unsafe_path,
    io_failure,
    object_too_large,
    invalid_wireguard_profile,
    profile_in_use,
    reference_unavailable,
    lock_timeout,
    session_mismatch,
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace aowis::config
