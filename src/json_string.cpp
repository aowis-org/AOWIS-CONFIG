#include "json_string_internal.h"

#include <cstdint>

namespace aowis::config::detail {
namespace {
int hexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool codeUnit(std::string_view text, std::size_t &position, std::uint32_t &unit)
{
    if (position + 4 > text.size()) return false;
    unit = 0;
    for (unsigned offset = 0; offset < 4; ++offset) {
        const int digit = hexDigit(text[position++]);
        if (digit < 0) return false;
        unit = (unit << 4U) | static_cast<unsigned>(digit);
    }
    return true;
}

void appendUtf8(std::string &out, std::uint32_t point)
{
    if (point < 0x80) {
        out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (point >> 6)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
}
} // namespace

std::string quoteJsonString(std::string_view text)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('"');
    for (const unsigned char character : text) {
        switch (character) {
        case '"': quoted += "\\\""; break;
        case '\\': quoted += "\\\\"; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\t': quoted += "\\t"; break;
        default:
            if (character < 0x20) {
                quoted += "\\u00";
                quoted.push_back(digits[character >> 4]);
                quoted.push_back(digits[character & 0x0f]);
            } else {
                quoted.push_back(static_cast<char>(character));
            }
        }
    }
    quoted.push_back('"');
    return quoted;
}

Result<std::string> unquoteJsonString(std::string_view json)
{
    const Error invalid{ErrorCode::corrupt_object, "Profile contains an invalid JSON string"};
    if (json.size() < 2 || json.front() != '"' || json.back() != '"') {
        return Result<std::string>::failure(invalid);
    }
    std::string output;
    output.reserve(json.size() - 2);
    for (std::size_t pos = 1; pos < json.size() - 1; ++pos) {
        const unsigned char current = static_cast<unsigned char>(json[pos]);
        if (current < 0x20 || current == '"') return Result<std::string>::failure(invalid);
        if (current != '\\') {
            output.push_back(static_cast<char>(current));
            continue;
        }
        if (++pos == json.size() - 1) return Result<std::string>::failure(invalid);
        const char escaped = json[pos];
        switch (escaped) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u': {
            std::uint32_t code = 0;
            ++pos;
            if (!codeUnit(json, pos, code)) return Result<std::string>::failure(invalid);
            if (code >= 0xdc00 && code <= 0xdfff) return Result<std::string>::failure(invalid);
            if (code >= 0xd800 && code <= 0xdbff) {
                if (pos + 6 > json.size() - 1 || json[pos] != '\\' || json[pos + 1] != 'u') {
                    return Result<std::string>::failure(invalid);
                }
                pos += 2;
                std::uint32_t low = 0;
                if (!codeUnit(json, pos, low) || low < 0xdc00 || low > 0xdfff) {
                    return Result<std::string>::failure(invalid);
                }
                code = 0x10000 + ((code - 0xd800) << 10U) + (low - 0xdc00);
            }
            appendUtf8(output, code);
            --pos;
            break;
        }
        default: return Result<std::string>::failure(invalid);
        }
    }
    return Result<std::string>::success(std::move(output));
}

} // namespace aowis::config::detail
