#include "stingray/material/shader_section.h"

#include <cstdio>
#include <cstring>

namespace dtglb::stingray::material {
namespace {

// Layout (game exe: shader resource load checks version 0x2b; shader_data and device_data readers):
//   header (12 u32: version, name, contexts offset + count, conditions offset, default data offset,
//   render config offset + count, shader_data offset + size, device_data offset + size)
//   | contexts | condition bytecode | render config ids (u64) | shader_data | pad 4 | device_data | pad 4
//   | default data block (to the end).
constexpr std::size_t kHeaderWords = 12;

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t pos;
    std::size_t end;
    bool ok = true;

    bool take(void* out, std::size_t n) {
        if (!ok || pos > end || n > end - pos) { ok = false; std::memset(out, 0, n); return false; }
        std::memcpy(out, bytes.data() + pos, n);
        pos += n;
        return true;
    }
    std::uint32_t u32() { std::uint32_t v; take(&v, 4); return v; }
    std::uint64_t u64() { std::uint64_t v; take(&v, 8); return v; }
    std::uint8_t u8() { std::uint8_t v; take(&v, 1); return v; }
    // element count guard: every element takes at least `min_size` bytes
    std::uint32_t count(std::size_t min_size) {
        const auto n = u32();
        if (ok && n > (end - pos) / min_size) ok = false;
        return ok ? n : 0;
    }
    template <std::size_t N> std::vector<std::array<std::uint32_t, N>> words() {
        std::vector<std::array<std::uint32_t, N>> out(count(4 * N));
        for (auto& item : out) for (auto& w : item) w = u32();
        return out;
    }
    std::vector<std::uint8_t> raw(std::size_t n) {
        if (!ok || n > end - pos) { ok = false; return {}; }
        std::vector<std::uint8_t> out(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return out;
    }
};

std::vector<RenderState> read_states(Reader& r) {
    std::vector<RenderState> out(r.count(13));
    for (auto& s : out) { s.key = r.u32(); s.kind = r.u8(); s.value = r.u32(); s.extra = r.u32(); }
    return out;
}

ShaderVariant read_variant(Reader& r) {
    ShaderVariant v;
    v.bytecode = r.raw(r.u32());
    v.compression = r.u32();
    v.size = r.u32();
    v.hash = r.u64();
    for (auto& list : v.constant_buffers) list = r.words<6>();
    for (auto& list : v.resources) list = r.words<7>();
    v.static_samplers = r.words<4>();
    v.inputs = r.words<3>();
    v.sampler_arrays = r.words<4>();
    return v;
}

ShaderPass read_pass(Reader& r) {
    ShaderPass p;
    p.render_states = read_states(r);
    p.sampler_states.resize(r.count(8));
    for (auto& s : p.sampler_states) { s.name = r.u32(); s.states = read_states(r); }
    for (auto& stage : p.stages) {
        stage.resize(r.count(20));
        for (auto& v : stage) v = read_variant(r);
    }
    p.records16 = r.words<4>();
    for (auto& w : p.words) w = r.u32();
    p.stream_out = r.words<5>();
    return p;
}

struct Writer {
    std::vector<std::uint8_t> out;
    void u32(std::uint32_t v) { const auto* b = reinterpret_cast<const std::uint8_t*>(&v); out.insert(out.end(), b, b + 4); }
    void u64(std::uint64_t v) { const auto* b = reinterpret_cast<const std::uint8_t*>(&v); out.insert(out.end(), b, b + 8); }
    void u8(std::uint8_t v) { out.push_back(v); }
    void raw(const std::vector<std::uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); }
    template <std::size_t N> void words(const std::vector<std::array<std::uint32_t, N>>& list) {
        u32(static_cast<std::uint32_t>(list.size()));
        for (const auto& item : list) for (const auto w : item) u32(w);
    }
    void pad4() { out.resize((out.size() + 3) & ~std::size_t{3}, 0); }
    std::uint32_t size() const { return static_cast<std::uint32_t>(out.size()); }
};

void write_states(Writer& w, const std::vector<RenderState>& states) {
    w.u32(static_cast<std::uint32_t>(states.size()));
    for (const auto& s : states) { w.u32(s.key); w.u8(s.kind); w.u32(s.value); w.u32(s.extra); }
}

void write_pass(Writer& w, const ShaderPass& p) {
    write_states(w, p.render_states);
    w.u32(static_cast<std::uint32_t>(p.sampler_states.size()));
    for (const auto& s : p.sampler_states) { w.u32(s.name); write_states(w, s.states); }
    for (const auto& stage : p.stages) {
        w.u32(static_cast<std::uint32_t>(stage.size()));
        for (const auto& v : stage) {
            w.u32(static_cast<std::uint32_t>(v.bytecode.size()));
            w.raw(v.bytecode);
            w.u32(v.compression);
            w.u32(v.size);
            w.u64(v.hash);
            for (const auto& list : v.constant_buffers) w.words(list);
            for (const auto& list : v.resources) w.words(list);
            w.words(v.static_samplers);
            w.words(v.inputs);
            w.words(v.sampler_arrays);
        }
    }
    w.words(p.records16);
    for (const auto word : p.words) w.u32(word);
    w.words(p.stream_out);
}

std::string hex(std::uint64_t value, int digits) {
    char text[24];
    std::snprintf(text, sizeof(text), "%0*llx", digits, static_cast<unsigned long long>(value));
    return text;
}

json::Value number(double value) { return json::Value::of(value); }

template <std::size_t N> json::Value word_list(const std::vector<std::array<std::uint32_t, N>>& list, bool name_first) {
    auto out = json::Value::array();
    for (const auto& item : list) {
        auto entry = json::Value::array();
        for (std::size_t i = 0; i < N; ++i)
            entry.push(i == 0 && name_first ? json::Value::of("#" + hex(item[i], 8)) : number(item[i]));
        out.push(std::move(entry));
    }
    return out;
}

} // namespace

