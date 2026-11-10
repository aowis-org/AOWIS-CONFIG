#pragma once

#include "aowis/config/error.h"

#include <utility>
#include <variant>

namespace aowis::config {

// A minimal result for value-returning operations. Accessing the wrong
// alternative throws std::bad_variant_access. No I/O or exceptions are
// introduced by simply constructing a result.
template<typename T>
class Result final {
public:
    static Result success(T value)
    {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(Error error)
    {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return this->payload_.index() == 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return this->hasValue();
    }

    [[nodiscard]] const T &value() const &
    {
        return std::get<0>(this->payload_);
    }

    [[nodiscard]] T &value() &
    {
        return std::get<0>(this->payload_);
    }

    [[nodiscard]] T &&value() &&
    {
        return std::get<0>(std::move(this->payload_));
    }

    [[nodiscard]] const Error &error() const
    {
        return std::get<1>(this->payload_);
    }

private:
    template<typename... Args>
    explicit Result(std::in_place_index_t<0> tag, Args &&...args)
        : payload_(tag, std::forward<Args>(args)...)
    {
    }

    template<typename... Args>
    explicit Result(std::in_place_index_t<1> tag, Args &&...args)
        : payload_(tag, std::forward<Args>(args)...)
    {
    }

    std::variant<T, Error> payload_;
};

} // namespace aowis::config
