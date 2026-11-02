#include "api/json_io.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "api/types.hpp"

namespace bp::api {

namespace {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::int64_t number = 0;
    std::string str;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue* find(const std::string& key) const {
        for (const auto& member : members) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skipWhitespace();
        JsonValue value = parseValue(0);
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("trailing characters");
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const char* reason) const {
        throw std::invalid_argument(std::string("json: ") + reason);
    }

    void skipWhitespace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    char peek() const {
        if (pos_ >= text_.size()) {
            fail("unexpected end");
        }
        return text_[pos_];
    }

    void expect(char c) {
        if (pos_ >= text_.size() || text_[pos_] != c) {
            fail("unexpected character");
        }
        ++pos_;
    }

    JsonValue parseValue(int depth) {
        if (depth > 128) {
            fail("nesting too deep");
        }
        switch (peek()) {
            case '{':
                return parseObject(depth);
            case '[':
                return parseArray(depth);
            case '"': {
                JsonValue value;
                value.type = JsonValue::Type::String;
                value.str = parseString();
                return value;
            }
            case 't':
                literal("true");
                return makeBool(true);
            case 'f':
                literal("false");
                return makeBool(false);
            case 'n':
                literal("null");
                return JsonValue{};
            default:
                if (peek() == '-' ||
                    std::isdigit(static_cast<unsigned char>(peek()))) {
                    return parseNumber();
                }
                fail("unexpected value");
        }
    }

    JsonValue makeBool(bool boolean) {
        JsonValue value;
        value.type = JsonValue::Type::Bool;
        value.boolean = boolean;
        return value;
    }

    void literal(const char* word) {
        const std::size_t len = std::char_traits<char>::length(word);
        if (text_.substr(pos_, len) != word) {
            fail("bad literal");
        }
        pos_ += len;
    }

    JsonValue parseObject(int depth) {
        JsonValue value;
        value.type = JsonValue::Type::Object;
        expect('{');
        skipWhitespace();
        if (peek() == '}') {
            ++pos_;
            return value;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                fail("expected key");
            }
            std::string key = parseString();
            if (value.find(key) != nullptr) {
                fail("duplicate key");
            }
            skipWhitespace();
            expect(':');
            skipWhitespace();
            value.members.emplace_back(std::move(key), parseValue(depth + 1));
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return value;
        }
    }

    JsonValue parseArray(int depth) {
        JsonValue value;
        value.type = JsonValue::Type::Array;
        expect('[');
        skipWhitespace();
        if (peek() == ']') {
            ++pos_;
            return value;
        }
        while (true) {
            skipWhitespace();
            value.items.push_back(parseValue(depth + 1));
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            return value;
        }
    }

    unsigned parseHex4() {
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = peek();
            ++pos_;
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else fail("bad unicode escape");
        }
        return code;
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (true) {
            const unsigned char c = static_cast<unsigned char>(peek());
            ++pos_;
            if (c == '"') {
                return result;
            }
            if (c < 0x20) {
                fail("unescaped control character");
            }
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            const char esc = peek();
            ++pos_;
            switch (esc) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    unsigned code = parseHex4();
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            fail("missing low surrogate");
                        }
                        pos_ += 2;
                        const unsigned low = parseHex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("bad low surrogate");
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) +
                               (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        fail("unexpected low surrogate");
                    }
                    appendUtf8(result, code);
                    break;
                }
                default:
                    fail("bad escape");
            }
        }
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    JsonValue parseNumber() {
        const std::size_t begin = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (peek() == '0') {
            ++pos_;
            if (pos_ < text_.size() &&
                std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("leading zero");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                fail("bad number");
            }
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() &&
            (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            fail("epoch seconds must be an integer");
        }
        JsonValue value;
        value.type = JsonValue::Type::Number;
        const char* first = text_.data() + begin;
        const char* last = text_.data() + pos_;
        const auto result = std::from_chars(first, last, value.number);
        if (result.ec != std::errc{} || result.ptr != last) {
            fail("integer out of range");
        }
        return value;
    }
};

const JsonValue& require(const JsonValue& obj, const std::string& key,
                         JsonValue::Type type) {
    const JsonValue* value = obj.find(key);
    if (value == nullptr || value->type != type) {
        throw std::invalid_argument("missing or bad field: " + key);
    }
    return *value;
}

void readOptionalString(const JsonValue& obj, const std::string& key,
                        std::string& target) {
    if (const JsonValue* value = obj.find(key)) {
        if (value->type != JsonValue::Type::String) {
            throw std::invalid_argument("bad field: " + key);
        }
        target = value->str;
    }
}

void readOptionalNumber(const JsonValue& obj, const std::string& key,
                        std::optional<std::int64_t>& target) {
    if (const JsonValue* value = obj.find(key)) {
        if (value->type != JsonValue::Type::Number) {
            throw std::invalid_argument("bad field: " + key);
        }
        target = value->number;
    }
}

Event parseOne(const JsonValue& obj) {
    if (obj.type != JsonValue::Type::Object) {
        throw std::invalid_argument("event must be an object");
    }
    const JsonValue& type = require(obj, "type", JsonValue::Type::String);
    if (type.str == "deadline") {
        DeadlineInput input;
        input.when = require(obj, "when", JsonValue::Type::Number).number;
        readOptionalString(obj, "title", input.title);
        readOptionalString(obj, "note", input.note);
        return toEvent(input);
    }
    if (type.str == "schedule") {
        ScheduleInput input;
        input.start = require(obj, "start", JsonValue::Type::Number).number;
        readOptionalNumber(obj, "end", input.end);
        readOptionalNumber(obj, "durationSeconds", input.durationSeconds);
        readOptionalString(obj, "title", input.title);
        readOptionalString(obj, "note", input.note);
        return toEvent(input);
    }
    throw std::invalid_argument("unknown event type: " + type.str);
}

void appendEscaped(std::string& out, const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

}  // namespace

std::vector<Event> parseEventsJson(std::string_view jsonText) {
    const JsonValue root = Parser(jsonText).parse();
    std::vector<Event> events;
    if (root.type == JsonValue::Type::Array) {
        events.reserve(root.items.size());
        for (const auto& item : root.items) {
            events.push_back(parseOne(item));
        }
        return events;
    }
    events.push_back(parseOne(root));
    return events;
}

std::string dumpEventsJson(const std::vector<Event>& events) {
    std::string out = "[";
    bool first = true;
    for (const auto& event : events) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out.push_back('{');
        if (event.kind == Kind::Deadline) {
            out += "\"type\":\"deadline\",\"when\":" +
                   std::to_string(event.start);
        } else {
            out += "\"type\":\"schedule\",\"start\":" +
                   std::to_string(event.start) + ",\"end\":" +
                   std::to_string(event.end);
        }
        out += ",\"title\":";
        appendEscaped(out, event.title);
        out += ",\"note\":";
        appendEscaped(out, event.note);
        out.push_back('}');
    }
    out.push_back(']');
    return out;
}

}  // namespace bp::api
