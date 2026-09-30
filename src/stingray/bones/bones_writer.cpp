#include "stingray/bones/bones_writer.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include "stingray/resource_name.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace dtglb::stingray::bones {
namespace {
template<class T> void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p+sizeof(T));
}
template<class T> T read(const std::vector<std::uint8_t>& b, std::size_t o) {
    T v{}; std::memcpy(&v,b.data()+o,sizeof(T)); return v;
}
bool valid_utf8(const std::string& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char first = bytes[i++];
        if (first < 0x80) continue;
        std::uint32_t codepoint = 0;
        std::size_t continuation = 0;
        if (first >= 0xc2 && first <= 0xdf) { codepoint = first & 0x1fu; continuation = 1; }
        else if (first >= 0xe0 && first <= 0xef) { codepoint = first & 0x0fu; continuation = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { codepoint = first & 0x07u; continuation = 3; }
        else return false;
        if (i + continuation > value.size()) return false;
        for (std::size_t j = 0; j < continuation; ++j) {
            const unsigned char next = bytes[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if ((continuation == 1 && codepoint < 0x80) ||
            (continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) return false;
    }
    return true;
}
}

std::vector<std::uint8_t> build_bones_body(
    const std::vector<std::string>& names,
    const std::vector<std::uint32_t>& requested_lods) {
    if (names.empty() || names.size() > 4096) return {};
    std::unordered_set<std::string> seen;
    std::unordered_set<std::uint32_t> seen_hashes;
    for (const auto& n:names) {
        if (n.empty() || n.find('\0')!=std::string::npos || !seen.insert(n).second) return {};
        if (!seen_hashes.insert(id32_from_id64(n)).second) return {};
    }
    std::vector<std::uint32_t> lods=requested_lods;
    if (lods.empty()) lods.push_back(static_cast<std::uint32_t>(names.size()));
    if (lods.empty() || lods.size()>32 || lods.front()!=names.size()) return {};
    for (std::size_t i=0;i<lods.size();++i) {
        if (lods[i]==0 || lods[i]>names.size()) return {};
        if (i && lods[i]>lods[i-1]) return {};
    }
    std::vector<std::uint8_t> out;
    append(out,static_cast<std::uint32_t>(names.size()));
    append(out,static_cast<std::uint32_t>(lods.size()));
    for (const auto& n:names) append(out,id32_from_id64(n));
    for (auto v:lods) append(out,v);
    for (const auto& n:names) { out.insert(out.end(),n.begin(),n.end()); out.push_back(0); }
    return out;
}

std::vector<std::uint8_t> build_cooked_bones(const BonesResource& resource) {
    if (resource.resource_name.empty()) return {};
    auto body=build_bones_body(resource.names,resource.lod_counts);
    if (body.empty()) return {};
    return wrap_cooked_resource("bones",resource.resource_name,body);
}

bool validate_cooked_bones(const std::vector<std::uint8_t>& blob,std::vector<std::string>* names,std::string& error) {
    std::vector<std::uint8_t> body; std::string stream;
    if (!parse_cooked_resource_envelope(blob,"bones",body,stream,error)) return false;
    if (!stream.empty()) { error="BONES may not declare an external stream name"; return false; }
    if (body.size()<12) { error="BONES body is truncated"; return false; }
    const auto count=read<std::uint32_t>(body,0), lod_count=read<std::uint32_t>(body,4);
    if (count<1 || count>4096 || lod_count<1 || lod_count>32) { error="BONES count is implausible"; return false; }
    const std::size_t fixed=8ull+count*4ull+lod_count*4ull;
    if (fixed>body.size()) { error="BONES fixed tables are truncated"; return false; }
    std::vector<std::uint32_t> hashes(count),lods(lod_count);
    for (std::uint32_t i=0;i<count;++i) hashes[i]=read<std::uint32_t>(body,8+i*4);
    for (std::uint32_t i=0;i<lod_count;++i) lods[i]=read<std::uint32_t>(body,8+count*4+i*4);
    if (lods.front()!=count) { error="BONES first LOD does not contain all bones"; return false; }
    for (std::size_t i=0;i<lods.size();++i) if (!lods[i] || lods[i]>count || (i&&lods[i]>lods[i-1])) { error="BONES LOD table is invalid"; return false; }
    std::size_t cur=fixed; std::vector<std::string> parsed; parsed.reserve(count);
    std::unordered_set<std::string> seen_names;
    std::unordered_set<std::uint32_t> seen_hashes;
    for (std::uint32_t i=0;i<count;++i) {
        auto it=std::find(body.begin()+static_cast<std::ptrdiff_t>(cur),body.end(),0);
        if (it==body.end()) { error="BONES name is not NUL terminated"; return false; }
        std::string n(reinterpret_cast<const char*>(body.data()+cur),static_cast<std::size_t>(it-(body.begin()+static_cast<std::ptrdiff_t>(cur))));
        if (n.empty() || id32_from_id64(n)!=hashes[i]) { error="BONES name/hash mismatch"; return false; }
        if (!seen_names.insert(n).second || !seen_hashes.insert(hashes[i]).second) { error="BONES identities are not unique"; return false; }
        parsed.push_back(n); cur=static_cast<std::size_t>(it-body.begin())+1;
    }
    if (cur!=body.size()) { error="BONES has trailing bytes"; return false; }
    if (names) *names=std::move(parsed);
    return true;
}

bool load_cooked_bones(const std::filesystem::path& path, const std::string& resource_name,
                       BonesResource& resource, std::string& error) {
    resource = {};
    if (resource_name.empty()) { error = "reference BONES requires an explicit resource identity"; return false; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open reference BONES: " + path.string(); return false; }
    // A cooked BONES body is a compact name/LOD table. Bound the read before
    // materializing both the envelope and its parsed body in memory.
    constexpr std::streamoff max_reference_bytes = 16 * 1024 * 1024;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0 || size > max_reference_bytes) {
        error = "reference BONES file size is invalid or exceeds 16 MiB";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> blob(static_cast<std::size_t>(size));
    if (!blob.empty()) file.read(reinterpret_cast<char*>(blob.data()), size);
    if (!file) { error = "failed reading reference BONES: " + path.string(); return false; }
    if (blob.size() < 16 || read<std::uint64_t>(blob, 8) != resource_name_hash(resource_name)) {
        error = "reference BONES envelope identity does not match --skeleton-resource";
        return false;
    }
    std::vector<std::uint8_t> body;
    std::string stream;
    if (!parse_cooked_resource_envelope(blob, "bones", body, stream, error)) return false;
    if (!stream.empty()) { error = "BONES may not declare an external stream name"; return false; }
    if (body.size() < 12) { error = "BONES body is truncated"; return false; }
    const auto count = read<std::uint32_t>(body, 0), lod_count = read<std::uint32_t>(body, 4);
    if (count < 1 || count > 4096 || lod_count < 1 || lod_count > 32) { error = "BONES count is implausible"; return false; }
    const std::size_t fixed = 8ull + count * 4ull + lod_count * 4ull;
    if (fixed > body.size()) { error = "BONES fixed tables are truncated"; return false; }
    std::vector<std::uint32_t> hashes(count), lods(lod_count);
    for (std::uint32_t i = 0; i < count; ++i) hashes[i] = read<std::uint32_t>(body, 8 + i * 4);
    for (std::uint32_t i = 0; i < lod_count; ++i) lods[i] = read<std::uint32_t>(body, 8 + count * 4 + i * 4);
    if (lods.front() != count) { error = "BONES first LOD does not contain all bones"; return false; }
    for (std::size_t i = 0; i < lods.size(); ++i)
        if (!lods[i] || lods[i] > count || (i && lods[i] > lods[i - 1])) { error = "BONES LOD table is invalid"; return false; }
    std::size_t cursor = fixed;
    std::vector<std::string> names;
    names.reserve(count);
    std::unordered_set<std::string> seen_names;
    std::unordered_set<std::uint32_t> seen_hashes;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto terminator = std::find(body.begin() + static_cast<std::ptrdiff_t>(cursor), body.end(), 0);
        if (terminator == body.end()) { error = "BONES name is not NUL terminated"; return false; }
        std::string name(reinterpret_cast<const char*>(body.data() + cursor),
                         static_cast<std::size_t>(terminator - (body.begin() + static_cast<std::ptrdiff_t>(cursor))));
        if (name.empty() || !valid_utf8(name) || id32_from_id64(name) != hashes[i]) { error = "BONES name/hash mismatch or invalid UTF-8"; return false; }
        if (!seen_names.insert(name).second || !seen_hashes.insert(hashes[i]).second) {
            error = "BONES identities are not unique"; return false;
        }
        names.push_back(std::move(name));
        cursor = static_cast<std::size_t>(terminator - body.begin()) + 1;
    }
    if (cursor != body.size()) { error = "BONES has trailing bytes"; return false; }
    resource.resource_name = resource_name;
    resource.names = std::move(names);
    resource.lod_counts = std::move(lods);
    return true;
}

bool map_bone_names(const std::vector<std::string>& source_names,
                    const std::vector<std::string>& reference_names,
                    std::vector<std::uint32_t>& source_to_reference,
                    std::string& error) {
    source_to_reference.clear();
    if (source_names.empty() || source_names.size() != reference_names.size()) {
        error = "reference BONES must contain exactly the source skin joint set";
        return false;
    }
    std::unordered_map<std::string, std::uint32_t> reference_indices;
    for (std::size_t i = 0; i < reference_names.size(); ++i) {
        if (reference_names[i].empty() || !reference_indices.emplace(reference_names[i], static_cast<std::uint32_t>(i)).second) {
            error = "reference BONES contains an ambiguous joint name";
            return false;
        }
    }
    std::unordered_set<std::string> seen_source;
    source_to_reference.reserve(source_names.size());
    for (const auto& name : source_names) {
        if (name.empty() || !seen_source.insert(name).second) {
            source_to_reference.clear(); error = "source skin contains an ambiguous joint name"; return false;
        }
        const auto found = reference_indices.find(name);
        if (found == reference_indices.end()) {
            source_to_reference.clear(); error = "source skin joint is absent from reference BONES: " + name; return false;
        }
        source_to_reference.push_back(found->second);
    }
    return true;
}

bool write_cooked_bones(const BonesResource& resource,const std::filesystem::path& path,std::string& error) {
    auto blob=build_cooked_bones(resource); if (blob.empty()) { error="invalid BONES resource fields"; return false; }
    std::vector<std::string> parsed; if (!validate_cooked_bones(blob,&parsed,error)) return false;
    std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
    std::ofstream f(path,std::ios::binary); if(!f){error="cannot open BONES output: "+path.string();return false;}
    f.write(reinterpret_cast<const char*>(blob.data()),static_cast<std::streamsize>(blob.size()));
    if(!f){error="failed writing BONES: "+path.string();return false;} return true;
}

} // namespace dtglb::stingray::bones
