#include "gltf/gltf_images.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dtglb::gltf {
namespace {
bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& directory) {
    auto p = path.begin(), d = directory.begin();
    for (; d != directory.end(); ++d, ++p) if (p == path.end() || *p != *d) return false;
    return true;
}
std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::array<int, 256> lookup{}; lookup.fill(-1); constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) lookup[static_cast<unsigned char>(alphabet[i])] = i;
    std::vector<std::uint8_t> output; int value = 0, bits = -8;
    for (unsigned char c : text) { if (c == '=') break; const int digit = lookup[c]; if (digit < 0) continue; value = (value << 6) | digit; bits += 6; if (bits >= 0) { output.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff)); bits -= 8; } }
    return output;
}
}
std::vector<std::uint8_t> read_image_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("cannot open " + path.string()); file.seekg(0, std::ios::end); const auto length = file.tellg(); file.seekg(0, std::ios::beg); if (length < 0) throw std::runtime_error("cannot size " + path.string()); std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length)); if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), length); if (!file) throw std::runtime_error("cannot read " + path.string()); return bytes;
}
std::string percent_decode_image_uri(std::string_view text) {
    std::string output; output.reserve(text.size()); const auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c-'0'; if (c >= 'a' && c <= 'f') return c-'a'+10; if (c >= 'A' && c <= 'F') return c-'A'+10; return -1; };
    for (std::size_t i=0;i<text.size();++i) { if (text[i]=='%' && i+2<text.size()) { const int hi=hex(text[i+1]), lo=hex(text[i+2]); if (hi>=0&&lo>=0) { output.push_back(static_cast<char>((hi<<4)|lo)); i+=2; continue; } } output.push_back(text[i]); } return output;
}
std::filesystem::path resolve_image_path(const std::filesystem::path& gltf_path, std::string_view uri) {
    const auto decoded = percent_decode_image_uri(uri); const std::filesystem::path relative(decoded); if (relative.has_root_name() || relative.has_root_directory()) throw std::runtime_error("image URI must be relative to the glTF document directory: " + std::string(uri));
    std::error_code error; const auto root=std::filesystem::weakly_canonical(gltf_path.parent_path(),error); if(error) throw std::runtime_error("cannot resolve glTF document directory for image URI: " + std::string(uri)); auto candidate=std::filesystem::weakly_canonical(root/relative,error); if(!error&&std::filesystem::exists(candidate,error)) candidate=std::filesystem::canonical(candidate,error); if(error||!path_is_within(candidate,root)) throw std::runtime_error("image URI escapes the glTF document directory: " + std::string(uri)); return candidate;
}
std::string infer_image_mime(const std::vector<std::uint8_t>& bytes, std::string_view uri) {
    static constexpr std::array<std::uint8_t,8> png{{0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a}}; static constexpr std::array<std::uint8_t,12> ktx2{{0xab,'K','T','X',' ','2','0',0xbb,0x0d,0x0a,0x1a,0x0a}};
    if(bytes.size()>=png.size()&&std::equal(png.begin(),png.end(),bytes.begin()))return "image/png"; if(bytes.size()>=2&&bytes[0]==0xff&&bytes[1]==0xd8)return "image/jpeg"; if(bytes.size()>=12&&std::memcmp(bytes.data(),"RIFF",4)==0&&std::memcmp(bytes.data()+8,"WEBP",4)==0)return "image/webp"; if(bytes.size()>=ktx2.size()&&std::equal(ktx2.begin(),ktx2.end(),bytes.begin()))return "image/ktx2"; if(bytes.size()>=4&&std::memcmp(bytes.data(),"DDS ",4)==0)return "image/vnd-ms.dds"; auto extension=std::filesystem::path(percent_decode_image_uri(uri)).extension().string(); std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); if(extension==".png")return "image/png"; if(extension==".jpg"||extension==".jpeg")return "image/jpeg"; if(extension==".webp")return "image/webp"; if(extension==".ktx2")return "image/ktx2"; if(extension==".dds")return "image/vnd-ms.dds"; return {};
}
std::vector<std::uint8_t> decode_data_uri(std::string_view uri, std::string* mime) {
    if(!uri.starts_with("data:"))return{}; const auto comma=uri.find(','); if(comma==std::string_view::npos)return{}; const auto meta=uri.substr(5,comma-5), body=uri.substr(comma+1); if(mime){const auto semicolon=meta.find(';');*mime=std::string(meta.substr(0,semicolon));} if(meta.find(";base64")!=std::string_view::npos)return base64_decode(body); const auto decoded=percent_decode_image_uri(body); return {decoded.begin(),decoded.end()};
}
}
