#pragma once

#include "aowis/config/result.h"

#include <string>
#include <string_view>
#include <utility>

namespace aowis::config {

// Lowercase RFC 9562 UUIDv7; an object ID is never a filesystem path.
class ObjectId final {
public:
    [[nodiscard]] static Result<ObjectId> parse(std::string_view text);
    [[nodiscard]] static Result<ObjectId> generate();

    [[nodiscard]] const std::string &value() const noexcept { return this->value_; }

private:
    explicit ObjectId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

} // namespace aowis::config
