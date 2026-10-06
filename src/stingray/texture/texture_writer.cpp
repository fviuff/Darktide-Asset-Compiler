#include "stingray/texture/texture_writer.h"
#include "stingray/texture/ktx2_decoder.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <cstdlib>

#include <webp/decode.h>

#ifndef _WIN32
#include <png.h>
extern "C" {
#include <jpeglib.h>
}
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#ifdef DTGLB_USE_DIRECTXTEX
#include <DirectXTex.h>
#endif
#endif

namespace dtglb::stingray::texture {
namespace {

const TextureProfile kSubstanceBc{
    "substance_basic_bc_static_control_bc1_srgb", 72, 8, true, 0x101u, 0xa88c22b7u, TextureFiltering::ColorSrgb};
const TextureProfile kSubstanceBca{
    "substance_basic_bca_bc3_srgb", 78, 16, true, 0x101u, 0x6bd03744u, TextureFiltering::ColorSrgb};
const TextureProfile kSubstanceNm{
    "substance_basic_nm_bc5", 83, 16, false, 0x001u, 0x3390aba5u, TextureFiltering::Normal};
const TextureProfile kSubstanceOrm{
    "substance_basic_orm_bc1", 71, 8, false, 0x001u, 0xe514d1dcu, TextureFiltering::ColorLinear};
const TextureProfile kPbrEmissiveEm{
    "pbr_emissive_em_bc1_linear", 71, 8, false, 0x001u, 0xae01e4bcu, TextureFiltering::ColorLinear};

struct SurfaceMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;
};

template<class T> void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
void append_bytes(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& v) {
    out.insert(out.end(), v.begin(), v.end());
}

std::vector<std::pair<std::uint32_t,std::uint32_t>> mip_dims(std::uint32_t width, std::uint32_t height) {
    std::vector<std::pair<std::uint32_t,std::uint32_t>> out;
    while (std::min(width,height) >= 4) {
        out.emplace_back(width,height);
        if (std::min(width,height) == 4) break;
        width = std::max(1u,width/2); height = std::max(1u,height/2);
    }
    return out;
}
std::size_t surface_bytes(std::uint32_t w,std::uint32_t h,std::uint32_t block) {
    return static_cast<std::size_t>(std::max(1u,(w+3)/4))*std::max(1u,(h+3)/4)*block;
}
bool surface_is_tileable(std::uint32_t w,std::uint32_t h,std::uint32_t block) {
    const std::uint32_t blocks_w=w/4,blocks_h=h/4;
    const std::uint32_t chunk_blocks_w=block==8?128u:64u;
    return blocks_w%chunk_blocks_w==0&&blocks_h%64u==0;
}
std::size_t streamed_mips(const std::vector<std::pair<std::uint32_t,std::uint32_t>>& dims,std::uint32_t block) {
    for(std::size_t i=1;i<dims.size();++i) {
        if(!surface_is_tileable(dims[i].first,dims[i].second,block)||
            surface_bytes(dims[i].first,dims[i].second,block)<=32768) return i;
    }
    return std::numeric_limits<std::size_t>::max();
}

float srgb_to_linear(std::uint8_t v) {
    const float e=static_cast<float>(v)/255.0f;
    return e<=0.04045f?e/12.92f:std::pow((e+0.055f)/1.055f,2.4f);
}
std::uint8_t linear_to_srgb(float v) {
    v=std::clamp(v,0.0f,1.0f);
    const float e=v<=0.0031308f?v*12.92f:1.055f*std::pow(v,1.0f/2.4f)-0.055f;
    return static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(std::lround(e*255.0f)),0,255));
}

ImageRGBA downsample_linear(const ImageRGBA& src) {
    ImageRGBA d; d.width=std::max(1u,src.width/2); d.height=std::max(1u,src.height/2); d.pixels.resize(static_cast<std::size_t>(d.width)*d.height*4);
    for(std::uint32_t y=0;y<d.height;++y) for(std::uint32_t x=0;x<d.width;++x){
        int acc[4]={}; int n=0;
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const auto sx=std::min(src.width-1,x*2+static_cast<std::uint32_t>(dx));const auto sy=std::min(src.height-1,y*2+static_cast<std::uint32_t>(dy));const auto p=(static_cast<std::size_t>(sy)*src.width+sx)*4;for(int c=0;c<4;++c)acc[c]+=src.pixels[p+c];++n;}
        const auto q=(static_cast<std::size_t>(y)*d.width+x)*4;for(int c=0;c<4;++c)d.pixels[q+c]=static_cast<std::uint8_t>((acc[c]+n/2)/n);
    }
    return d;
}
ImageRGBA downsample_srgb(const ImageRGBA& src) {
    ImageRGBA d; d.width=std::max(1u,src.width/2); d.height=std::max(1u,src.height/2); d.pixels.resize(static_cast<std::size_t>(d.width)*d.height*4);
    for(std::uint32_t y=0;y<d.height;++y) for(std::uint32_t x=0;x<d.width;++x){
        float acc[3]={}; int alpha=0,n=0;
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const auto sx=std::min(src.width-1,x*2+static_cast<std::uint32_t>(dx));const auto sy=std::min(src.height-1,y*2+static_cast<std::uint32_t>(dy));const auto p=(static_cast<std::size_t>(sy)*src.width+sx)*4;for(int c=0;c<3;++c)acc[c]+=srgb_to_linear(src.pixels[p+c]);alpha+=src.pixels[p+3];++n;}
        const auto q=(static_cast<std::size_t>(y)*d.width+x)*4;for(int c=0;c<3;++c)d.pixels[q+c]=linear_to_srgb(acc[c]/n);d.pixels[q+3]=static_cast<std::uint8_t>((alpha+n/2)/n);
    }
    return d;
}
ImageRGBA downsample_normal(const ImageRGBA& src) {
    ImageRGBA d; d.width=std::max(1u,src.width/2); d.height=std::max(1u,src.height/2); d.pixels.resize(static_cast<std::size_t>(d.width)*d.height*4);
    for(std::uint32_t y=0;y<d.height;++y) for(std::uint32_t x=0;x<d.width;++x){
        double vx=0,vy=0,vz=0;int alpha=0,n=0;
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const auto sx=std::min(src.width-1,x*2+static_cast<std::uint32_t>(dx));const auto sy=std::min(src.height-1,y*2+static_cast<std::uint32_t>(dy));const auto p=(static_cast<std::size_t>(sy)*src.width+sx)*4;double nx=src.pixels[p]/127.5-1.0,ny=src.pixels[p+1]/127.5-1.0,nz=src.pixels[p+2]/127.5-1.0;double len=std::sqrt(nx*nx+ny*ny+nz*nz);if(len>0){nx/=len;ny/=len;nz/=len;}else{nx=0;ny=0;nz=1;}vx+=nx;vy+=ny;vz+=nz;alpha+=src.pixels[p+3];++n;}
        double len=std::sqrt(vx*vx+vy*vy+vz*vz);if(len>0){vx/=len;vy/=len;vz/=len;}else{vx=0;vy=0;vz=1;}const auto q=(static_cast<std::size_t>(y)*d.width+x)*4;
        d.pixels[q]=static_cast<std::uint8_t>(std::clamp<long>(std::lround((vx+1.0)*127.5),0,255));d.pixels[q+1]=static_cast<std::uint8_t>(std::clamp<long>(std::lround((vy+1.0)*127.5),0,255));d.pixels[q+2]=static_cast<std::uint8_t>(std::clamp<long>(std::lround((vz+1.0)*127.5),0,255));d.pixels[q+3]=static_cast<std::uint8_t>((alpha+n/2)/n);
    }
    return d;
}

