// Copyright (c) Zorkats
//
// Bounded JSON reader for protocol 1 lines. Used only for the few fields the protocol needs.
// Rejects duplicate object keys, invalid UTF-8, control characters inside strings, NaN and
// Infinity, and nesting deeper than kJsonMaxDepth. Callers cap the input size first.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gdx_companion {

constexpr int kJsonMaxDepth = 8;

class JsonReader;

class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind() const { return kind_; }
    bool AsBool() const { return bool_; }
    double AsNumber() const { return number_; }
    const std::string& AsString() const { return string_; }
    const std::vector<JsonValue>& AsArray() const { return items_; }
    // Object keys in document order; empty for other kinds.
    const std::vector<std::string>& Keys() const { return keys_; }

    // Returns nullptr when this is not an object or the key is absent.
    const JsonValue* Find(std::string_view key) const;

    // True when the value is an integral number that fits in int64_t.
    bool AsInt64(int64_t& out) const;

    // Parses one complete JSON document. On failure, returns false and sets error.
    static bool Parse(std::string_view text, JsonValue& out, std::string& error);

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> items_;   // array items, or object values parallel to keys_
    std::vector<std::string> keys_;  // object keys
    friend class JsonReader;
};

// Escapes a string for use between JSON quotes. Output is ASCII-safe for control characters
// and passes valid UTF-8 through unchanged.
std::string JsonEscape(std::string_view text);

// True when the bytes are well-formed UTF-8 (no overlongs, surrogates or code points above
// U+10FFFF).
bool IsValidUtf8(std::string_view text);

}  // namespace gdx_companion
