#include "stingray/resource_name.h"
#include "stingray/material/material_v61.h"
#include "stingray/murmur_hash.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

namespace dtglb::stingray::material {
namespace {
template<class T> void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
template<class T> bool pull(const std::vector<std::uint8_t>& b, std::size_t& p, std::size_t end, T& v) {
    if (p > end || sizeof(T) > end - p) return false;
    std::memcpy(&v, b.data() + p, sizeof(T)); p += sizeof(T); return true;
}
template<class T> T read_at(const std::vector<std::uint8_t>& b, std::size_t p) {
    T v{}; std::memcpy(&v,b.data()+p,sizeof(T)); return v;
}
std::size_t variable_width(std::uint32_t klass) {
    switch (klass) { case 0: return 4; case 1: return 8; case 2: return 12; case 3: return 16; default: return 0; }
}
bool checked_u32(std::size_t v, std::uint32_t& out) {
    if (v > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(v); return true;
}

struct TextureHashLocation {
    std::uint32_t channel_hash = 0;
    std::uint64_t resource_hash = 0;
    std::size_t resource_hash_offset = 0;
};

bool skip_counted_records(const std::vector<std::uint8_t>& bytes, std::size_t& p,
                         std::size_t end, std::size_t record_size, std::uint32_t& count) {
    if (!pull(bytes, p, end, count) || record_size == 0 || count > (end - p) / record_size) return false;
    p += static_cast<std::size_t>(count) * record_size;
    return true;
}

bool locate_v61_variable_data(const std::vector<std::uint8_t>& bytes,
                              std::size_t& data_start, std::size_t& data_size,
                              MaterialDependencyHashes* dependencies = nullptr,
                              std::vector<TextureHashLocation>* texture_locations = nullptr) {
    if (bytes.size() < kMaterialPayloadOffset) return false;
    std::size_t hp = 0;
    std::uint32_t version=0, material_offset=0, material_size=0;
    std::uint32_t shader_offset=0, shader_size=0, other_offset=0, other_size=0;
    if (!pull(bytes,hp,bytes.size(),version) || (version != 61 && version != 62) ||
        !pull(bytes,hp,bytes.size(),material_offset) || material_offset != kMaterialPayloadOffset ||
        !pull(bytes,hp,bytes.size(),material_size) || !pull(bytes,hp,bytes.size(),shader_offset) ||
        !pull(bytes,hp,bytes.size(),shader_size) || !pull(bytes,hp,bytes.size(),other_offset) ||
        !pull(bytes,hp,bytes.size(),other_size)) return false;
    const std::uint64_t material_end64=static_cast<std::uint64_t>(material_offset)+material_size;
    if (material_end64 > bytes.size()) return false;
    const auto material_end=static_cast<std::size_t>(material_end64);
    std::size_t shader_end=material_end;
    if (shader_size) {
        const std::uint64_t end64=static_cast<std::uint64_t>(shader_offset)+shader_size;
        if (shader_offset != material_end || end64 > bytes.size()) return false;
        shader_end=static_cast<std::size_t>(end64);
    }
    std::size_t stream_end=shader_end;
    if (other_size) {
        const std::uint64_t end64=static_cast<std::uint64_t>(other_offset)+other_size;
        if (other_offset != shader_end || end64 > bytes.size()) return false;
        stream_end=static_cast<std::size_t>(end64);
    }
    if (stream_end != bytes.size()) return false;

    std::size_t p=material_offset;
    std::uint32_t direct_selector=0;
    std::uint64_t provider_hash=0, parent_hash=0;
    if (!pull(bytes,p,material_end,direct_selector) ||
        !pull(bytes,p,material_end,provider_hash) ||
        !pull(bytes,p,material_end,parent_hash)) return false;
    std::uint32_t count=0;
    if (!skip_counted_records(bytes,p,material_end,4,count)) return false;
    if (!pull(bytes,p,material_end,count) || count > (material_end-p)/12) return false;
    MaterialDependencyHashes found;
    std::vector<TextureHashLocation> locations;
    found.shader_provider_material_hash=provider_hash;
    found.parent_material_hash=parent_hash;
    found.texture_resource_hashes.reserve(count);
    for (std::uint32_t i=0;i<count;++i) {
        std::uint32_t channel_hash=0;
        std::uint64_t texture_hash=0;
        if (!pull(bytes,p,material_end,channel_hash)) return false;
        const std::size_t hash_offset = p;
        if (!pull(bytes,p,material_end,texture_hash)) return false;
        found.texture_resource_hashes.push_back(texture_hash);
        locations.push_back({channel_hash, texture_hash, hash_offset});
    }
    if (!skip_counted_records(bytes,p,material_end,8,count) ||
        !skip_counted_records(bytes,p,material_end,20,count)) return false;
    std::uint32_t variable_data_size=0;
    if (!pull(bytes,p,material_end,variable_data_size) || variable_data_size > material_end-p) return false;
    data_start=p;
    data_size=variable_data_size;
    p += variable_data_size;
    if (!skip_counted_records(bytes,p,material_end,5,count) ||
        !skip_counted_records(bytes,p,material_end,8,count) || p != material_end) return false;
    if (dependencies) *dependencies=std::move(found);
    if (texture_locations) *texture_locations=std::move(locations);
    return true;
}
}

bool validate_material_stream_model(const MaterialStream& m, std::string& error) {
    if (m.version != 60 && m.version != 61 && m.version != 62) { error="unsupported material stream version"; return false; }
    const unsigned shader_provider_forms = (m.direct_shader_selector_hash != 0 ? 1u : 0u) +
        (m.shader_provider_material_hash != 0 ? 1u : 0u) + (!m.shader_blob.empty() ? 1u : 0u);
    if (shader_provider_forms != 1u) { error="material requires exactly one shader provider form"; return false; }
    if (m.variables.size() > std::numeric_limits<std::uint32_t>::max() ||
        m.variable_data.size() > std::numeric_limits<std::uint32_t>::max()) { error="material table is too large"; return false; }
    for (const auto& v:m.variables) {
        if (v.elements != 0) { error="material variable arrays are not supported yet"; return false; }
        if (v.element_stride != 0) { error="material variable element stride is reserved and must be zero"; return false; }
        const auto w=variable_width(v.klass); if (!w) { error="unsupported material variable class"; return false; }
        if (v.data_offset > m.variable_data.size() || w > m.variable_data.size()-v.data_offset) { error="material variable points outside variable_data"; return false; }
    }
    return true;
}

std::vector<std::uint8_t> serialize_material_stream(const MaterialStream& m, std::string& error) {
    if (!validate_material_stream_model(m,error)) return {};
    std::vector<std::uint8_t> material;
    append(material,m.direct_shader_selector_hash); append(material,m.shader_provider_material_hash); append(material,m.parent_material_hash);
    append(material,static_cast<std::uint32_t>(m.unknown_u32.size())); for(auto v:m.unknown_u32) append(material,v);
    append(material,static_cast<std::uint32_t>(m.textures.size())); for(const auto&t:m.textures){append(material,t.channel_hash);append(material,t.resource_hash);}
    append(material,static_cast<std::uint32_t>(m.contexts.size())); for(const auto&c:m.contexts){append(material,c.name_hash);append(material,c.value_hash);}
    append(material,static_cast<std::uint32_t>(m.variables.size())); for(const auto&v:m.variables){append(material,v.klass);append(material,v.elements);append(material,v.name_hash);append(material,v.data_offset);append(material,v.element_stride);}
    append(material,static_cast<std::uint32_t>(m.variable_data.size())); material.insert(material.end(),m.variable_data.begin(),m.variable_data.end());
    append(material,static_cast<std::uint32_t>(m.unknown_5.size())); for(const auto&r:m.unknown_5) material.insert(material.end(),r.begin(),r.end());
    append(material,static_cast<std::uint32_t>(m.unknown_8.size())); for(const auto&r:m.unknown_8) material.insert(material.end(),r.begin(),r.end());
    std::uint32_t ms=0; if(!checked_u32(material.size(),ms)){error="material payload exceeds u32 size";return{};}
    const std::uint64_t material_end64=static_cast<std::uint64_t>(kMaterialPayloadOffset)+ms;
    if(material_end64>0xffffffffull){error="material stream offsets exceed u32 range";return{};}
    const auto material_end=static_cast<std::uint32_t>(material_end64);
    std::uint32_t shader_size=0,other_size=0;if(!checked_u32(m.shader_blob.size(),shader_size)||!checked_u32(m.other_blob.size(),other_size)){error="material section exceeds u32 size";return{};}
    const auto shader_offset=shader_size?material_end:m.shader_offset_if_empty;
    const std::uint64_t shader_end64=static_cast<std::uint64_t>(material_end)+shader_size;if(shader_end64>0xffffffffull){error="material shader end exceeds u32 range";return{};}
    const auto shader_end=static_cast<std::uint32_t>(shader_end64);
    const auto other_offset=other_size?shader_end:m.other_offset_if_empty;
    std::vector<std::uint8_t> out;out.reserve(28+material.size()+m.shader_blob.size()+m.other_blob.size());
    append(out,m.version);append(out,std::uint32_t{kMaterialPayloadOffset});append(out,ms);append(out,shader_offset);append(out,shader_size);append(out,other_offset);append(out,other_size);
    out.insert(out.end(),material.begin(),material.end());out.insert(out.end(),m.shader_blob.begin(),m.shader_blob.end());out.insert(out.end(),m.other_blob.begin(),m.other_blob.end());return out;
}

bool parse_material_stream(const std::vector<std::uint8_t>& b, MaterialStream& out, std::string& error) {
    if(b.size()<28){error="material stream is shorter than 28-byte header";return false;}
    MaterialStream m;std::size_t hp=0;std::uint32_t material_offset=0,material_size=0,shader_offset=0,shader_size=0,other_offset=0,other_size=0;
    if(!pull(b,hp,b.size(),m.version)||!pull(b,hp,b.size(),material_offset)||!pull(b,hp,b.size(),material_size)||!pull(b,hp,b.size(),shader_offset)||!pull(b,hp,b.size(),shader_size)||!pull(b,hp,b.size(),other_offset)||!pull(b,hp,b.size(),other_size)){error="material stream header truncated";return false;}
    if(m.version!=60&&m.version!=61&&m.version!=62){error="unsupported material stream version";return false;} if(material_offset!=28){error="unsupported material payload offset";return false;}
    const std::uint64_t me64=static_cast<std::uint64_t>(material_offset)+material_size;if(me64>b.size()){error="material section points outside stream";return false;}const auto me=static_cast<std::size_t>(me64);
    std::size_t shader_end=me;if(shader_size){if(shader_offset!=me||static_cast<std::uint64_t>(shader_offset)+shader_size>b.size()){error="invalid/non-contiguous shader section";return false;}shader_end=static_cast<std::size_t>(shader_offset)+shader_size;}
    std::size_t other_end=shader_end;if(other_size){if(other_offset!=shader_end||static_cast<std::uint64_t>(other_offset)+other_size>b.size()){error="invalid/non-contiguous auxiliary section";return false;}other_end=static_cast<std::size_t>(other_offset)+other_size;}
    if(other_end!=b.size()){error="material stream has unexplained trailing bytes";return false;}
    std::size_t p=material_offset;
    std::uint32_t n=0;if(!pull(b,p,me,m.direct_shader_selector_hash)||!pull(b,p,me,m.shader_provider_material_hash)||!pull(b,p,me,m.parent_material_hash)||!pull(b,p,me,n)){error="material payload prefix truncated";return false;}
    if(n>(me-p)/4){error="material unknown_u32 count invalid";return false;}m.unknown_u32.resize(n);for(auto&v:m.unknown_u32)if(!pull(b,p,me,v)){error="material unknown_u32 table truncated";return false;}
    if(!pull(b,p,me,n)){error="material texture count missing";return false;}if(n>(me-p)/12){error="material texture count invalid";return false;}m.textures.resize(n);for(auto&t:m.textures)if(!pull(b,p,me,t.channel_hash)||!pull(b,p,me,t.resource_hash)){error="material texture table truncated";return false;}
    if(!pull(b,p,me,n)){error="material context count missing";return false;}if(n>(me-p)/8){error="material context count invalid";return false;}m.contexts.resize(n);for(auto&c:m.contexts)if(!pull(b,p,me,c.name_hash)||!pull(b,p,me,c.value_hash)){error="material context table truncated";return false;}
    if(!pull(b,p,me,n)){error="material variable count missing";return false;}if(n>(me-p)/20){error="material variable count invalid";return false;}m.variables.resize(n);for(auto&v:m.variables)if(!pull(b,p,me,v.klass)||!pull(b,p,me,v.elements)||!pull(b,p,me,v.name_hash)||!pull(b,p,me,v.data_offset)||!pull(b,p,me,v.element_stride)){error="material variable table truncated";return false;}
    std::uint32_t data_size=0;if(!pull(b,p,me,data_size)||data_size>me-p){error="material variable_data truncated";return false;}m.variable_data.assign(b.begin()+static_cast<std::ptrdiff_t>(p),b.begin()+static_cast<std::ptrdiff_t>(p+data_size));p+=data_size;
    if(!pull(b,p,me,n)){error="material unknown_5 count missing";return false;}if(n>(me-p)/5){error="material unknown_5 count invalid";return false;}m.unknown_5.resize(n);for(auto&r:m.unknown_5){std::copy_n(b.begin()+static_cast<std::ptrdiff_t>(p),5,r.begin());p+=5;}
    if(!pull(b,p,me,n)){error="material unknown_8 count missing";return false;}if(n>(me-p)/8){error="material unknown_8 count invalid";return false;}m.unknown_8.resize(n);for(auto&r:m.unknown_8){std::copy_n(b.begin()+static_cast<std::ptrdiff_t>(p),8,r.begin());p+=8;}
    if(p!=me){error="material payload has unexplained trailing bytes";return false;}
    if(shader_size)m.shader_blob.assign(b.begin()+static_cast<std::ptrdiff_t>(shader_offset),b.begin()+static_cast<std::ptrdiff_t>(shader_offset+shader_size));else m.shader_offset_if_empty=shader_offset;
    if(other_size)m.other_blob.assign(b.begin()+static_cast<std::ptrdiff_t>(other_offset),b.begin()+static_cast<std::ptrdiff_t>(other_offset+other_size));else m.other_offset_if_empty=other_offset;
    if (!validate_material_stream_model(m, error)) return false;
    out = std::move(m);
    return true;
}

std::vector<std::uint8_t> serialize_material_header(const MaterialHeader& h, std::string& error) {
    if(h.stream_name.empty()){error="material stream name is empty";return{};}if(h.stream_name.size()>30){error="material stream name exceeds 30-byte header field";return{};}for(unsigned char c:h.stream_name)if(c>0x7f){error="material stream name must be ASCII";return{};}
    std::vector<std::uint8_t> out(68,0);const auto type=id64("material");std::memcpy(out.data(),&type,8);std::memcpy(out.data()+8,&h.resource_hash,8);std::uint32_t one=1,thirty=30;std::memcpy(out.data()+16,&one,4);out[28]=1;std::memcpy(out.data()+29,&thirty,4);std::memcpy(out.data()+33,&one,4);std::memcpy(out.data()+38,h.stream_name.data(),h.stream_name.size());return out;
}

bool parse_material_header(const std::vector<std::uint8_t>& b, MaterialHeader& out, std::string& error) {
    if(b.size()!=68){error="material header must be exactly 68 bytes";return false;}if(read_at<std::uint64_t>(b,0)!=id64("material")){error="material type hash mismatch";return false;}if(read_at<std::uint32_t>(b,16)!=1||read_at<std::uint32_t>(b,20)!=0||read_at<std::uint32_t>(b,24)!=0||b[28]!=1||read_at<std::uint32_t>(b,29)!=30||read_at<std::uint32_t>(b,33)!=1||b[37]!=0){error="material fixed header fields mismatch";return false;}MaterialHeader h;h.resource_hash=read_at<std::uint64_t>(b,8);std::size_t n=0;while(n<30&&b[38+n]){if(b[38+n]>0x7f){error="material stream name is not ASCII";return false;}++n;}for(std::size_t i=n;i<30;++i)if(b[38+i]!=0){error="material stream field has nonzero bytes after terminator";return false;}h.stream_name.assign(reinterpret_cast<const char*>(b.data()+38),n);if(h.stream_name.empty()){error="material stream name is empty";return false;}out=std::move(h);return true;
}

bool build_inherited_material(const InheritedMaterialSpec& s, MaterialStream& out, std::string& error) {
    if(!s.shader_provider_material_hash){error="inherited material requires a nonzero shader provider material hash";return false;}
    static const std::array<const char*,8> allowed{{"default","metal_solid","metal_sheet","cloth","concrete","brick","bone","plastic"}};
    if(!s.surface_material.empty() && std::find_if(allowed.begin(),allowed.end(),[&](const char* v){return s.surface_material==v;})==allowed.end()){error="unknown surface material";return false;}
    if (s.contexts.has_value() && !s.surface_material.empty()) { error="inherited material cannot combine explicit contexts with surface_material"; return false; }
    MaterialStream m;m.version=61;m.direct_shader_selector_hash=0;m.shader_provider_material_hash=s.shader_provider_material_hash;m.parent_material_hash=s.parent_material_hash;m.textures=s.textures;m.variables=s.variables;m.variable_data=s.variable_data;
    if (!s.contexts.has_value()) {
        ContextBinding c;c.name_hash=kSurfaceMaterialContextHash;c.value_hash=id32_from_id64(s.surface_material.empty()?"default":s.surface_material);m.contexts.push_back(c);
    } else {
        for (const auto& c : *s.contexts) {
            if (std::find_if(m.contexts.begin(), m.contexts.end(), [&](const ContextBinding& prior){ return prior.name_hash == c.name_hash; }) != m.contexts.end()) { error="inherited material contexts contain duplicate names"; return false; }
            m.contexts.push_back(c);
        }
    }
    if(!validate_material_stream_model(m,error))return false;out=std::move(m);return true;
}

bool clone_material_v61_with_variable_data_edit(
    const std::vector<std::uint8_t>& donor, std::uint32_t data_offset,
    const std::vector<std::uint8_t>& expected_bytes,
    const std::vector<std::uint8_t>& replacement_bytes,
    std::vector<std::uint8_t>& out, std::string& error) {
    if (expected_bytes.empty() || expected_bytes.size() != replacement_bytes.size()) {
        error="variable_data edit requires same-length, nonempty expected and replacement bytes";
        return false;
    }
    std::size_t data_start=0, data_size=0;
    if (!locate_v61_variable_data(donor,data_start,data_size)) {
        error="donor is not a structurally bounded v61 material stream";
        return false;
    }
    if (data_offset > data_size || expected_bytes.size() > data_size-data_offset) {
        error="variable_data edit range is outside donor variable_data";
        return false;
    }
    const auto patch_start=data_start+data_offset;
    if (!std::equal(expected_bytes.begin(),expected_bytes.end(),donor.begin()+static_cast<std::ptrdiff_t>(patch_start))) {
        error="variable_data donor bytes do not match expected_bytes";
        return false;
    }
    out=donor;
    std::copy(replacement_bytes.begin(),replacement_bytes.end(),out.begin()+static_cast<std::ptrdiff_t>(patch_start));
    return true;
}

bool clone_material_v61_with_texture_hash_edits(
    const std::vector<std::uint8_t>& donor,
    const std::vector<TextureResourceHashEdit>& edits,
    std::vector<std::uint8_t>& out,
    std::string& error) {
    if (edits.empty()) {
        error = "texture hash edit requires at least one channel replacement";
        return false;
    }
    std::size_t data_start = 0, data_size = 0;
    std::vector<TextureHashLocation> locations;
    if (!locate_v61_variable_data(donor, data_start, data_size, nullptr, &locations)) {
        error = "donor is not a structurally bounded v61 material stream";
        return false;
    }
    (void)data_start;
    (void)data_size;
    std::set<std::uint32_t> edited_channels;
    std::vector<std::pair<std::size_t, std::uint64_t>> replacements;
    replacements.reserve(edits.size());
    for (const auto& edit : edits) {
        if (!edit.channel_hash || !edit.replacement_resource_hash ||
            !edited_channels.insert(edit.channel_hash).second) {
            error = "texture hash edits require unique nonzero channels and replacement resource hashes";
            return false;
        }
        const auto found = std::find_if(locations.begin(), locations.end(), [&](const auto& texture) {
            return texture.channel_hash == edit.channel_hash;
        });
        if (found == locations.end()) {
            error = "material stream has no texture record for requested channel hash";
            return false;
        }
        if (std::find_if(std::next(found), locations.end(), [&](const auto& texture) {
                return texture.channel_hash == edit.channel_hash;
            }) != locations.end()) {
            error = "material stream repeats a texture channel hash";
            return false;
        }
        if (found->resource_hash != edit.expected_resource_hash) {
            error = "texture channel donor resource hash does not match expected hash";
            return false;
        }
        replacements.emplace_back(found->resource_hash_offset, edit.replacement_resource_hash);
    }
    std::vector<std::uint8_t> result = donor;
    for (const auto& [offset, hash] : replacements)
        std::memcpy(result.data() + offset, &hash, sizeof(hash));
    std::size_t verified_start = 0, verified_size = 0;
    std::vector<TextureHashLocation> verified;
    if (!locate_v61_variable_data(result, verified_start, verified_size, nullptr, &verified)) {
        error = "texture hash edit produced an invalid v61 material stream";
        return false;
    }
    for (const auto& edit : edits) {
        const auto found = std::find_if(verified.begin(), verified.end(), [&](const auto& texture) {
            return texture.channel_hash == edit.channel_hash;
        });
        if (found == verified.end() || found->resource_hash != edit.replacement_resource_hash) {
            error = "texture hash edit did not survive v61 stream validation";
            return false;
        }
    }
    out = std::move(result);
    error.clear();
    return true;
}

bool validate_preserved_material_v61(const std::vector<std::uint8_t>& bytes,
                                     MaterialDependencyHashes& dependencies,
                                     std::string& error) {
    std::size_t data_start=0, data_size=0;
    MaterialDependencyHashes found;
    if (!locate_v61_variable_data(bytes,data_start,data_size,&found)) {
        error="preserved stream is not a structurally bounded v61 material stream";
        return false;
    }
    dependencies=std::move(found);
    return true;
}

bool write_material_pair(std::string resource_name,const MaterialStream& stream,const std::filesystem::path& header_path,const std::filesystem::path& stream_path,std::string stream_name,std::string& error) {
    if(resource_name.empty()){error="material resource name is empty";return false;}auto sb=serialize_material_stream(stream,error);if(sb.empty())return false;MaterialStream parsed;if(!parse_material_stream(sb,parsed,error))return false;MaterialHeader h{resource_name_hash(resource_name),std::move(stream_name)};auto hb=serialize_material_header(h,error);if(hb.empty())return false;MaterialHeader hp;if(!parse_material_header(hb,hp,error))return false;std::error_code ec;std::filesystem::create_directories(header_path.parent_path(),ec);std::filesystem::create_directories(stream_path.parent_path(),ec);std::ofstream hf(header_path,std::ios::binary);if(!hf){error="cannot open material header output";return false;}hf.write(reinterpret_cast<const char*>(hb.data()),static_cast<std::streamsize>(hb.size()));if(!hf){error="failed writing material header";return false;}std::ofstream sf(stream_path,std::ios::binary);if(!sf){error="cannot open material stream output";return false;}sf.write(reinterpret_cast<const char*>(sb.data()),static_cast<std::streamsize>(sb.size()));if(!sf){error="failed writing material stream";return false;}return true;
}

bool write_preserved_material_pair(std::string resource_name,const PreservedMaterialStream& stream,const std::filesystem::path& header_path,const std::filesystem::path& stream_path,std::string stream_name,std::string& error) {
    if(resource_name.empty()){error="material resource name is empty";return false;}
    MaterialDependencyHashes dependencies;
    if(!validate_preserved_material_v61(stream.bytes,dependencies,error))return false;
    MaterialHeader h{resource_name_hash(resource_name),std::move(stream_name)};
    auto hb=serialize_material_header(h,error);
    if(hb.empty())return false;
    MaterialHeader hp;
    if(!parse_material_header(hb,hp,error))return false;
    std::error_code ec;
    std::filesystem::create_directories(header_path.parent_path(),ec);
    std::filesystem::create_directories(stream_path.parent_path(),ec);
    std::ofstream hf(header_path,std::ios::binary);
    if(!hf){error="cannot open material header output";return false;}
    hf.write(reinterpret_cast<const char*>(hb.data()),static_cast<std::streamsize>(hb.size()));
    if(!hf){error="failed writing material header";return false;}
    std::ofstream sf(stream_path,std::ios::binary);
    if(!sf){error="cannot open material stream output";return false;}
    sf.write(reinterpret_cast<const char*>(stream.bytes.data()),static_cast<std::streamsize>(stream.bytes.size()));
    if(!sf){error="failed writing material stream";return false;}
    return true;
}

} // namespace dtglb::stingray::material