#ifndef DTGLB_USE_DIRECTXTEX
// Portable fallback used by non-Windows builds without the production encoder.
std::uint16_t rgb565(int r,int g,int b){return static_cast<std::uint16_t>(((r*31+127)/255<<11)|((g*63+127)/255<<5)|((b*31+127)/255));}
std::array<int,3> rgb_from_565(std::uint16_t v){const int r=(v>>11)&31,g=(v>>5)&63,b=v&31;return{(r*255+15)/31,(g*255+31)/63,(b*255+15)/31};}

std::array<std::uint8_t,8> bc1(const std::array<std::array<std::uint8_t,4>,16>& px){
    int mn[3]={255,255,255},mx[3]={0,0,0};for(const auto&p:px)for(int c=0;c<3;++c){mn[c]=std::min(mn[c],static_cast<int>(p[c]));mx[c]=std::max(mx[c],static_cast<int>(p[c]));}
    std::uint16_t c0=rgb565(mx[0],mx[1],mx[2]),c1=rgb565(mn[0],mn[1],mn[2]);if(c0==c1){if(c0<0xffff)++c0;else if(c1>0)--c1;}if(c0<c1)std::swap(c0,c1);const auto p0=rgb_from_565(c0),p1=rgb_from_565(c1);std::array<std::array<int,3>,4> pal{p0,p1,{(2*p0[0]+p1[0]+1)/3,(2*p0[1]+p1[1]+1)/3,(2*p0[2]+p1[2]+1)/3},{(p0[0]+2*p1[0]+1)/3,(p0[1]+2*p1[1]+1)/3,(p0[2]+2*p1[2]+1)/3}};
    std::uint32_t bits=0;for(int i=0;i<16;++i){int best=0,bd=std::numeric_limits<int>::max();for(int j=0;j<4;++j){int d=0;for(int c=0;c<3;++c){int e=static_cast<int>(px[i][c])-pal[j][c];d+=e*e;}if(d<bd){bd=d;best=j;}}bits|=static_cast<std::uint32_t>(best)<<(2*i);}std::array<std::uint8_t,8> o{};std::memcpy(o.data(),&c0,2);std::memcpy(o.data()+2,&c1,2);std::memcpy(o.data()+4,&bits,4);return o;
}
std::array<std::uint8_t,8> bc4(const std::array<std::uint8_t,16>& values){
    int a0=*std::max_element(values.begin(),values.end()),a1=*std::min_element(values.begin(),values.end());if(a0==a1){if(a0<255)++a0;else if(a1>0)--a1;}if(a0<a1)std::swap(a0,a1);std::array<int,8> pal{};pal[0]=a0;pal[1]=a1;for(int i=1;i<7;++i)pal[i+1]=((7-i)*a0+i*a1+3)/7;std::uint64_t bits=0;for(int i=0;i<16;++i){int best=0,bd=999;for(int j=0;j<8;++j){const int d=std::abs(static_cast<int>(values[i])-pal[j]);if(d<bd){bd=d;best=j;}}bits|=static_cast<std::uint64_t>(best)<<(3*i);}std::array<std::uint8_t,8> o{};o[0]=static_cast<std::uint8_t>(a0);o[1]=static_cast<std::uint8_t>(a1);for(int i=0;i<6;++i)o[2+i]=static_cast<std::uint8_t>((bits>>(8*i))&0xff);return o;
}
#endif

std::vector<std::uint8_t> encode_surface(const ImageRGBA& img,const TextureProfile& profile,std::string& error){
    if(img.width%4||img.height%4){error="BC mip dimensions are not multiples of four";return{};}
#ifdef DTGLB_USE_DIRECTXTEX
    DirectX::Image source{img.width, img.height, DXGI_FORMAT_R8G8B8A8_UNORM,
        static_cast<std::size_t>(img.width) * 4,
        static_cast<std::size_t>(img.width) * img.height * 4,
        const_cast<std::uint8_t*>(img.pixels.data())};
    const auto format = static_cast<DXGI_FORMAT>(profile.dxgi_format);
    DirectX::ScratchImage compressed;
    const auto flags = profile.filtering == TextureFiltering::ColorSrgb ? DirectX::TEX_COMPRESS_SRGB : DirectX::TEX_COMPRESS_DEFAULT;
    const HRESULT hr = DirectX::Compress(source, format, flags,
        DirectX::TEX_THRESHOLD_DEFAULT, compressed);
    if(FAILED(hr)){std::ostringstream ss;ss<<"DirectXTex BC compression failed (HRESULT 0x"<<std::hex<<static_cast<unsigned long>(hr)<<")";error=ss.str();return{};}
    const DirectX::Image* image = compressed.GetImage(0, 0, 0);
    const auto expected = surface_bytes(img.width, img.height, profile.block_bytes);
    if(!image||image->format!=format||image->width!=img.width||image->height!=img.height||image->slicePitch!=expected||!image->pixels){error="DirectXTex returned an invalid BC surface";return{};}
    return {image->pixels, image->pixels + image->slicePitch};
#else
    std::vector<std::uint8_t> out;out.reserve(surface_bytes(img.width,img.height,profile.block_bytes));
    for(std::uint32_t by=0;by<img.height;by+=4)for(std::uint32_t bx=0;bx<img.width;bx+=4){
        std::array<std::array<std::uint8_t,4>,16> px{};int n=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x){
            const auto p=(static_cast<std::size_t>(by+y)*img.width+bx+x)*4;
            for(int c=0;c<4;++c)px[n][c]=img.pixels[p+c];++n;
        }
        if(profile.dxgi_format==78){
            // BC3 stores a BC4-style alpha block before its opaque BC1 color.
            std::array<std::uint8_t,16> alpha{};
            for(int i=0;i<16;++i)alpha[i]=px[i][3];
            const auto a=bc4(alpha), color=bc1(px);
            out.insert(out.end(),a.begin(),a.end());out.insert(out.end(),color.begin(),color.end());
        }else if(profile.dxgi_format==71||profile.dxgi_format==72){
            auto b=bc1(px);out.insert(out.end(),b.begin(),b.end());
        }else if(profile.dxgi_format==83){
            std::array<std::uint8_t,16> r{},g{};
            for(int i=0;i<16;++i){r[i]=px[i][0];g[i]=px[i][1];}
            auto br=bc4(r),bg=bc4(g);
            out.insert(out.end(),br.begin(),br.end());out.insert(out.end(),bg.begin(),bg.end());
        }else{error="unsupported native texture DXGI profile";return{};}
    }
    return out;
#endif
}

std::vector<std::uint8_t> build_dds(std::uint32_t width,std::uint32_t height,const TextureProfile& profile,const std::vector<SurfaceMip>& mips){
    std::vector<std::uint8_t> h(148,0);std::memcpy(h.data(),"DDS ",4);auto put=[&](std::size_t p,std::uint32_t v){std::memcpy(h.data()+p,&v,4);};std::uint32_t flags=0x00001007u|0x00080000u;if(mips.size()>1)flags|=0x00020000u;std::uint32_t caps=0x00001000u;if(mips.size()>1)caps|=0x00000008u|0x00400000u;put(4,124);put(8,flags);put(12,height);put(16,width);put(20,static_cast<std::uint32_t>(mips.front().bytes.size()));put(28,static_cast<std::uint32_t>(mips.size()));put(76,32);put(80,4);std::memcpy(h.data()+84,"DX10",4);put(108,caps);put(128,profile.dxgi_format);put(132,3);put(140,1);for(const auto&m:mips)append_bytes(h,m.bytes);return h;
}

