#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dtglb::json {

// A parsed JSON document. Object members keep their source order (names are unique).
struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;                              // Array
    std::vector<std::pair<std::string, Value>> members;    // Object

    static Value object() { Value v; v.kind = Kind::Object; return v; }
    static Value array() { Value v; v.kind = Kind::Array; return v; }
    static Value of(double n) { Value v; v.kind = Kind::Number; v.number = n; return v; }
    static Value of(bool b) { Value v; v.kind = Kind::Bool; v.boolean = b; return v; }
    static Value of(std::string s) { Value v; v.kind = Kind::String; v.string = std::move(s); return v; }

    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_string() const { return kind == Kind::String; }
    const Value* find(std::string_view name) const;
    Value& set(std::string name, Value value);   // adds or replaces a member
    Value& push(Value value) { items.push_back(std::move(value)); return items.back(); }
};

bool parse(std::string_view text, Value& out, std::string& error);
// Numbers are written so they parse back to the same float/u32 (up to 9 significant digits).
std::string dump(const Value& value, int indent = 1);

}
