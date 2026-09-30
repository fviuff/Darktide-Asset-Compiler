#include "stingray/resource_name.h"
#include "stingray/package/package_v43.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace dtglb::stingray::package {
namespace {template<class T>void append(std::vector<std::uint8_t>&o,const T&v){const auto*p=reinterpret_cast<const std::uint8_t*>(&v);o.insert(o.end(),p,p+sizeof(T));}template<class T>T read(const std::vector<std::uint8_t>&b,std::size_t o){T v{};std::memcpy(&v,b.data()+o,sizeof(T));return v;}}
std::vector<std::uint8_t> build_package_body(std::vector<PackageEntry> entries){
    entries.erase(std::remove_if(entries.begin(),entries.end(),[](const auto&e){return e.engine_type.empty()||e.name.empty();}),entries.end());
    std::sort(entries.begin(),entries.end(),[](const auto&a,const auto&b){return a.engine_type<b.engine_type||(a.engine_type==b.engine_type&&a.name<b.name);});
    entries.erase(std::unique(entries.begin(),entries.end(),[](const auto&a,const auto&b){return a.engine_type==b.engine_type&&a.name==b.name;}),entries.end());
    std::vector<std::uint8_t>o;append(o,std::uint32_t{43});append(o,static_cast<std::uint32_t>(entries.size()));for(const auto&e:entries){append(o,id64(e.engine_type));append(o,resource_name_hash(e.name));}o.push_back(1);return o;
}
std::vector<std::uint8_t> build_package_blob(const std::string&package_name,std::vector<PackageEntry>entries){if(package_name.empty())return{};return wrap_cooked_resource("package",package_name,build_package_body(std::move(entries)));}
bool validate_package_blob(const std::vector<std::uint8_t>&blob,std::string&error){std::vector<std::uint8_t>body;std::string stream;if(!parse_cooked_resource_envelope(blob,"package",body,stream,error))return false;if(!stream.empty()){error="package has external stream";return false;}if(body.size()<9){error="package body truncated";return false;}auto v=read<std::uint32_t>(body,0),n=read<std::uint32_t>(body,4);if(v!=43){error="package version is not 43";return false;}if(body.size()!=9ull+n*16ull){error="package entry table size mismatch";return false;}if(body.back()!=1){error="package footer is not 1";return false;}return true;}
bool write_package_blob(const std::string&package_name,std::vector<PackageEntry>entries,const std::filesystem::path&path,std::string&error){auto blob=build_package_blob(package_name,std::move(entries));if(blob.empty()||!validate_package_blob(blob,error))return false;std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);std::ofstream f(path,std::ios::binary);if(!f){error="cannot open package output";return false;}f.write(reinterpret_cast<const char*>(blob.data()),static_cast<std::streamsize>(blob.size()));return static_cast<bool>(f);}
}