#ifdef _WIN32
HMODULE load_oodle_module(const std::filesystem::path& path, DWORD* failure = nullptr) {
    DWORD old_mode = 0;
    const bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX,
                                            &old_mode) != 0;
    HMODULE module = LoadLibraryW(path.c_str());
    const DWORD load_error = module ? ERROR_SUCCESS : GetLastError();
    if (changed) SetThreadErrorMode(old_mode, nullptr);
    if (failure) *failure = load_error;
    return module;
}

std::filesystem::path oodle_config_file() {
    const auto env_path = [](const wchar_t* name) {
        const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (!needed) return std::filesystem::path{};
        std::vector<wchar_t> value(needed);
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);
        return copied && copied < needed ? std::filesystem::path(value.data()) : std::filesystem::path{};
    };
    auto root = env_path(L"DARKTIDE_OODLE_CONFIG_DIR");
    if (root.empty()) root = env_path(L"LOCALAPPDATA");
    if (root.empty()) return {};
    if (GetEnvironmentVariableW(L"DARKTIDE_OODLE_CONFIG_DIR", nullptr, 0)) return root / L"oodle_path.txt";
    return root / L"DarktideGLBCompiler" / L"oodle_path.txt";
}

std::filesystem::path oodle_dll_from_input(const std::filesystem::path& input) {
    std::error_code ec;
    if (std::filesystem::is_directory(input, ec)) return input / L"binaries" / L"oo2core_9_win64.dll";
    return input;
}

bool valid_oodle_dll(const std::filesystem::path& path, std::string& error) {
    DWORD load_error = ERROR_SUCCESS;
    HMODULE module = load_oodle_module(path, &load_error);
    if (!module) {
        error = "cannot load Oodle DLL at " + path.string() + " (Windows error " + std::to_string(load_error) + ")";
        return false;
    }
    const bool exports = GetProcAddress(module, "OodleLZ_GetCompressedBufferSizeNeeded") &&
        GetProcAddress(module, "OodleLZ_Compress") && GetProcAddress(module, "OodleLZ_Decompress");
    FreeLibrary(module);
    if (!exports) error = "Oodle DLL is missing required exports: OodleLZ_GetCompressedBufferSizeNeeded, OodleLZ_Compress, OodleLZ_Decompress";
    return exports;
}

class DarktideOodle {
public:
    using BoundFn = std::size_t (WINAPI*)(int, long long);
    using CompressFn = long long (WINAPI*)(int, const void*, long long, void*, int, const void*, const void*, const void*, void*, long long);
    using DecompressFn = long long (WINAPI*)(const void*, long long, void*, long long, int, int, int, void*, long long, void*, void*, void*, long long, int);

    ~DarktideOodle() { if (module_) FreeLibrary(module_); }
    DarktideOodle(const DarktideOodle&) = delete;
    DarktideOodle& operator=(const DarktideOodle&) = delete;
    DarktideOodle() = default;

    bool load(std::string& error) {
        std::vector<std::filesystem::path> candidates;
        std::string candidate_error;
        auto env_path = [](const wchar_t* name) -> std::filesystem::path {
            const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
            if (!needed) return {};
            std::vector<wchar_t> value(needed);
            const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);
            if (!copied || copied >= needed) return {};
            return std::filesystem::path(value.data());
        };
        const auto explicit_dll = env_path(L"DARKTIDE_OODLE_DLL");
        const auto game_root = env_path(L"DARKTIDE_GAME_ROOT");
        const bool explicit_override = !explicit_dll.empty() || !game_root.empty();
        if (!explicit_dll.empty()) {
            candidates.push_back(explicit_dll);
        } else if (!game_root.empty()) {
            candidates.push_back(game_root / L"binaries" / L"oo2core_9_win64.dll");
        } else {
            std::vector<wchar_t> executable_path(32768);
            const DWORD executable_length = GetModuleFileNameW(nullptr, executable_path.data(),
                static_cast<DWORD>(executable_path.size()));
            if (executable_length && executable_length < executable_path.size()) {
                candidates.push_back(std::filesystem::path(executable_path.data(), executable_path.data() + executable_length)
                    .parent_path() / L"oo2core_9_win64.dll");
            }
            const auto config = oodle_config_file();
            if (!config.empty()) {
                std::ifstream saved(config, std::ios::binary);
                std::string utf8_path;
                if (saved && std::getline(saved, utf8_path) && !utf8_path.empty()) {
                    try {
                        if (utf8_path.find('\0') != std::string::npos) throw std::runtime_error("path contains NUL");
                        candidates.push_back(std::filesystem::u8path(utf8_path));
                    } catch (const std::exception&) {
                        candidate_error = "Saved Oodle path is invalid UTF-8; rerun --configure-oodle with the game's binaries folder or DLL";
                    }
                }
            }
            // cant ship oodle ourselves so just go look where steam usually puts the game
            const auto program_files = env_path(L"ProgramFiles(x86)");
            if (!program_files.empty()) candidates.push_back(program_files / L"Steam" / L"steamapps" / L"common" / L"Warhammer 40,000 DARKTIDE" / L"binaries" / L"oo2core_9_win64.dll");
        }

        for (const auto& raw_candidate : candidates) {
            const auto candidate = oodle_dll_from_input(raw_candidate);
            DWORD load_error = ERROR_SUCCESS;
            module_ = load_oodle_module(candidate, &load_error);
            if (!module_) {
                if (explicit_override) candidate_error = "Explicit Oodle location could not be loaded at " + candidate.string() +
                    " (Windows error " + std::to_string(load_error) + ")";
                continue;
            }
            bound_ = reinterpret_cast<BoundFn>(GetProcAddress(module_, "OodleLZ_GetCompressedBufferSizeNeeded"));
            compress_ = reinterpret_cast<CompressFn>(GetProcAddress(module_, "OodleLZ_Compress"));
            decompress_ = reinterpret_cast<DecompressFn>(GetProcAddress(module_, "OodleLZ_Decompress"));
            if (bound_ && compress_ && decompress_) return true;
            candidate_error = "Oodle DLL is missing required exports at " + candidate.string();
            FreeLibrary(module_); module_ = nullptr;
        }
        error = "Darktide Oodle DLL was not found or could not be loaded. Run --configure-oodle <game directory or oo2core_9_win64.dll>; "
            "or set DARKTIDE_OODLE_DLL / DARKTIDE_GAME_ROOT. Expected oo2core_9_win64.dll under the game's binaries directory.";
        if (explicit_override) error = candidate_error + ". Correct DARKTIDE_OODLE_DLL or DARKTIDE_GAME_ROOT and retry.";
        else if (!candidate_error.empty()) error = candidate_error + ". Reconfigure with the installed game's oo2core_9_win64.dll.";
        return false;
    }

    bool compress_roundtrip(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& packed, const char* label, std::string& error) const {
        if (input.empty() || input.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max())) { error = std::string("invalid Oodle input for ") + label; return false; }
        constexpr int kKraken = 8;
        constexpr int kNormal = 4;
        const auto bound = bound_(kKraken, static_cast<long long>(input.size()));
        if (!bound) { error = std::string("Oodle returned an invalid compression bound for ") + label; return false; }
        std::vector<std::uint8_t> output(bound);
        const auto written = compress_(kKraken, input.data(), static_cast<long long>(input.size()), output.data(), kNormal, nullptr, nullptr, nullptr, nullptr, 0);
        if (written <= 0 || static_cast<std::size_t>(written) > output.size()) { error = std::string("Oodle compression failed for ") + label; return false; }
        output.resize(static_cast<std::size_t>(written));
        std::vector<std::uint8_t> restored(input.size());
        const auto decoded = decompress_(output.data(), written, restored.data(), static_cast<long long>(restored.size()), 1, 1, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        if (decoded != static_cast<long long>(input.size()) || restored != input) { error = std::string("Oodle round-trip validation failed for ") + label; return false; }
        packed = std::move(output);
        return true;
    }

    bool decompress(const std::uint8_t* input, std::size_t size, std::vector<std::uint8_t>& output, std::string& error) const {
        const auto decoded = decompress_(input, static_cast<long long>(size), output.data(), static_cast<long long>(output.size()),
                                         1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        if (decoded != static_cast<long long>(output.size())) { error = "Oodle could not unpack the texture"; return false; }
        return true;
    }

private:
    HMODULE module_ = nullptr;
    BoundFn bound_ = nullptr;
    CompressFn compress_ = nullptr;
    DecompressFn decompress_ = nullptr;
};
#endif

