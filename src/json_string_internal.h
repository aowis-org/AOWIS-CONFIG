#pragma once

#include "aowis/config/result.h"

#include <string>
#include <string_view>

namespace aowis::config::detail {

// Escape/decode one JSON string value. Callers validate the surrounding
// object and apply their own field/schema checks.
[[nodiscard]] std::string quoteJsonString(std::string_view text);
[[nodiscard]] Result<std::string> unquoteJsonString(std::string_view json);

} // namespace aowis::config::detail
