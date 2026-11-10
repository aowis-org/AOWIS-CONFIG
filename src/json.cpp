#include "aowis/config/json.h"
#include "json_internal.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace aowis::config::detail {
namespace {
constexpr unsigned max_depth = 64;
constexpr std::size_t max_json_bytes = 8U * 1024U * 1024U;

bool validUtf8(std::string_view input)
{
    for (std::size_t pos = 0; pos < input.size();) {
        const unsigned char initial = static_cast<unsigned char>(input[pos]);
        if (initial < 0x80) {
            ++pos;
            continue;
        }
        unsigned length = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (initial >= 0xc2 && initial <= 0xdf) {
            length = 2; value = initial & 0x1f; minimum = 0x80;
        } else if (initial >= 0xe0 && initial <= 0xef) {
            length = 3; value = initial & 0x0f; minimum = 0x800;
        } else if (initial >= 0xf0 && initial <= 0xf4) {
            length = 4; value = initial & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (pos + length > input.size()) {
            return false;
        }
        for (unsigned index = 1; index < length; ++index) {
            const unsigned char subsequent = static_cast<unsigned char>(input[pos + index]);
            if ((subsequent & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6U) | (subsequent & 0x3fU);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
        pos += length;
    }
    return true;
}

bool digit(char value) { return value >= '0' && value <= '9'; }
int hexValue(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

class Parser final {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    bool document(std::vector<JsonMember> &members)
    {
        this->whitespace();
        if (!this->object(0, &members)) return false;
        this->whitespace();
        return this->position_ == this->input_.size();
    }

private:
    void whitespace()
    {
        while (this->position_ < this->input_.size()) {
            const char value = this->input_[this->position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++this->position_;
        }
    }
    bool consume(char expected)
    {
        if (this->position_ < this->input_.size() && this->input_[this->position_] == expected) {
            ++this->position_;
            return true;
        }
        return false;
    }
    bool fourHex(std::uint16_t &result)
    {
        if (this->input_.size() - this->position_ < 4) return false;
        result = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const int next = hexValue(this->input_[this->position_++]);
            if (next < 0) return false;
            result = static_cast<std::uint16_t>((result << 4U) | next);
        }
        return true;
    }
    bool string(std::string_view *raw = nullptr)
    {
        if (!this->consume('"')) return false;
        const std::size_t start = this->position_;
        while (this->position_ < this->input_.size()) {
            const unsigned char value = static_cast<unsigned char>(this->input_[this->position_++]);
            if (value == '"') {
                if (raw != nullptr) *raw = this->input_.substr(start, this->position_ - start - 1);
                return true;
            }
            if (value < 0x20) return false;
            if (value != '\\') continue;
            if (this->position_ == this->input_.size()) return false;
            const char escaped = this->input_[this->position_++];
            if (escaped == 'u') {
                std::uint16_t point = 0;
                if (!this->fourHex(point)) return false;
                if (point >= 0xdc00 && point <= 0xdfff) return false;
                if (point >= 0xd800 && point <= 0xdbff) {
                    if (!this->consume('\\') || !this->consume('u')) return false;
                    std::uint16_t low = 0;
                    if (!this->fourHex(low) || low < 0xdc00 || low > 0xdfff) return false;
                }
            } else if (escaped != '"' && escaped != '\\' && escaped != '/'
                       && escaped != 'b' && escaped != 'f' && escaped != 'n'
                       && escaped != 'r' && escaped != 't') {
                return false;
            }
        }
        return false;
    }
    bool literal(std::string_view value)
    {
        if (this->input_.substr(this->position_, value.size()) != value) return false;
        this->position_ += value.size();
        return true;
    }
    bool number()
    {
        this->consume('-');
        if (this->consume('0')) {
            if (this->position_ < this->input_.size() && digit(this->input_[this->position_])) return false;
        } else {
            if (this->position_ == this->input_.size() || this->input_[this->position_] < '1'
                || this->input_[this->position_] > '9') return false;
            do { ++this->position_; } while (this->position_ < this->input_.size()
                                             && digit(this->input_[this->position_]));
        }
        if (this->consume('.')) {
            if (this->position_ == this->input_.size() || !digit(this->input_[this->position_])) return false;
            do { ++this->position_; } while (this->position_ < this->input_.size()
                                             && digit(this->input_[this->position_]));
        }
        if (this->consume('e') || this->consume('E')) {
            if (!this->consume('+')) this->consume('-');
            if (this->position_ == this->input_.size() || !digit(this->input_[this->position_])) return false;
            do { ++this->position_; } while (this->position_ < this->input_.size()
                                             && digit(this->input_[this->position_]));
        }
        return true;
    }
    bool value(unsigned depth)
    {
        if (depth > max_depth || this->position_ == this->input_.size()) return false;
        const char first = this->input_[this->position_];
        if (first == '{') return this->object(depth, nullptr);
        if (first == '[') return this->array(depth);
        if (first == '"') return this->string();
        if (first == 't') return this->literal("true");
        if (first == 'f') return this->literal("false");
        if (first == 'n') return this->literal("null");
        return this->number();
    }
    bool object(unsigned depth, std::vector<JsonMember> *members)
    {
        if (depth > max_depth || !this->consume('{')) return false;
        this->whitespace();
        if (this->consume('}')) return true;
        while (true) {
            std::string_view key;
            if (!this->string(&key)) return false;
            this->whitespace();
            if (!this->consume(':')) return false;
            this->whitespace();
            const std::size_t start = this->position_;
            if (!this->value(depth + 1)) return false;
            if (members != nullptr) members->push_back({key, this->input_.substr(start, this->position_ - start)});
            this->whitespace();
            if (this->consume('}')) return true;
            if (!this->consume(',')) return false;
            this->whitespace();
        }
    }
    bool array(unsigned depth)
    {
        if (depth > max_depth || !this->consume('[')) return false;
        this->whitespace();
        if (this->consume(']')) return true;
        while (true) {
            if (!this->value(depth + 1)) return false;
            this->whitespace();
            if (this->consume(']')) return true;
            if (!this->consume(',')) return false;
            this->whitespace();
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
};
} // namespace

Result<std::vector<JsonMember>> parseJsonObject(std::string_view text)
{
    if (text.size() > max_json_bytes) {
        return Result<std::vector<JsonMember>>::failure(
            {ErrorCode::object_too_large, "JSON record exceeds the configured size limit"});
    }
    if (!validUtf8(text)) {
        return Result<std::vector<JsonMember>>::failure({ErrorCode::invalid_json, "JSON is not valid UTF-8"});
    }
    Parser parser(text);
    std::vector<JsonMember> members;
    if (!parser.document(members)) {
        return Result<std::vector<JsonMember>>::failure({ErrorCode::invalid_json, "Invalid JSON object"});
    }
    return Result<std::vector<JsonMember>>::success(std::move(members));
}

} // namespace aowis::config::detail

namespace aowis::config {
Result<bool> validateJsonObject(std::string_view json)
{
    Result<std::vector<detail::JsonMember>> parsed = detail::parseJsonObject(json);
    if (!parsed) return Result<bool>::failure(parsed.error());
    return Result<bool>::success(true);
}
} // namespace aowis::config