std::vector<std::vector<std::uint8_t>> tile_surface(std::uint32_t width,std::uint32_t height,const std::vector<std::uint8_t>& payload,std::uint32_t block,std::string& error){
    const std::uint32_t bw=width/4,bh=height/4,cbw=block==8?128u:64u,cbh=64;if(!surface_is_tileable(width,height,block)){error="external texture mip does not divide into 64 KiB chunks";return{};}const std::size_t row=static_cast<std::size_t>(bw)*block,crow=static_cast<std::size_t>(cbw)*block;std::vector<std::vector<std::uint8_t>> chunks;for(std::uint32_t by=0;by<bh;by+=cbh)for(std::uint32_t bx=0;bx<bw;bx+=cbw){std::vector<std::uint8_t> c;c.reserve(65536);const auto x=static_cast<std::size_t>(bx)*block;for(std::uint32_t r=0;r<cbh;++r){const auto s=static_cast<std::size_t>(by+r)*row+x;c.insert(c.end(),payload.begin()+static_cast<std::ptrdiff_t>(s),payload.begin()+static_cast<std::ptrdiff_t>(s+crow));}if(c.size()!=65536){error="external texture chunk is not 64 KiB";return{};}chunks.push_back(std::move(c));}return chunks;
}

bool write_file(const std::filesystem::path&p,const std::vector<std::uint8_t>&b,std::string&error){std::error_code ec;std::filesystem::create_directories(p.parent_path(),ec);std::ofstream f(p,std::ios::binary);if(!f){error="cannot open texture output: "+p.string();return false;}f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));if(!f){error="failed writing texture output: "+p.string();return false;}return true;}

} // namespace

bool oodle_compress(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& packed, std::string& error) {
#ifdef _WIN32
    DarktideOodle oodle;
    return oodle.load(error) && oodle.compress_roundtrip(input, packed, "shader", error);
#else
    (void)input; (void)packed;
    error = "Oodle compression requires Darktide's Windows Oodle DLL (unsupported on this platform)";
    return false;
#endif
}

bool configure_oodle(const std::filesystem::path& dll_or_game_directory, std::string& error) {
#ifdef _WIN32
    const auto dll = oodle_dll_from_input(dll_or_game_directory);
    if (!valid_oodle_dll(dll, error)) return false;
    const auto config = oodle_config_file();
    if (config.empty()) {
        error = "LOCALAPPDATA is unavailable; set DARKTIDE_OODLE_CONFIG_DIR to choose a configuration directory";
        return false;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(dll, ec).lexically_normal();
    if (ec) { error = "could not resolve Oodle DLL path: " + ec.message(); return false; }
    std::filesystem::create_directories(config.parent_path(), ec);
    if (ec) { error = "could not create Oodle configuration directory: " + ec.message(); return false; }
    const auto encoded = absolute.u8string();
    const auto temporary = config.parent_path() / (L"oodle_path.txt.tmp." + std::to_wstring(GetCurrentProcessId()));
    std::ofstream saved(temporary, std::ios::binary | std::ios::trunc);
    if (!saved || !saved.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())) || !saved.put('\n')) {
        saved.close();
        std::filesystem::remove(temporary, ec);
        error = "could not write temporary Oodle configuration file: " + config.parent_path().string();
        return false;
    }
    saved.flush();
    saved.close();
    if (!saved) {
        std::filesystem::remove(temporary, ec);
        error = "could not finish writing temporary Oodle configuration file: " + config.parent_path().string();
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), config.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        std::filesystem::remove(temporary, ec);
        error = "could not replace Oodle configuration file (Windows error " + std::to_string(move_error) + ")";
        return false;
    }
    return true;
#else
    (void)dll_or_game_directory;
    error = "Oodle configuration is available only on Windows";
    return false;
#endif
}

std::filesystem::path game_root() {
#ifdef _WIN32
    // same places the oodle dll is looked up, the dll lives in <game>/binaries
    const auto env_path = [](const wchar_t* name) -> std::filesystem::path {
        const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (!needed) return {};
        std::vector<wchar_t> value(needed);
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);
        return copied && copied < needed ? std::filesystem::path(value.data()) : std::filesystem::path{};
    };
    std::error_code ec;
    if (auto root = env_path(L"DARKTIDE_GAME_ROOT"); !root.empty()) return root;
    if (const auto config = oodle_config_file(); !config.empty()) {
        std::ifstream saved(config, std::ios::binary);
        std::string utf8_path;
        if (saved && std::getline(saved, utf8_path) && !utf8_path.empty() && utf8_path.find('\0') == std::string::npos) {
            const auto dll = std::filesystem::u8path(utf8_path);
            const auto root = dll.parent_path().parent_path();
            if (std::filesystem::is_directory(root / L"bundle", ec)) return root;
        }
    }
    if (const auto program_files = env_path(L"ProgramFiles(x86)"); !program_files.empty())
        return program_files / L"Steam" / L"steamapps" / L"common" / L"Warhammer 40,000 DARKTIDE";
#endif
    return {};
}

const TextureProfile& substance_basic_bc_profile(){return kSubstanceBc;}
const TextureProfile& substance_basic_bca_profile(){return kSubstanceBca;}
const TextureProfile& substance_basic_nm_profile(){return kSubstanceNm;}
const TextureProfile& substance_basic_orm_profile(){return kSubstanceOrm;}
const TextureProfile& pbr_emissive_em_profile(){return kPbrEmissiveEm;}
bool decode_image_rgba(const std::vector<std::uint8_t>& bytes,const std::string& mime,ImageRGBA& out,std::string& error){
    if(bytes.empty()){error="image payload is empty";return false;}
    static constexpr std::uint8_t ktx2_signature[] = {
        0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    const bool ktx2 = mime == "image/ktx2" ||
        (bytes.size() >= sizeof(ktx2_signature) &&
         std::memcmp(bytes.data(), ktx2_signature, sizeof(ktx2_signature)) == 0);
    if (ktx2) return decode_ktx2_rgba(bytes, out, error);
    const bool webp = mime == "image/webp" ||
        (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
         std::memcmp(bytes.data() + 8, "WEBP", 4) == 0);
    if (webp) {
        int width = 0, height = 0;
        if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height) || width <= 0 || height <= 0) {
            error = "WebP decode header failed";
            return false;
        }
        const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (pixel_count > std::numeric_limits<std::size_t>::max() / 4) {
            error = "WebP image is too large";
            return false;
        }
        ImageRGBA decoded;
        decoded.width = static_cast<std::uint32_t>(width);
        decoded.height = static_cast<std::uint32_t>(height);
        try {
            decoded.pixels.resize(pixel_count * 4);
        } catch (const std::bad_alloc&) {
            error = "WebP RGBA allocation failed";
            return false;
        } catch (const std::length_error&) {
            error = "WebP RGBA allocation is too large";
            return false;
        }
        if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), decoded.pixels.data(), decoded.pixels.size(), width * 4)) {
            error = "WebP decode failed";
            return false;
        }
        out = std::move(decoded);
        return true;
    }
