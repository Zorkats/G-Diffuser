// Copyright (c) Zorkats

#include "gdx_companion_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <utility>

namespace gdx_companion {

namespace {

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void AppendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool ParseDocument(JsonValue& out, std::string& error) {
        SkipSpace();
        if (!ParseValue(out, 0)) {
            error = error_;
            return false;
        }
        SkipSpace();
        if (pos_ != text_.size()) {
            Fail("trailing data after the JSON value");
            error = error_;
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* message) {
        if (error_.empty()) {
            error_ = std::string(message) + " at byte " + std::to_string(pos_);
        }
        return false;
    }

    bool AtEnd() const { return pos_ >= text_.size(); }
    char Peek() const { return text_[pos_]; }

    void SkipSpace() {
        while (!AtEnd()) {
            char c = Peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (AtEnd()) return Fail("unexpected end of input");
        char c = Peek();
        switch (c) {
        case '{':
            return ParseObject(out, depth);
        case '[':
            return ParseArray(out, depth);
        case '"':
            out.kind_ = JsonValue::Kind::String;
            return ParseString(out.string_);
        case 't':
            return ParseLiteral("true", JsonValue::Kind::Bool, true, out);
        case 'f':
            return ParseLiteral("false", JsonValue::Kind::Bool, false, out);
        case 'n':
            return ParseLiteral("null", JsonValue::Kind::Null, false, out);
        default:
            if (c == '-' || IsDigit(c)) return ParseNumber(out);
            return Fail("unexpected character");
        }
    }

    bool ParseLiteral(std::string_view word, JsonValue::Kind kind, bool value, JsonValue& out) {
        if (text_.substr(pos_, word.size()) != word) return Fail("invalid literal");
        pos_ += word.size();
        out.kind_ = kind;
        out.bool_ = value;
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        size_t start = pos_;
        if (Peek() == '-') ++pos_;
        if (AtEnd()) return Fail("invalid number");
        if (Peek() == '0') {
            ++pos_;
        } else if (IsDigit(Peek())) {
            while (!AtEnd() && IsDigit(Peek())) ++pos_;
        } else {
            return Fail("invalid number");
        }
        if (!AtEnd() && Peek() == '.') {
            ++pos_;
            if (AtEnd() || !IsDigit(Peek())) return Fail("invalid number");
            while (!AtEnd() && IsDigit(Peek())) ++pos_;
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) ++pos_;
            if (AtEnd() || !IsDigit(Peek())) return Fail("invalid number");
            while (!AtEnd() && IsDigit(Peek())) ++pos_;
        }
        std::string token(text_.substr(start, pos_ - start));
        out.kind_ = JsonValue::Kind::Number;
        out.number_ = std::strtod(token.c_str(), nullptr);
        return true;
    }

    bool ReadHex4(unsigned int& value) {
        if (pos_ + 4 > text_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            int digit = HexValue(text_[pos_ + i]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<unsigned int>(digit);
        }
        pos_ += 4;
        return true;
    }

    // Caller guarantees Peek() is the opening quote.
    bool ParseString(std::string& out) {
        ++pos_;
        out.clear();
        while (true) {
            if (AtEnd()) return Fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(Peek());
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return Fail("control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (AtEnd()) return Fail("unterminated escape");
            char e = Peek();
            ++pos_;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned int cp = 0;
                if (!ReadHex4(cp)) return Fail("invalid \\u escape");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned int low = 0;
                    if (text_.substr(pos_, 2) != "\\u") return Fail("unpaired surrogate");
                    pos_ += 2;
                    if (!ReadHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return Fail("unpaired surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return Fail("unpaired surrogate");
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                return Fail("invalid escape");
            }
        }
    }

    bool ParseArray(JsonValue& out, int depth) {
        if (depth >= kJsonMaxDepth) return Fail("nesting too deep");
        ++pos_;
        out.kind_ = JsonValue::Kind::Array;
        SkipSpace();
        if (!AtEnd() && Peek() == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipSpace();
            JsonValue child;
            if (!ParseValue(child, depth + 1)) return false;
            out.items_.push_back(std::move(child));
            SkipSpace();
            if (AtEnd()) return Fail("unterminated array");
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        if (depth >= kJsonMaxDepth) return Fail("nesting too deep");
        ++pos_;
        out.kind_ = JsonValue::Kind::Object;
        SkipSpace();
        if (!AtEnd() && Peek() == '}') {
            ++pos_;
            return true;
        }
        std::unordered_set<std::string> seen;
        while (true) {
            SkipSpace();
            if (AtEnd() || Peek() != '"') return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            if (!seen.insert(key).second) return Fail("duplicate object key");
            SkipSpace();
            if (AtEnd() || Peek() != ':') return Fail("expected ':'");
            ++pos_;
            SkipSpace();
            JsonValue child;
            if (!ParseValue(child, depth + 1)) return false;
            out.keys_.push_back(std::move(key));
            out.items_.push_back(std::move(child));
            SkipSpace();
            if (AtEnd()) return Fail("unterminated object");
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == '}') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;
};

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (kind_ != Kind::Object) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

bool JsonValue::AsInt64(int64_t& out) const {
    if (kind_ != Kind::Number) return false;
    if (number_ != std::trunc(number_)) return false;
    // Stay inside the exactly representable range of int64_t.
    if (number_ < -9.0e18 || number_ > 9.0e18) return false;
    out = static_cast<int64_t>(number_);
    return true;
}

bool JsonValue::Parse(std::string_view text, JsonValue& out, std::string& error) {
    out = JsonValue();
    if (!IsValidUtf8(text)) {
        error = "input is not valid UTF-8";
        return false;
    }
    JsonReader reader(text);
    return reader.ParseDocument(out, error);
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
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
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned int>(c));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len = 0;
        unsigned int cp = 0;
        unsigned int min_cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
            min_cp = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
            min_cp = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
            min_cp = 0x10000;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

}  // namespace gdx_companion
