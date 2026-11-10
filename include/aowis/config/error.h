#pragma once

#include <string>

namespace aowis::config {

// The error type is deliberately independent of Qt, errno, and exceptions.
// File-system and data-format errors will be added when persistence lands.
enum class ErrorCode {
    invalid_owner_id,
    invalid_root,
    missing_environment,
    unsupported_platform,
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace aowis::config