#if defined(_WIN32) && defined(DTGLB_USE_DIRECTXTEX)
    const bool dds = mime == "image/vnd-ms.dds" ||
        (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0);
    if (dds) {
        const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = coinit == S_OK || coinit == S_FALSE;
        if (FAILED(coinit) && coinit != RPC_E_CHANGED_MODE) {
            error = "DDS COM initialization failed";
            return false;
        }
        const auto finish = [&]() { if (uninitialize) CoUninitialize(); };
        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage loaded, rgba;
        const auto* source = reinterpret_cast<const std::byte*>(bytes.data());
        HRESULT hr = DirectX::LoadFromDDSMemory(source, bytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, loaded);
        if (SUCCEEDED(hr) && (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.depth != 1 || metadata.mipLevels == 0))
            hr = E_NOTIMPL;
        const DirectX::Image* image = SUCCEEDED(hr) ? loaded.GetImage(0, 0, 0) : nullptr;
        if (SUCCEEDED(hr) && !image) hr = E_FAIL;
        if (SUCCEEDED(hr) && DirectX::IsCompressed(image->format)) {
            hr = DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, rgba);
            image = SUCCEEDED(hr) ? rgba.GetImage(0, 0, 0) : nullptr;
        } else if (SUCCEEDED(hr) && image->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            hr = DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f, rgba);
            image = SUCCEEDED(hr) ? rgba.GetImage(0, 0, 0) : nullptr;
        }
        if (FAILED(hr) || !image || image->width == 0 || image->height == 0 || !image->pixels ||
            image->width > std::numeric_limits<std::uint32_t>::max() ||
            image->height > std::numeric_limits<std::uint32_t>::max() ||
            image->width > std::numeric_limits<std::size_t>::max() / image->height ||
            image->width * image->height > std::numeric_limits<std::size_t>::max() / 4u) {
            finish();
            error = "DDS decode failed (only ordinary 2D textures are supported)";
            return false;
        }
        try {
            out.width = static_cast<std::uint32_t>(image->width);
            out.height = static_cast<std::uint32_t>(image->height);
            out.pixels.resize(static_cast<std::size_t>(image->width) * image->height * 4u);
            for (std::size_t y = 0; y < image->height; ++y)
                std::memcpy(out.pixels.data() + y * image->width * 4u, image->pixels + y * image->rowPitch, image->width * 4u);
        } catch (const std::bad_alloc&) {
            finish();
            error = "DDS RGBA allocation failed";
            return false;
        } catch (const std::length_error&) {
            finish();
            error = "DDS RGBA allocation is too large";
            return false;
        }
        finish();
        return true;
    }
#elif !defined(_WIN32)
    if (mime == "image/vnd-ms.dds" || (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0)) {
        error = "DDS decode unsupported on this platform (DirectXTex is Windows-only)";
        return false;
    }
#endif
#ifdef _WIN32
    // WIC decodes from the in-memory byte stream, so no filename conversion or
    // temporary files are needed.  CoInitializeEx may report changed mode when
    // the caller owns COM; in that case COM is still usable but must not be uninitialized here.
    const HRESULT coinit=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const bool uninitialize=coinit==S_OK||coinit==S_FALSE;
    if(FAILED(coinit)&&coinit!=RPC_E_CHANGED_MODE){error="WIC COM initialization failed";return false;}
    auto finish=[&](){if(uninitialize)CoUninitialize();};
    IWICImagingFactory* factory=nullptr; IWICStream* stream=nullptr; IWICBitmapDecoder* decoder=nullptr;
    IWICBitmapFrameDecode* frame=nullptr; IWICFormatConverter* converter=nullptr;
    auto release=[&](){if(converter)converter->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(stream)stream->Release();if(factory)factory->Release();finish();};
    HRESULT hr=bytes.size()>std::numeric_limits<DWORD>::max()?E_INVALIDARG:S_OK;
    if(SUCCEEDED(hr))hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),static_cast<DWORD>(bytes.size()));
    if(SUCCEEDED(hr))hr=factory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder);
    if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr))hr=converter->Initialize(frame,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom);
    UINT width=0,height=0;
    if(SUCCEEDED(hr))hr=converter->GetSize(&width,&height);
    const std::size_t pixel_count=static_cast<std::size_t>(width)*static_cast<std::size_t>(height);
    if(SUCCEEDED(hr)&&(width==0||height==0||width>std::numeric_limits<UINT>::max()/4||pixel_count>std::numeric_limits<std::size_t>::max()/4||pixel_count*4>std::numeric_limits<UINT>::max()))hr=E_OUTOFMEMORY;
    ImageRGBA r;
    if(SUCCEEDED(hr)){
        try{
            r.width=width;r.height=height;r.pixels.resize(pixel_count*4);
            const UINT stride=width*4;
            hr=converter->CopyPixels(nullptr,stride,static_cast<UINT>(r.pixels.size()),r.pixels.data());
        }catch(const std::bad_alloc&){hr=E_OUTOFMEMORY;}
    }
    if(FAILED(hr)){error=(mime=="image/jpeg")?"JPEG decode failed":"PNG decode failed";release();return false;}
    out=std::move(r);release();return true;
#else
    const bool png=(mime=="image/png")||(bytes.size()>=8&&png_sig_cmp(const_cast<png_bytep>(bytes.data()),0,8)==0);
    if(png){png_image image{};image.version=PNG_IMAGE_VERSION;if(!png_image_begin_read_from_memory(&image,bytes.data(),bytes.size())){error="PNG decode header failed";return false;}image.format=PNG_FORMAT_RGBA;ImageRGBA r;r.width=image.width;r.height=image.height;r.pixels.resize(PNG_IMAGE_SIZE(image));if(!png_image_finish_read(&image,nullptr,r.pixels.data(),0,nullptr)){error="PNG decode failed";png_image_free(&image);return false;}png_image_free(&image);out=std::move(r);return true;}
    const bool jpeg=(mime=="image/jpeg")||(bytes.size()>=2&&bytes[0]==0xff&&bytes[1]==0xd8);
    if(jpeg){jpeg_decompress_struct cinfo{};jpeg_error_mgr jerr{};cinfo.err=jpeg_std_error(&jerr);jpeg_create_decompress(&cinfo);jpeg_mem_src(&cinfo,const_cast<unsigned char*>(bytes.data()),static_cast<unsigned long>(bytes.size()));if(jpeg_read_header(&cinfo,TRUE)!=JPEG_HEADER_OK){jpeg_destroy_decompress(&cinfo);error="JPEG header decode failed";return false;}cinfo.out_color_space=JCS_RGB;jpeg_start_decompress(&cinfo);ImageRGBA r;r.width=cinfo.output_width;r.height=cinfo.output_height;r.pixels.resize(static_cast<std::size_t>(r.width)*r.height*4);std::vector<std::uint8_t> row(static_cast<std::size_t>(r.width)*3);while(cinfo.output_scanline<cinfo.output_height){JSAMPROW p=row.data();jpeg_read_scanlines(&cinfo,&p,1);const auto y=cinfo.output_scanline-1;for(std::uint32_t x=0;x<r.width;++x){const auto s=static_cast<std::size_t>(x)*3,d=(static_cast<std::size_t>(y)*r.width+x)*4;r.pixels[d]=row[s];r.pixels[d+1]=row[s+1];r.pixels[d+2]=row[s+2];r.pixels[d+3]=255;}}jpeg_finish_decompress(&cinfo);jpeg_destroy_decompress(&cinfo);out=std::move(r);return true;}
    error="native automatic material path currently decodes PNG and JPEG images";return false;
#endif
}

