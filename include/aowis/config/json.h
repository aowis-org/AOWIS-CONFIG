#pragma once

#include "aowis/config/result.h"

#include <string_view>

namespace aowis::config {

// Strict RFC 8259 JSON object validation, including UTF-8 and string escapes.
// The config core does not interpret component-owned payload schemas.
[[nodiscard]] Result<bool> validateJsonObject(std::string_view json);

} // namespace aowis::config