bool decode_shader_section(const std::vector<std::uint8_t>& bytes, ShaderSection& out, std::string& error) {
    if (bytes.size() < kHeaderWords * 4) { error = "shader section is shorter than its header"; return false; }
    std::array<std::uint32_t, kHeaderWords> h{};
    std::memcpy(h.data(), bytes.data(), sizeof(h));
    const auto [version, name, contexts_offset, contexts, conditions_offset, default_offset, render_config_offset,
                render_configs, data_offset, data_size, device_offset, device_size] = h;
    if (version != 0x2b) { error = "shader section version " + std::to_string(version) + " (expected 43)"; return false; }
    const auto size = bytes.size();
    if (!(contexts_offset <= conditions_offset && conditions_offset <= render_config_offset &&
          render_config_offset <= size && data_offset <= size && data_size <= size - data_offset &&
          device_offset <= size && device_size <= size - device_offset && default_offset <= size &&
          render_configs <= (size - render_config_offset) / 8 && contexts <= (conditions_offset - contexts_offset) / 12)) {
        error = "shader section offsets point outside the section"; return false;
    }
    ShaderSection s;
    s.version = version;
    s.name = name;
    Reader r{bytes, contexts_offset, conditions_offset};
    s.contexts.resize(contexts);
    for (auto& c : s.contexts) {
        c.name = r.u32();
        c.reserved = r.u32();
        c.shaders = r.words<2>();
    }
    if (!r.ok || r.pos != conditions_offset) { error = "shader contexts do not end where the conditions start"; return false; }
    s.conditions.assign(bytes.begin() + conditions_offset, bytes.begin() + render_config_offset);
    s.render_config.resize(render_configs);
    std::memcpy(s.render_config.data(), bytes.data() + render_config_offset, 8 * render_configs);

    Reader d{bytes, data_offset, data_offset + data_size};
    s.shader_data.resize(d.count(24));
    for (auto& e : s.shader_data) {
        e.name = d.u32();
        e.resource_size = d.u32();
        e.resources = d.words<4>();
        e.constant_buffers.resize(d.count(12));
        for (auto& cb : e.constant_buffers) { cb.variables = d.words<5>(); cb.size = d.u32(); cb.offset = d.u32(); }
        e.records = d.words<7>();
        e.passes.resize(d.count(17));
        for (auto& p : e.passes) { p.layer = d.u32(); p.sort_key = d.u64(); p.value = d.u32(); p.instanced = d.u8(); }
        e.tail[0] = d.u32();
        e.tail[1] = d.u32();
    }
    if (!d.ok || d.pos != data_offset + data_size) { error = "shader_data does not parse to its size"; return false; }

    Reader v{bytes, device_offset, device_offset + device_size};
    s.device_data.resize(v.count(8));
    for (auto& shader : s.device_data) {
        shader.groups.resize(v.count(4));
        for (auto& group : shader.groups) {
            group.resize(v.count(40));
            for (auto& pass : group) pass = read_pass(v);
        }
        shader.tail = v.u32();
    }
    if (!v.ok || v.pos != device_offset + device_size) { error = "device_data does not parse to its size"; return false; }
    s.default_data.assign(bytes.begin() + default_offset, bytes.end());
    out = std::move(s);
    if (encode_shader_section(out) != bytes) { error = "shader section layout differs from the known one"; return false; }
    return true;
}