bool normalize_image_for_family(const ImageRGBA& source,const TextureProfile& profile,ImageRGBA& out,bool& changed,std::string& error){
    if(source.width==0||source.height==0||static_cast<std::size_t>(source.width)>std::numeric_limits<std::size_t>::max()/static_cast<std::size_t>(source.height)||static_cast<std::size_t>(source.width)*source.height>std::numeric_limits<std::size_t>::max()/4||source.pixels.size()!=static_cast<std::size_t>(source.width)*source.height*4){error="invalid RGBA image";return false;}
    const auto target=[](std::uint32_t v){std::uint32_t p=1;while(p<v&&p<8192)p<<=1;return std::clamp(p,512u,8192u);};
    const auto dw=target(source.width),dh=target(source.height); changed=dw!=source.width||dh!=source.height;
    if(!changed){try{out=source;}catch(const std::bad_alloc&){error="image copy allocation failed";return false;}catch(const std::length_error&){error="image copy allocation is too large";return false;}return true;}
    const bool srgb=profile.filtering==TextureFiltering::ColorSrgb;
    const bool normal=profile.filtering==TextureFiltering::Normal;
    try{out.width=dw;out.height=dh;out.pixels.resize(static_cast<std::size_t>(dw)*dh*4);}catch(const std::bad_alloc&){error="normalized RGBA allocation failed";return false;}catch(const std::length_error&){error="normalized RGBA allocation is too large";return false;}
    const auto sample=[&](std::uint32_t x,std::uint32_t y,int c){return source.pixels[(static_cast<std::size_t>(y)*source.width+x)*4+c]/255.0f;};
    for(std::uint32_t y=0;y<dh;++y) for(std::uint32_t x=0;x<dw;++x){
        const double fx=(static_cast<double>(x)+0.5)*source.width/dw-0.5,fy=(static_cast<double>(y)+0.5)*source.height/dh-0.5;
        const auto x0=static_cast<std::uint32_t>(std::clamp(std::floor(fx),0.0,static_cast<double>(source.width-1))),y0=static_cast<std::uint32_t>(std::clamp(std::floor(fy),0.0,static_cast<double>(source.height-1)));
        const auto x1=std::min(source.width-1,x0+1),y1=std::min(source.height-1,y0+1); const float tx=static_cast<float>(std::clamp(fx-static_cast<double>(x0),0.0,1.0)),ty=static_cast<float>(std::clamp(fy-static_cast<double>(y0),0.0,1.0));
        auto interp=[&](int c){const float a=sample(x0,y0,c)*(1-tx)+sample(x1,y0,c)*tx,b=sample(x0,y1,c)*(1-tx)+sample(x1,y1,c)*tx;return a*(1-ty)+b*ty;};
        auto interp_srgb=[&](int c){const float a=srgb_to_linear(source.pixels[(static_cast<std::size_t>(y0)*source.width+x0)*4+c])*(1-tx)+srgb_to_linear(source.pixels[(static_cast<std::size_t>(y0)*source.width+x1)*4+c])*tx,b=srgb_to_linear(source.pixels[(static_cast<std::size_t>(y1)*source.width+x0)*4+c])*(1-tx)+srgb_to_linear(source.pixels[(static_cast<std::size_t>(y1)*source.width+x1)*4+c])*tx;return a*(1-ty)+b*ty;};
        const auto q=(static_cast<std::size_t>(y)*dw+x)*4;
        if(normal){double vx=0,vy=0,vz=0;for(int yy=0;yy<2;++yy)for(int xx=0;xx<2;++xx){const float w=(xx?tx:1-tx)*(yy?ty:1-ty);double nx=sample(xx?x1:x0,yy?y1:y0,0)*2.0-1.0,ny=sample(xx?x1:x0,yy?y1:y0,1)*2.0-1.0,nz=sample(xx?x1:x0,yy?y1:y0,2)*2.0-1.0;const double nlen=std::sqrt(nx*nx+ny*ny+nz*nz);if(nlen>0){nx/=nlen;ny/=nlen;nz/=nlen;}else{nx=0;ny=0;nz=1;}vx+=w*nx;vy+=w*ny;vz+=w*nz;}const double len=std::sqrt(vx*vx+vy*vy+vz*vz);if(len>0){vx/=len;vy/=len;vz/=len;}else{vx=0;vy=0;vz=1;}out.pixels[q]=static_cast<std::uint8_t>(std::clamp<long>(std::lround((vx+1)*127.5),0,255));out.pixels[q+1]=static_cast<std::uint8_t>(std::clamp<long>(std::lround((vy+1)*127.5),0,255));out.pixels[q+2]=static_cast<std::uint8_t>(std::clamp<long>(std::lround((vz+1)*127.5),0,255));}
        else for(int c=0;c<3;++c) out.pixels[q+c]=srgb?linear_to_srgb(interp_srgb(c)):static_cast<std::uint8_t>(std::clamp<long>(std::lround(interp(c)*255),0,255));
        out.pixels[q+3]=static_cast<std::uint8_t>(std::clamp<long>(std::lround(interp(3)*255),0,255));
    }
    return true;
}

bool normalize_image_for_family(const ImageRGBA& source,ImageRGBA& out,bool& changed,std::string& error){
    return normalize_image_for_family(source,substance_basic_bc_profile(),out,changed,error);
}

std::string make_texture_stream_name(const std::string& resource_name){std::ostringstream ss;ss<<std::hex<<std::nouppercase<<std::setfill('0')<<std::setw(16)<<id64("texture-stream:"+resource_name);return "data/am/"+ss.str()+".stream";}

