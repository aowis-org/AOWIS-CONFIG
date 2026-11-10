#pragma once

#include "aowis/config/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace aowis::config::detail {

struct JsonMember {
    std::string_view key; // Original unescaped name bytes; envelope keys are canonical ASCII.
    std::string_view value; // Original JSON value, including quotes for strings.
};

[[nodiscard]] Result<std::vector<JsonMember>> parseJsonObject(std::string_view text);

} // namespace aowis::config::detail
