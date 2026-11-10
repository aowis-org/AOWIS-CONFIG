#pragma once

#include "aowis/config/result.h"

#include <string>
#include <string_view>
#include <utility>

namespace aowis::config {

// Owner IDs are *application/service* names (gui, gui-mobile, server-dvmdb),
// not profile names and not arbitrary filesystem paths.
class OwnerId final {
public:
    [[nodiscard]] static Result<OwnerId> parse(std::string_view text);

    [[nodiscard]] const std::string &value() const noexcept
    {
        return this->value_;
    }

private:
    explicit OwnerId(std::string value) : value_(std::move(value)) {}

    std::string value_;
};

} // namespace aowis::config
