#include "gltf/json_value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dtglb::json {
namespace {

struct Parser {
    std::string_view s;
    std::size_t p = 0;
    std::string& error;

    bool fail(const char* what) {
        error = std::string(what) + " at character " + std::to_string(p);
        return false;
    }
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p; }
    bool literal(std::string_view word) {
        if (s.substr(p, word.size()) != word) return false;
        p += word.size();
        return true;
    }

    static void utf8(std::string& out, unsigned code) {
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3f)); }
        else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    bool hex4(unsigned& code) {
        if (p + 4 > s.size()) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[p++];
            code = code * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99);
            if (code >= 0x10000 * 16) return false;
        }
        return code < 0x10000;
    }
    bool string(std::string& out) {
        if (p >= s.size() || s[p] != '"') return fail("expected a string");
        ++p;
        while (p < s.size() && s[p] != '"') {
            const char c = s[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) break;
            const char e = s[p++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code;
                if (!hex4(code)) return fail("bad \\u escape");
                if (code >= 0xd800 && code < 0xdc00 && s.substr(p, 2) == "\\u") {
                    p += 2;
                    unsigned low;
                    if (!hex4(low) || low < 0xdc00 || low >= 0xe000) return fail("bad surrogate pair");
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, code);
                break;
            }
            default: return fail("bad escape");
            }
        }
        if (p >= s.size()) return fail("unterminated string");
        ++p;
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > 256) return fail("nesting too deep");
        ws();
        if (p >= s.size()) return fail("unexpected end");
        const char c = s[p];
        if (c == '{') {
            out = Value::object();
            ++p; ws();
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string name;
                if (!string(name)) return false;
                ws();
                if (p >= s.size() || s[p++] != ':') return fail("expected ':'");
                Value member;
                if (!value(member, depth + 1)) return false;
                out.set(std::move(name), std::move(member));
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out = Value::array();
            ++p; ws();
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            for (;;) {
                Value item;
                if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item));
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { out = Value::of(std::string()); return string(out.string); }
        if (literal("true")) { out = Value::of(true); return true; }
        if (literal("false")) { out = Value::of(false); return true; }
        if (literal("null")) { out = Value(); return true; }
        const std::size_t start = p;
        if (p < s.size() && s[p] == '-') ++p;
        while (p < s.size() && ((s[p] >= '0' && s[p] <= '9') || s[p] == '.' || s[p] == 'e' || s[p] == 'E' ||
                                s[p] == '+' || s[p] == '-')) ++p;
        if (p == start) return fail("unexpected character");
        const std::string text(s.substr(start, p - start));
        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) return fail("bad number");
        out = Value::of(number);
        return true;
    }
};

void write_string(std::string& out, const std::string& text) {
    out += '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20) { char buffer[8]; std::snprintf(buffer, sizeof buffer, "\\u%04x", c); out += buffer; }
        else out += static_cast<char>(c);
    }
    out += '"';
}

void write_number(std::string& out, double number) {
    char buffer[40];
    if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 1e15) {
        std::snprintf(buffer, sizeof buffer, "%.0f", number);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.9g", number);
    }
    out += buffer;
}

bool flat(const Value& value) {
    for (const auto& item : value.items)
        if (item.is_array() || item.is_object()) return false;
    return true;
}

void write(std::string& out, const Value& value, int indent, int level) {
    const auto newline = [&](int l) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(l * indent), ' ');
    };
    switch (value.kind) {
    case Value::Kind::Null: out += "null"; break;
    case Value::Kind::Bool: out += value.boolean ? "true" : "false"; break;
    case Value::Kind::Number: write_number(out, value.number); break;
    case Value::Kind::String: write_string(out, value.string); break;
    case Value::Kind::Array: {
        out += '[';
        const bool one_line = flat(value);
        for (std::size_t i = 0; i < value.items.size(); ++i) {
            if (i) out += one_line ? ", " : ",";
            if (!one_line) newline(level + 1);
            write(out, value.items[i], indent, level + 1);
        }
        if (!one_line && !value.items.empty()) newline(level);
        out += ']';
        break;
    }
    case Value::Kind::Object: {
        out += '{';
        for (std::size_t i = 0; i < value.members.size(); ++i) {
            if (i) out += ',';
            newline(level + 1);
            write_string(out, value.members[i].first);
            out += indent > 0 ? ": " : ":";
            write(out, value.members[i].second, indent, level + 1);
        }
        if (!value.members.empty()) newline(level);
        out += '}';
        break;
    }
    }
}

}

const Value* Value::find(std::string_view name) const {
    for (const auto& member : members)
        if (member.first == name) return &member.second;
    return nullptr;
}

Value& Value::set(std::string name, Value value) {
    for (auto& member : members)
        if (member.first == name) { member.second = std::move(value); return member.second; }
    members.emplace_back(std::move(name), std::move(value));
    return members.back().second;
}

bool parse(std::string_view text, Value& out, std::string& error) {
    Parser parser{text, 0, error};
    if (!parser.value(out, 0)) return false;
    parser.ws();
    if (parser.p != text.size()) return parser.fail("trailing characters");
    return true;
}

std::string dump(const Value& value, int indent) {
    std::string out;
    write(out, value, indent, 0);
    return out;
}

}
