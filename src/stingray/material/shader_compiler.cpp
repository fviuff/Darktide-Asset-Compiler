#include "stingray/material/shader_compiler.h"

#include "stingray/murmur_hash.h"
#include "stingray/texture/texture_writer.h"

#include <algorithm>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#endif

namespace dtglb::stingray::material {
namespace {

std::uint32_t id32(std::string_view name) {
    // dxc names a texture / sampler pair from a combined sampler object __tex_<name> / __samp_<name>; the game's
    // records use <name>
    for (const std::string_view prefix : {"__tex_", "__samp_"})
        if (name.substr(0, prefix.size()) == prefix) name.remove_prefix(prefix.size());
    return id32_from_id64(name);
}

#ifdef _WIN32
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// dxcompiler.dll loads dxil.dll (the validator that signs the DXIL) by name, so dxil.dll is loaded from the same
// folder first.
DxcCreateInstanceProc dxc_create(std::string& error) {
    static DxcCreateInstanceProc create = nullptr;
    if (create) return create;
    std::vector<std::filesystem::path> folders;
    std::vector<wchar_t> exe(32768);
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length && length < exe.size()) folders.push_back(std::filesystem::path(exe.data(), exe.data() + length).parent_path());
    std::vector<wchar_t> kits(32768);
    const DWORD kits_length = GetEnvironmentVariableW(L"ProgramFiles(x86)", kits.data(), static_cast<DWORD>(kits.size()));
    if (kits_length && kits_length < kits.size()) {
        const auto root = std::filesystem::path(kits.data(), kits.data() + kits_length) / L"Windows Kits" / L"10";
        folders.push_back(root / L"Redist" / L"D3D" / L"x64");
        std::error_code ec;
        std::vector<std::filesystem::path> versions;
        for (const auto& entry : std::filesystem::directory_iterator(root / L"bin", ec))
            if (std::filesystem::exists(entry.path() / L"x64" / L"dxcompiler.dll", ec)) versions.push_back(entry.path() / L"x64");
        std::sort(versions.rbegin(), versions.rend());
        folders.insert(folders.end(), versions.begin(), versions.end());
    }
    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::exists(folder / L"dxcompiler.dll", ec) || !std::filesystem::exists(folder / L"dxil.dll", ec)) continue;
        if (!LoadLibraryExW((folder / L"dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) continue;
        HMODULE module = LoadLibraryExW((folder / L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) continue;
        create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
        if (create) return create;
    }
    error = "DirectX shader compiler not found: put dxcompiler.dll and dxil.dll next to DarktideGLBCompiler.exe "
            "(or install the Windows SDK)";
    return nullptr;
}
#endif

} // namespace

bool compile_hlsl(const std::string& source, const std::string& source_name, const std::string& entry,
                  const std::string& profile, std::vector<std::uint8_t>& dxbc, std::string& error) {
#ifdef _WIN32
    const auto create = dxc_create(error);
    if (!create) return false;
    Com<IDxcCompiler3> compiler;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&compiler)))) {
        error = "cannot start the DirectX shader compiler"; return false;
    }
    const auto wname = widen(source_name), wentry = widen(entry), wprofile = widen(profile);
    std::vector<LPCWSTR> args{wname.c_str(), L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-O3",
                              L"-validator-version", L"1.7"};
    const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    Com<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                 __uuidof(IDxcResult), reinterpret_cast<void**>(&result)))) {
        error = "the DirectX shader compiler failed to run"; return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    Com<IDxcBlobUtf8> messages;
    result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&messages), nullptr);
    if (FAILED(status)) {
        error = messages.p && messages->GetStringLength()
            ? std::string(messages->GetStringPointer(), messages->GetStringLength()) : "shader compile failed";
        return false;
    }
    Com<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&object), nullptr)) ||
        !object.p || !object->GetBufferSize()) {
        error = "the DirectX shader compiler returned no bytecode"; return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(object->GetBufferPointer());
    dxbc.assign(bytes, bytes + object->GetBufferSize());
    return true;
#else
    (void)source; (void)source_name; (void)entry; (void)profile; (void)dxbc;
    error = "compiling shaders needs the Windows DirectX shader compiler";
    return false;
#endif
}

bool make_shader_variant(const std::vector<std::uint8_t>& dxbc, RootTable& roots, ShaderVariant& out,
                         std::string& error) {
#ifdef _WIN32
    const auto create = dxc_create(error);
    if (!create) return false;
    Com<IDxcUtils> utils;
    Com<ID3D12ShaderReflection> reflection;
    const DxcBuffer buffer{dxbc.data(), dxbc.size(), 0};
    if (FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(&utils))) ||
        FAILED(utils->CreateReflection(&buffer, __uuidof(ID3D12ShaderReflection), reinterpret_cast<void**>(&reflection)))) {
        error = "cannot read the compiled shader's reflection"; return false;
    }
    D3D12_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    ShaderVariant v;
    const auto root = [&](std::uint32_t name) {
        const auto found = std::find(roots.begin(), roots.end(), name);
        if (found != roots.end()) return static_cast<std::uint32_t>(found - roots.begin());
        roots.push_back(name);
        return static_cast<std::uint32_t>(roots.size() - 1);
    };
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC b{};
        reflection->GetResourceBindingDesc(i, &b);
        const auto name = id32(b.Name);
        const bool unbounded = b.BindCount == 0;
        const std::uint32_t count = unbounded ? 0xffffffffu : b.BindCount;
        switch (b.Type) {
        case D3D_SIT_CBUFFER: {
            D3D12_SHADER_BUFFER_DESC cb{};
            reflection->GetConstantBufferByName(b.Name)->GetDesc(&cb);
            v.constant_buffers[0].push_back({name, root(name), cb.Size, b.BindPoint, count, b.Space});
            break;
        }
        case D3D_SIT_SAMPLER:
            if (unbounded) v.sampler_arrays.push_back({name, b.BindPoint, 0, b.Space});
            else v.static_samplers.push_back({name, b.BindPoint, count, b.Space});
            break;
        default: {
            const bool uav = b.Type == D3D_SIT_UAV_RWTYPED || b.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                b.Type == D3D_SIT_UAV_RWBYTEADDRESS || b.Type == D3D_SIT_UAV_APPEND_STRUCTURED ||
                b.Type == D3D_SIT_UAV_CONSUME_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
            const bool raw = b.Type == D3D_SIT_BYTEADDRESS || b.Type == D3D_SIT_UAV_RWBYTEADDRESS;
            const bool structured = b.Type == D3D_SIT_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                b.Type == D3D_SIT_UAV_APPEND_STRUCTURED || b.Type == D3D_SIT_UAV_CONSUME_STRUCTURED ||
                b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
            const std::uint32_t stride = raw ? 0u : structured ? b.NumSamples : 0xffffffffu;
            v.resources[(uav ? 3 : 1) + (unbounded ? 1 : 0)].push_back(
                {name, root(name), b.BindPoint, count, b.Space, stride, 0});
            break;
        }
        }
    }
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC p{};
        reflection->GetInputParameterDesc(i, &p);
        v.inputs.push_back({id32_from_id64(p.SemanticName), p.SemanticIndex, p.Register});
    }
    if (!texture::oodle_compress(dxbc, v.bytecode, error)) return false;
    v.compression = 5;
    v.size = static_cast<std::uint32_t>(dxbc.size());
    v.hash = murmur64(v.bytecode.data(), v.bytecode.size());
    out = std::move(v);
    return true;
#else
    (void)dxbc; (void)roots; (void)out;
    error = "reading shader reflection needs the Windows DirectX shader compiler";
    return false;
#endif
}

}
