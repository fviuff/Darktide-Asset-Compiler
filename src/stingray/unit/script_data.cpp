#include "stingray/unit/script_data.h"

#include "stingray/murmur_hash.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dtglb::stingray::unit {
namespace {

constexpr std::uint32_t kNone = 0xffffffffu;
constexpr std::uint32_t kBool = 1, kNumber = 2, kString = 3, kTable = 0xffffffffu;

void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}
void set(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) { std::memcpy(out.data() + offset, &value, 4); }
std::uint32_t get(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
}

bool key_id(const std::string& name, std::uint32_t& id) {
    if (!name.empty() && name[0] == '#') return name.size() == 9 && std::sscanf(name.c_str() + 1, "%8x", &id) == 1;
    if (name.empty()) return false;
    id = id32_from_id64(name);
    return true;
}

bool write_value(std::vector<std::uint8_t>& out, std::uint32_t key, const json::Value& value, const std::string& where, std::string& error);

// Writes the children of an object or array; returns the first child's offset (kNone when empty).
bool write_children(std::vector<std::uint8_t>& out, const json::Value& value, const std::string& where,
                    std::uint32_t& first, std::string& error) {
    first = kNone;
    std::size_t previous = 0;
    bool any = false;
    const auto add = [&](std::uint32_t key, const json::Value& child, const std::string& path) {
        const auto offset = out.size();
        if (any) set(out, previous + 4, static_cast<std::uint32_t>(offset)); else first = static_cast<std::uint32_t>(offset);
        any = true;
        previous = offset;
        return write_value(out, key, child, path, error);
    };
    if (value.is_object()) {
        for (const auto& [name, child] : value.members) {
            std::uint32_t key;
            if (!key_id(name, key)) { error = where + ": key '" + name + "' is not a name or #<8 hex>"; return false; }
            if (!add(key, child, where + "." + name)) return false;
        }
    } else {
        for (std::size_t i = 0; i < value.items.size(); ++i)
            if (!add(static_cast<std::uint32_t>(i), value.items[i], where + "[" + std::to_string(i + 1) + "]")) return false;
    }
    return true;
}

bool write_value(std::vector<std::uint8_t>& out, std::uint32_t key, const json::Value& value, const std::string& where, std::string& error) {
    const auto entry = out.size();
    put(out, key);
    put(out, kNone);
    switch (value.kind) {
        case json::Value::Kind::Bool:
            put(out, kBool); put(out, 4); put(out, value.boolean ? 1u : 0u);
            return true;
        case json::Value::Kind::Number: {
            const float number = static_cast<float>(value.number);
            if (!std::isfinite(number)) { error = where + ": number out of range"; return false; }
            std::uint32_t bits; std::memcpy(&bits, &number, 4);
            put(out, kNumber); put(out, 4); put(out, bits);
            return true;
        }
        case json::Value::Kind::String: {
            if (value.string.find('\0') != std::string::npos) { error = where + ": text holds a NUL"; return false; }
            const auto size = (value.string.size() + 1 + 3) & ~std::size_t{3};
            put(out, kString); put(out, static_cast<std::uint32_t>(size));
            out.insert(out.end(), value.string.begin(), value.string.end());
            out.resize(out.size() + size - value.string.size(), 0);
            return true;
        }
        case json::Value::Kind::Object:
        case json::Value::Kind::Array: {
            put(out, kTable); put(out, 4); put(out, kNone);
            std::uint32_t first;
            if (!write_children(out, value, where, first, error)) return false;
            set(out, entry + 16, first);
            return true;
        }
        case json::Value::Kind::Null:
            break;
    }
    error = where + ": null is not storable";
    return false;
}

bool read_entries(const std::vector<std::uint8_t>& bytes, std::uint32_t offset, json::Value& out, int depth, std::string& error) {
    out = json::Value::object();
    for (int guard = 0; offset != kNone; ++guard) {
        if (depth > 64 || guard > 1000000 || offset > bytes.size() || bytes.size() - offset < 16) { error = "script data entry out of range"; return false; }
        const auto key = get(bytes, offset), next = get(bytes, offset + 4), type = get(bytes, offset + 8), size = get(bytes, offset + 12);
        if (bytes.size() - offset - 16 < size) { error = "script data value out of range"; return false; }
        char name[12]; std::snprintf(name, sizeof(name), "#%08x", key);
        json::Value value;
        if (type == kTable) {
            if (size != 4 || !read_entries(bytes, get(bytes, offset + 16), value, depth + 1, error)) {
                if (error.empty()) error = "script data table entry is malformed";
                return false;
            }
        } else if (type == kBool && size == 4) {
            value = json::Value::of(get(bytes, offset + 16) != 0);
        } else if (type == kNumber && size == 4) {
            float number; std::memcpy(&number, bytes.data() + offset + 16, 4);
            value = json::Value::of(static_cast<double>(number));
        } else if (type == kString && size > 0) {
            const auto* text = reinterpret_cast<const char*>(bytes.data() + offset + 16);
            value = json::Value::of(std::string(text, strnlen(text, size)));
        } else {
            error = "script data entry has an unknown type";
            return false;
        }
        out.set(name, std::move(value));
        offset = next;
    }
    return true;
}

} // namespace

bool encode_script_data(const json::Value& data, std::vector<std::uint8_t>& out, std::string& error) {
    if (!data.is_object()) { error = "unit data must be an object"; return false; }
    out.assign(8, 0);
    std::uint32_t first;
    if (!write_children(out, data, "data", first, error)) return false;
    set(out, 0, first);
    return true;
}

bool decode_script_data(const std::vector<std::uint8_t>& bytes, json::Value& data, std::string& error) {
    if (bytes.size() < 8 || get(bytes, 4) != 0) { error = "script data header is malformed"; return false; }
    return read_entries(bytes, get(bytes, 0), data, 0, error);
}

} // namespace dtglb::stingray::unit