std::vector<std::uint8_t> encode_shader_section(const ShaderSection& s) {
    Writer w;
    w.out.resize(kHeaderWords * 4);
    const auto contexts_offset = w.size();
    for (const auto& c : s.contexts) { w.u32(c.name); w.u32(c.reserved); w.words(c.shaders); }
    const auto conditions_offset = w.size();
    w.raw(s.conditions);
    const auto render_config_offset = w.size();
    for (const auto id : s.render_config) w.u64(id);
    const auto data_offset = w.size();
    w.u32(static_cast<std::uint32_t>(s.shader_data.size()));
    for (const auto& e : s.shader_data) {
        w.u32(e.name);
        w.u32(e.resource_size);
        w.words(e.resources);
        w.u32(static_cast<std::uint32_t>(e.constant_buffers.size()));
        for (const auto& cb : e.constant_buffers) { w.words(cb.variables); w.u32(cb.size); w.u32(cb.offset); }
        w.words(e.records);
        w.u32(static_cast<std::uint32_t>(e.passes.size()));
        for (const auto& p : e.passes) { w.u32(p.layer); w.u64(p.sort_key); w.u32(p.value); w.u8(p.instanced); }
        w.u32(e.tail[0]);
        w.u32(e.tail[1]);
    }
    const auto data_size = w.size() - data_offset;
    w.pad4();
    const auto device_offset = w.size();
    w.u32(static_cast<std::uint32_t>(s.device_data.size()));
    for (const auto& shader : s.device_data) {
        w.u32(static_cast<std::uint32_t>(shader.groups.size()));
        for (const auto& group : shader.groups) {
            w.u32(static_cast<std::uint32_t>(group.size()));
            for (const auto& pass : group) write_pass(w, pass);
        }
        w.u32(shader.tail);
    }
    const auto device_size = w.size() - device_offset;
    w.pad4();
    const auto default_offset = w.size();
    w.raw(s.default_data);
    const std::array<std::uint32_t, kHeaderWords> h{
        s.version, s.name, contexts_offset, static_cast<std::uint32_t>(s.contexts.size()), conditions_offset,
        default_offset, render_config_offset, static_cast<std::uint32_t>(s.render_config.size()), data_offset,
        data_size, device_offset, device_size};
    std::memcpy(w.out.data(), h.data(), sizeof(h));
    return std::move(w.out);
}

json::Value describe_shader_variant(const ShaderVariant& v) {
    auto stage = json::Value::object();
    stage.set("compressed", number(static_cast<double>(v.bytecode.size())));
    stage.set("size", number(v.size));
    stage.set("hash", json::Value::of(hex(v.hash, 16)));
    stage.set("constant_buffers", word_list(v.constant_buffers[0], true));
    for (std::size_t list = 1; list < v.resources.size(); ++list) {
        static const char* names[] = {"", "srvs", "srv_arrays", "uavs", "uav_arrays"};
        if (!v.resources[list].empty()) stage.set(names[list], word_list(v.resources[list], true));
    }
    if (!v.static_samplers.empty()) stage.set("samplers", word_list(v.static_samplers, true));
    if (!v.sampler_arrays.empty()) stage.set("sampler_arrays", word_list(v.sampler_arrays, true));
    stage.set("inputs", word_list(v.inputs, true));
    return stage;
}

json::Value describe_shader_section(const ShaderSection& s) {
    auto root = json::Value::object();
    root.set("name", json::Value::of("#" + hex(s.name, 8)));
    auto contexts = json::Value::array();
    for (const auto& c : s.contexts) {
        auto context = json::Value::object();
        context.set("name", json::Value::of("#" + hex(c.name, 8)));
        auto shaders = json::Value::array();
        for (const auto& [shader, condition] : c.shaders) {
            auto entry = json::Value::object();
            entry.set("shader", json::Value::of("#" + hex(shader, 8)));
            if (condition != 0xffffffffu) entry.set("condition", number(condition));
            shaders.push(std::move(entry));
        }
        context.set("shaders", std::move(shaders));
        contexts.push(std::move(context));
    }
    root.set("contexts", std::move(contexts));
    root.set("condition_bytes", number(static_cast<double>(s.conditions.size())));
    static const char* stage_names[] = {"vertex", "domain", "hull", "geometry", "pixel", "compute"};
    auto shaders = json::Value::array();
    for (std::size_t i = 0; i < s.shader_data.size(); ++i) {
        const auto& e = s.shader_data[i];
        auto shader = json::Value::object();
        shader.set("name", json::Value::of("#" + hex(e.name, 8)));
        auto passes = json::Value::array();
        for (std::size_t p = 0; p < e.passes.size(); ++p) {
            auto pass = json::Value::object();
            pass.set("layer", json::Value::of("#" + hex(e.passes[p].layer, 8)));
            const ShaderPass* device = nullptr;
            for (const auto& d : s.device_data)
                if (i < d.groups.size() && p < d.groups[i].size()) device = &d.groups[i][p];
            if (device) {
                pass.set("render_states", number(static_cast<double>(device->render_states.size())));
                for (std::size_t k = 0; k < device->stages.size(); ++k)
                    for (const auto& v : device->stages[k]) pass.set(stage_names[k], describe_shader_variant(v));
            }
            passes.push(std::move(pass));
        }
        shader.set("passes", std::move(passes));
        shaders.push(std::move(shader));
    }
    root.set("shaders", std::move(shaders));
    return root;
}

}