bool write_native_texture_rgba(const ImageRGBA& source,const TextureProfile& profile,const std::string& resource,const std::filesystem::path& texture_path,const std::filesystem::path& stream_path,CookedTexture& out,std::string& error){
#ifdef _WIN32
    DarktideOodle oodle;if(!oodle.load(error))return false;
#else
    error="native texture writing requires Darktide's Windows Oodle DLL (unsupported on this platform)";return false;
#endif
    ImageRGBA base;bool normalized=false;if(!normalize_image_for_family(source,profile,base,normalized,error))return false;const auto dims=mip_dims(base.width,base.height);if(dims.empty()||dims.size()>16){error="texture mip count is outside 1..16";return false;}const auto sc=streamed_mips(dims,profile.block_bytes);if(sc==std::numeric_limits<std::size_t>::max()){error="texture has no resident mip tail";return false;}
    std::vector<SurfaceMip> mips;mips.reserve(dims.size());ImageRGBA cur=base;for(std::size_t i=0;i<dims.size();++i){std::string e;auto enc=encode_surface(cur,profile,e);if(enc.empty()&&!e.empty()){error=e;return false;}mips.push_back({cur.width,cur.height,std::move(enc)});if(i+1<dims.size()){if(profile.filtering==TextureFiltering::Normal)cur=downsample_normal(cur);else if(profile.filtering==TextureFiltering::ColorSrgb)cur=downsample_srgb(cur);else cur=downsample_linear(cur);}}
    std::vector<SurfaceMip> resident(mips.begin()+static_cast<std::ptrdiff_t>(sc),mips.end());const auto dds=build_dds(resident.front().width,resident.front().height,profile,resident);std::vector<std::uint8_t> packed_dds;
#ifdef _WIN32
    if(!oodle.compress_roundtrip(dds,packed_dds,"resident DDS",error))return false;
#endif
    std::vector<std::uint8_t> stream;std::vector<std::uint32_t> cumulative;for(std::size_t i=0;i<sc;++i){auto chunks=tile_surface(mips[i].width,mips[i].height,mips[i].bytes,profile.block_bytes,error);if(!error.empty())return false;for(std::size_t j=0;j<chunks.size();++j){std::vector<std::uint8_t> packed;
#ifdef _WIN32
        const auto label="streamed mip "+std::to_string(i)+" chunk "+std::to_string(j);if(!oodle.compress_roundtrip(chunks[j],packed,label.c_str(),error))return false;
#endif
        if(stream.size()+packed.size()>0xffffffffull){error="compressed texture stream exceeds u32 range";return false;}stream.insert(stream.end(),packed.begin(),packed.end());cumulative.push_back(static_cast<std::uint32_t>(stream.size()));}}
    std::vector<std::uint8_t> body;append(body,std::uint32_t{1});append(body,static_cast<std::uint32_t>(packed_dds.size()));append(body,static_cast<std::uint32_t>(dds.size()));append_bytes(body,packed_dds);append(body,std::uint32_t{67});append(body,profile.body_flags);append(body,static_cast<std::uint32_t>(sc));append(body,base.width);append(body,base.height);
    std::uint32_t ext=0,res=0;for(std::size_t i=0;i<16;++i){std::uint32_t off=0,sz=0;if(i<mips.size()){sz=static_cast<std::uint32_t>(mips[i].bytes.size());if(i<sc){off=ext;ext+=sz;}else{off=res;res+=sz;}}append(body,off);append(body,sz);}append(body,static_cast<std::uint32_t>(8+4*cumulative.size()));append(body,static_cast<std::uint32_t>(cumulative.size()));append(body,std::uint16_t{0});append(body,static_cast<std::uint16_t>(cumulative.size()));for(auto v:cumulative)append(body,v);append(body,profile.footer_word);
    const auto stream_name=make_texture_stream_name(resource);auto cooked=wrap_cooked_resource("texture",resource,body,stream_name);
    std::string inspection;
    if(!inspect_texture_blob(cooked,inspection,error)){error="internal texture validation failed: "+error;return false;}
    if(!write_file(texture_path,cooked,error)||!write_file(stream_path,stream,error))return false;out={resource,stream_name,texture_path,stream_path,profile.id,source.width,source.height,base.width,base.height,static_cast<std::uint32_t>(mips.size()),static_cast<std::uint32_t>(sc),static_cast<std::uint32_t>(cumulative.size()),static_cast<std::uint32_t>(dds.size()),normalized};return true;
}

bool texture_body_as_kind1(const std::vector<std::uint8_t>& body,std::vector<std::uint8_t>& out,std::string& error){
    auto u32=[&](std::size_t at){std::uint32_t v=0;std::memcpy(&v,body.data()+at,4);return v;};
    if(body.size()<12){error="texture body is truncated";return false;}
    if(u32(0)==1){out=body;return true;}
    // kind 0: [0][0][0] DDS, 148-byte mip block (marker 67), one zero word, footer
    constexpr std::size_t tail=148+4+4;
    if(u32(0)!=0||u32(4)||u32(8)||body.size()<12+128+tail||std::memcmp(body.data()+12,"DDS ",4)!=0||
       u32(body.size()-tail)!=67||u32(body.size()-8)!=0){error="texture body is neither kind 0 nor kind 1";return false;}
    const std::vector<std::uint8_t> dds(body.begin()+12,body.end()-static_cast<std::ptrdiff_t>(tail));
#ifdef _WIN32
    DarktideOodle oodle;if(!oodle.load(error))return false;
    std::vector<std::uint8_t> packed;if(!oodle.compress_roundtrip(dds,packed,"texture DDS",error))return false;
#else
    error="repacking textures requires Darktide's Windows Oodle DLL";return false;
#endif
    out.clear();append(out,std::uint32_t{1});append(out,static_cast<std::uint32_t>(packed.size()));append(out,static_cast<std::uint32_t>(dds.size()));append_bytes(out,packed);
    out.insert(out.end(),body.end()-static_cast<std::ptrdiff_t>(tail),body.end()-8);
    append(out,std::uint32_t{8});append(out,std::uint32_t{0});append(out,std::uint32_t{0}); // chunk meta: none
    out.insert(out.end(),body.end()-4,body.end());
    return true;
}

bool encode_texture_like(const ImageRGBA& source, const std::vector<std::uint8_t>& game_body,
                         std::vector<std::uint8_t>& body, std::string& error) {
#ifdef DTGLB_USE_DIRECTXTEX
    const auto u32 = [&](std::size_t at) { std::uint32_t v = 0; std::memcpy(&v, game_body.data() + at, 4); return v; };
    if (game_body.size() < 12 + 156) { error = "the game's texture body is truncated"; return false; }
    // kind 1: [1][packed][dds size] Oodle-packed DDS; kind 0: [0][0][0] plain DDS. Then the mip block (marker 67),
    // the chunk metadata and the footer.
    DarktideOodle oodle;
    if (!oodle.load(error)) return false;
    std::vector<std::uint8_t> dds;
    std::size_t tail = 0;
    if (u32(0) == 1) {
        const std::size_t packed = u32(4);
        dds.resize(u32(8));
        if (12 + packed + 148 > game_body.size() || !oodle.decompress(game_body.data() + 12, packed, dds, error)) return false;
        tail = 12 + packed;
    } else {
        tail = game_body.size() - 156;
        dds.assign(game_body.begin() + 12, game_body.begin() + static_cast<std::ptrdiff_t>(tail));
    }
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || u32(tail) != 67) {
        error = "the game's texture does not hold a DDS image";
        return false;
    }
    const auto d32 = [&](std::size_t at) { std::uint32_t v = 0; std::memcpy(&v, dds.data() + at, 4); return v; };
    const std::uint32_t height = d32(12), width = d32(16), mip_count = std::max(1u, d32(28));
    const std::uint32_t body_flags = u32(tail + 4), footer = u32(game_body.size() - 4);
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    const std::uint32_t fourcc = d32(84);
    if (std::memcmp(dds.data() + 84, "DX10", 4) == 0 && dds.size() >= 148) {
        format = static_cast<DXGI_FORMAT>(d32(128));
    } else {
        // legacy DDS pixel formats the game uses
        switch (fourcc) {
        case 111: format = DXGI_FORMAT_R16_FLOAT; break;
        case 112: format = DXGI_FORMAT_R16G16_FLOAT; break;
        case 113: format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case 114: format = DXGI_FORMAT_R32_FLOAT; break;
        case 115: format = DXGI_FORMAT_R32G32_FLOAT; break;
        case 116: format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case 0x31545844: format = DXGI_FORMAT_BC1_UNORM; break; // DXT1
        case 0x33545844: format = DXGI_FORMAT_BC2_UNORM; break; // DXT3
        case 0x35545844: format = DXGI_FORMAT_BC3_UNORM; break; // DXT5
        case 0x31495441: format = DXGI_FORMAT_BC4_UNORM; break; // ATI1
        case 0x32495441: format = DXGI_FORMAT_BC5_UNORM; break; // ATI2
        case 0: {
            const std::uint32_t bits = d32(88), red = d32(92), alpha = d32(104);
            if (bits == 32) format = red == 0xff ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            else if (bits == 8) format = alpha ? DXGI_FORMAT_A8_UNORM : DXGI_FORMAT_R8_UNORM;
            else if (bits == 16) format = DXGI_FORMAT_R8G8_UNORM;
            break;
        }
        default: break;
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN || !width || !height) {
        error = "the game's texture uses a pixel format the compiler can't write";
        return false;
    }
    const bool srgb = DirectX::IsSRGB(format);
    DirectX::Image image{source.width, source.height, srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                         static_cast<std::size_t>(source.width) * 4, static_cast<std::size_t>(source.width) * source.height * 4,
                         const_cast<std::uint8_t*>(source.pixels.data())};
    DirectX::ScratchImage sized, chain, encoded;
    const auto filter = DirectX::TEX_FILTER_FORCE_NON_WIC | (srgb ? DirectX::TEX_FILTER_SRGB : DirectX::TEX_FILTER_DEFAULT);
    HRESULT hr = DirectX::Resize(image, width, height, filter, sized);
    if (SUCCEEDED(hr)) {
        hr = mip_count > 1 ? DirectX::GenerateMipMaps(*sized.GetImage(0, 0, 0), filter, mip_count, chain)
                           : chain.InitializeFromImage(*sized.GetImage(0, 0, 0));
    }
    if (SUCCEEDED(hr)) {
        hr = DirectX::IsCompressed(format)
            ? DirectX::Compress(chain.GetImages(), chain.GetImageCount(), chain.GetMetadata(), format,
                                srgb ? DirectX::TEX_COMPRESS_SRGB : DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, encoded)
            : DirectX::Convert(chain.GetImages(), chain.GetImageCount(), chain.GetMetadata(), format,
                               DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, encoded);
    }
    if (FAILED(hr)) {
        std::ostringstream ss;
        ss << "DirectXTex could not encode the texture (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        error = ss.str();
        return false;
    }
    // DDS (DX10 header) holding every mip, then the game's mip block with the resident mip table
    std::vector<std::uint8_t> header(148, 0);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(header.data() + at, &value, 4); };
    std::memcpy(header.data(), "DDS ", 4);
    put(4, 124); put(8, 0x81007u | (mip_count > 1 ? 0x20000u : 0u)); put(12, height); put(16, width);
    put(20, static_cast<std::uint32_t>(encoded.GetImage(0, 0, 0)->slicePitch));
    put(28, mip_count); put(76, 32); put(80, 4); std::memcpy(header.data() + 84, "DX10", 4);
    put(108, 0x1000u | (mip_count > 1 ? 0x400008u : 0u)); put(128, static_cast<std::uint32_t>(format)); put(132, 3); put(140, 1);
    std::vector<std::uint8_t> resident = header, table;
    std::uint32_t offset = 0;
    for (std::uint32_t i = 0; i < 16; ++i) {
        std::uint32_t size = 0;
        if (i < mip_count) {
            const DirectX::Image* mip = encoded.GetImage(i, 0, 0);
            if (!mip) { error = "DirectXTex returned fewer mips than the game's texture has"; return false; }
            resident.insert(resident.end(), mip->pixels, mip->pixels + mip->slicePitch);
            size = static_cast<std::uint32_t>(mip->slicePitch);
        }
        append(table, size ? offset : 0u);
        append(table, size);
        offset += size;
    }
    std::vector<std::uint8_t> packed;
    if (!oodle.compress_roundtrip(resident, packed, "texture", error)) return false;
    body.clear();
    append(body, std::uint32_t{1}); append(body, static_cast<std::uint32_t>(packed.size()));
    append(body, static_cast<std::uint32_t>(resident.size())); append_bytes(body, packed);
    append(body, std::uint32_t{67}); append(body, body_flags); append(body, std::uint32_t{0});
    append(body, width); append(body, height); append_bytes(body, table);
    append(body, std::uint32_t{8}); append(body, std::uint32_t{0}); append(body, std::uint32_t{0}); // no streamed chunks
    append(body, footer);
    return true;
