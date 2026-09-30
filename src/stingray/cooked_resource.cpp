#include "stingray/resource_name.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include <cstring>

namespace dtglb::stingray {
namespace {
template<class T> void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
template<class T> T read(const std::vector<std::uint8_t>& b, std::size_t o) {
    T v{}; std::memcpy(&v, b.data()+o, sizeof(T)); return v;
}
}

std::vector<std::uint8_t> wrap_cooked_resource(
    std::string_view engine_type,
    std::string_view resource_name,
    const std::vector<std::uint8_t>& body,
    std::string_view stream_name) {
    std::vector<std::uint8_t> out(38, 0);
    const std::uint64_t th = id64(engine_type);
    const std::uint64_t nh = resource_name_hash(resource_name);
    std::memcpy(out.data()+0, &th, 8);
    std::memcpy(out.data()+8, &nh, 8);
    out[16] = 1;
    const auto body_size = static_cast<std::uint32_t>(body.size());
    std::memcpy(out.data()+29, &body_size, 4);
    out[33] = 1;
    const auto stream_size = static_cast<std::uint32_t>(stream_name.size());
    std::memcpy(out.data()+34, &stream_size, 4);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), stream_name.begin(), stream_name.end());
    return out;
}

bool parse_cooked_resource_envelope(
    const std::vector<std::uint8_t>& blob,
    std::string_view expected_engine_type,
    std::vector<std::uint8_t>& body,
    std::string& stream_name,
    std::string& error) {
    if (blob.size() < 38) { error = "cooked resource is shorter than 38-byte envelope"; return false; }
    const auto expected = id64(expected_engine_type);
    if (read<std::uint64_t>(blob,0) != expected) { error = "cooked resource type hash mismatch"; return false; }
    if (blob[16] != 1 || blob[33] != 1) { error = "unsupported cooked resource envelope flags"; return false; }
    for (std::size_t i=17;i<29;++i) if (blob[i] != 0) { error = "unsupported nonzero cooked envelope field"; return false; }
    const auto bs = read<std::uint32_t>(blob,29);
    const auto ss = read<std::uint32_t>(blob,34);
    if (38ull + bs + ss != blob.size()) { error = "cooked resource envelope length mismatch"; return false; }
    body.assign(blob.begin()+38, blob.begin()+38+bs);
    stream_name.assign(reinterpret_cast<const char*>(blob.data()+38+bs), ss);
    return true;
}

} // namespace dtglb::stingray
