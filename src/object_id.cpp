#include "aowis/config/object_id.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <random>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace aowis::config {

namespace {
bool fillRandom(std::uint8_t *bytes, std::size_t size)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t count = getrandom(bytes + completed, size - completed, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        completed += static_cast<std::size_t>(count);
    }
    return true;
#else
    try {
        std::random_device entropy;
        for (std::size_t index = 0; index < size; ++index) {
            bytes[index] = static_cast<std::uint8_t>(entropy());
        }
        return true;
    } catch (...) {
        return false;
    }
#endif
}

bool lowercaseHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}
} // namespace

Result<ObjectId> ObjectId::parse(std::string_view text)
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-'
        || text[18] != '-' || text[23] != '-' || text[14] != '7'
        || (text[19] != '8' && text[19] != '9' && text[19] != 'a' && text[19] != 'b')) {
        return Result<ObjectId>::failure({ErrorCode::invalid_object_id, "Expected a lowercase UUIDv7 object ID"});
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (!lowercaseHex(text[index])) {
            return Result<ObjectId>::failure({ErrorCode::invalid_object_id, "Expected a lowercase UUIDv7 object ID"});
        }
    }
    return Result<ObjectId>::success(ObjectId(std::string(text)));
}

Result<ObjectId> ObjectId::generate()
{
    std::array<std::uint8_t, 16> bytes{};
    if (!fillRandom(bytes.data(), bytes.size())) {
        return Result<ObjectId>::failure({ErrorCode::io_failure, "Operating-system random source unavailable"});
    }
    const std::uint64_t milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    for (unsigned index = 0; index < 6; ++index) {
        bytes[index] = static_cast<std::uint8_t>(milliseconds >> ((5U - index) * 8U));
    }
    bytes[6] = static_cast<std::uint8_t>(0x70U | (bytes[6] & 0x0fU));
    bytes[8] = static_cast<std::uint8_t>(0x80U | (bytes[8] & 0x3fU));
    constexpr char hex[] = "0123456789abcdef";
    std::string identifier(36, '-');
    std::size_t position = 0;
    for (std::uint8_t byte : bytes) {
        if (position == 8 || position == 13 || position == 18 || position == 23) {
            ++position;
        }
        identifier[position++] = hex[byte >> 4U];
        identifier[position++] = hex[byte & 0x0fU];
    }
    return ObjectId::parse(identifier);
}

} // namespace aowis::config