#else
    (void)source; (void)game_body; (void)body;
    error = "writing textures like the game's needs the Windows build (DirectXTex and Darktide's Oodle DLL)";
    return false;
#endif
}

bool inspect_texture_blob(const std::vector<std::uint8_t>& blob,std::string& report,std::string& error){
    std::vector<std::uint8_t> b;
    std::string stream_name;
    if(!parse_cooked_resource_envelope(blob,"texture",b,stream_name,error))return false;
    if(b.size()<12){error="texture body truncated";return false;}
    auto u32=[&](std::size_t at){std::uint32_t v=0;std::memcpy(&v,b.data()+at,4);return v;};
    const std::uint32_t kind=u32(0),packed=u32(4),resident=u32(8);
    if(kind!=1){error="unsupported texture payload kind (expected current kind 1)";return false;}
    if(!packed||resident<148){error="kind-1 texture has invalid compressed or decompressed resident DDS sizes";return false;}
    if(static_cast<std::size_t>(packed)>b.size()-12){error="texture resident payload is truncated";return false;}
    std::size_t p=12u+packed;
    if(p>b.size()||b.size()-p<20u+128u+16u){error="texture body metadata is truncated";return false;}
    const std::uint32_t marker=u32(p),flags=u32(p+4),streamed=u32(p+8),w=u32(p+12),h=u32(p+16);
    if(marker!=67||!w||!h||streamed>16){error="texture marker, dimensions, or streamed mip count is invalid";return false;}

    std::uint32_t block=0;
    const std::uint32_t footer=u32(b.size()-4);
    if(flags==0x101u&&footer==0xa88c22b7u)block=8u;
    else if(flags==0x101u&&footer==0x6bd03744u)block=16u;
    else if(flags==0x001u&&footer==0x3390aba5u)block=16u;
    else if(flags==0x001u&&(footer==0xe514d1dcu||footer==0xae01e4bcu))block=8u;
    if(!block){error="kind-1 texture flags/footer do not match a known profile";return false;}
    std::size_t external_bytes=0,resident_payload_bytes=0,mip_count=0;
    bool saw_empty=false;
    for(std::size_t i=0;i<16;++i){
        const auto off=u32(p+20+i*8),size=u32(p+24+i*8);
        if(!size){saw_empty=true;if(off){error="empty texture mip has a nonzero offset";return false;}continue;}
        if(saw_empty){error="texture mip table is not contiguous";return false;}
        const std::uint32_t mw=std::max(1u,w>>static_cast<unsigned>(i));
        const std::uint32_t mh=std::max(1u,h>>static_cast<unsigned>(i));
        if(block&&size!=surface_bytes(mw,mh,block)){error="texture mip byte size does not match its BC surface";return false;}
        auto& total=i<streamed?external_bytes:resident_payload_bytes;
        if(off!=total){error="texture mip offsets are not cumulative within their storage class";return false;}
        total+=size;++mip_count;
    }
    if(!mip_count||streamed>=mip_count){error="texture has no valid resident mip tail";return false;}

    if(resident!=148u+resident_payload_bytes){error="kind-1 declared resident DDS size does not match the mip table";return false;}

    p+=20u+128u;
    const std::uint32_t meta=u32(p);p+=4;
    const std::uint32_t chunks=u32(p);std::uint16_t zero=0,echo=0;std::memcpy(&zero,b.data()+p+4,2);std::memcpy(&echo,b.data()+p+6,2);p+=8;
    if(zero||echo!=chunks||meta!=8u+4u*chunks||static_cast<std::size_t>(chunks)>(b.size()-p-4)/4||p+4ull*chunks+4!=b.size()){error="texture chunk metadata invalid";return false;}
    std::uint32_t prior=0;
    for(std::size_t i=0;i<chunks;++i){const auto end=u32(p+i*4);if(end<=prior){error="texture chunk cumulative sizes are not increasing";return false;}prior=end;}
    if(external_bytes%65536u||chunks!=external_bytes/65536u){error="texture chunk count does not match 64 KiB streamed BC tiles";return false;}
    p+=4ull*chunks;
    std::ostringstream ss;ss<<"texture "<<w<<"x"<<h<<", kind "<<kind<<", mips "<<mip_count<<", streamed mips "<<streamed<<", chunks "<<chunks<<", flags 0x"<<std::hex<<flags<<", footer 0x"<<footer;report=ss.str();return true;
}

} // namespace dtglb::stingray::texture
