#include "processing/native_profile_canonicalizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace dtglb::processing {
namespace {
constexpr float kHalfMax = 65504.0f;
using Vec3 = std::array<float, 3>;

const VertexChannel* find_channel(const std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    const auto it = std::find_if(channels.begin(), channels.end(), [=](const VertexChannel& c) { return c.semantic == semantic && c.set == set; });
    return it == channels.end() ? nullptr : &*it;
}
VertexChannel* find_channel(std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    const auto it = std::find_if(channels.begin(), channels.end(), [=](const VertexChannel& c) { return c.semantic == semantic && c.set == set; });
    return it == channels.end() ? nullptr : &*it;
}
std::set<int> active_nodes(const Scene& scene) {
    std::set<int> result; std::vector<int> pending(scene.scene_roots.begin(), scene.scene_roots.end());
    while (!pending.empty()) { const int index = pending.back(); pending.pop_back(); if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size() || !result.insert(index).second) continue; for (int child : scene.nodes[static_cast<std::size_t>(index)].children) pending.push_back(child); }
    return result;
}
void add_skin_ancestry(const Scene& scene, std::size_t skin_index, std::set<int>& nodes) {
    if (skin_index >= scene.skins.size()) return;
    for (int joint : scene.skins[skin_index].joints) { std::set<int> seen; while (joint >= 0 && static_cast<std::size_t>(joint) < scene.nodes.size() && seen.insert(joint).second) { nodes.insert(joint); joint = scene.nodes[static_cast<std::size_t>(joint)].parent; } }
}
void note(Scene& scene, NativeProfileCanonicalizationReport& report, std::size_t index, const char* code, const std::string& message, bool lossy = true) {
    report.changed = true; report.lossy = report.lossy || lossy; report.approximations.emplace_back(code); report.diagnostics.push_back({index, code, message}); scene.notes.push_back(message + " [" + code + "]");
}
bool finite_matrix(const Matrix4& matrix) { return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); }); }
Vec3 linear(const Matrix4& m, Vec3 v) { return {m[0]*v[0]+m[4]*v[1]+m[8]*v[2], m[1]*v[0]+m[5]*v[1]+m[9]*v[2], m[2]*v[0]+m[6]*v[1]+m[10]*v[2]}; }
Vec3 point(const Matrix4& m, Vec3 v) { auto r = linear(m, v); r[0]+=m[12]; r[1]+=m[13]; r[2]+=m[14]; return r; }
float determinant3(const Matrix4& m) {
    return m[0] * (m[5] * m[10] - m[9] * m[6]) -
           m[4] * (m[1] * m[10] - m[9] * m[2]) +
           m[8] * (m[1] * m[6] - m[5] * m[2]);
}
bool inverse_transpose_linear(const Matrix4& m, Vec3 v, Vec3& result) {
    const float determinant = determinant3(m);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-12f) return false;
    // The matrix is column-major; this is the inverse-transpose of its upper-left 3x3.
    result = {
        ((m[5] * m[10] - m[9] * m[6]) * v[0] + (m[9] * m[2] - m[1] * m[10]) * v[1] + (m[1] * m[6] - m[5] * m[2]) * v[2]) / determinant,
        ((m[8] * m[6] - m[4] * m[10]) * v[0] + (m[0] * m[10] - m[8] * m[2]) * v[1] + (m[4] * m[2] - m[0] * m[6]) * v[2]) / determinant,
        ((m[4] * m[9] - m[8] * m[5]) * v[0] + (m[8] * m[1] - m[0] * m[9]) * v[1] + (m[0] * m[5] - m[4] * m[1]) * v[2]) / determinant};
    return std::all_of(result.begin(), result.end(), [](float value) { return std::isfinite(value); });
}
Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 r{}; for (int c=0;c<4;++c) for (int row=0;row<4;++row) r[static_cast<std::size_t>(c*4+row)] = a[row]*b[c*4] + a[4+row]*b[c*4+1] + a[8+row]*b[c*4+2] + a[12+row]*b[c*4+3]; return r;
}
Vec3 normalized(Vec3 v) { const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); return (!std::isfinite(n)||n<=1e-12f) ? v : Vec3{v[0]/n,v[1]/n,v[2]/n}; }

bool unit_transform_supported(const Matrix4& matrix) {
    if (!finite_matrix(matrix)) return false;
    if (std::abs(matrix[3]) > 1e-6f || std::abs(matrix[7]) > 1e-6f ||
        std::abs(matrix[11]) > 1e-6f || std::abs(matrix[15] - 1.0f) > 1e-6f) return false;
    const auto length = [&](std::size_t offset) {
        return std::sqrt(matrix[offset] * matrix[offset] + matrix[offset + 1] * matrix[offset + 1] +
                         matrix[offset + 2] * matrix[offset + 2]);
    };
    const float sx = length(0), sy = length(4), sz = length(8);
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz) ||
        sx <= 1e-12f || sy <= 1e-12f || sz <= 1e-12f) return false;
    const auto dot = [&](std::size_t a, std::size_t b, float denominator) {
        return (matrix[a] * matrix[b] + matrix[a + 1] * matrix[b + 1] +
                matrix[a + 2] * matrix[b + 2]) / denominator;
    };
    return std::max({std::abs(dot(0, 4, sx * sy)), std::abs(dot(0, 8, sx * sz)),
                     std::abs(dot(4, 8, sy * sz))}) <= 2e-5f;
}

bool zero_matrix(const Matrix4& matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return value == 0.0f; });
}

void identity_node(NodeInfo& node) {
    const Matrix4 identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    node.local_stingray = identity;
    node.world_stingray = identity;
    node.local_translation_stingray = {0,0,0};
    node.local_rotation_stingray = {0,0,0,1};
    node.local_scale_stingray = {1,1,1};
    node.has_exact_trs = true;
}

bool clamp_packed_channels(std::vector<VertexChannel>& channels, std::size_t primitive_index, Scene& scene, NativeProfileCanonicalizationReport& report, std::string& error) {
    for (auto& channel : channels) {
        const std::uint32_t packed = channel.semantic == VertexChannel::Semantic::Position ? 3u : channel.semantic == VertexChannel::Semantic::Texcoord ? 2u : 0u;
        if (!packed) continue;
        if (channel.components < packed || channel.values.size() % channel.components != 0) { error="native profile POSITION/TEXCOORD channel has malformed rows"; return false; }
        bool changed=false; for (std::size_t row=0; row<channel.values.size()/channel.components; ++row) for (std::uint32_t c=0;c<packed;++c) { float& v=channel.values[row*channel.components+c]; if (!std::isfinite(v)) { error="native profile POSITION/TEXCOORD contains a non-finite value"; return false; } if (v>kHalfMax||v<-kHalfMax) { v=std::clamp(v,-kHalfMax,kHalfMax); changed=true; } }
        if (changed) note(scene, report, primitive_index, "binary16_clamp", "primitive " + std::to_string(primitive_index) + ": clamped POSITION/TEXCOORD to finite binary16 range");
    }
    return true;
}

bool valid_bake_source(const Scene& scene, const Primitive& primitive, std::size_t skin_index, std::size_t& vertices,
                       bool allow_zero_totals = false) {
    const auto& skin=scene.skins[skin_index];
    const bool has_gltf=skin.inverse_bind_matrices_gltf.size()==skin.joints.size();
    const bool has_stingray=skin.inverse_bind_matrices_stingray.size()==skin.joints.size();
    if (skin.joints.empty() || (!has_gltf && !has_stingray)) return false;
    std::set<int> distinct; for (int joint:skin.joints) if (joint<0 || static_cast<std::size_t>(joint)>=scene.nodes.size() || !distinct.insert(joint).second) return false;
    const auto& channels=primitive.source_channels; const auto* pos=find_channel(channels,VertexChannel::Semantic::Position); if (!pos || pos->components<3 || pos->components>4 || pos->values.size()%pos->components) return false; vertices=pos->values.size()/pos->components;
    std::set<std::uint32_t> sets; for (const auto& c:channels) if (c.semantic==VertexChannel::Semantic::BlendIndices || c.semantic==VertexChannel::Semantic::BlendWeights) sets.insert(c.set);
    if (sets.empty()) return false;
    for (std::uint32_t set:sets) { const auto* joints=find_channel(channels,VertexChannel::Semantic::BlendIndices,set); const auto* weights=find_channel(channels,VertexChannel::Semantic::BlendWeights,set); if (!joints||!weights||joints->components!=4||weights->components!=4||joints->values.size()!=vertices*4||weights->values.size()!=vertices*4) return false; for (std::size_t row=0;row<vertices;++row) for (int c=0;c<4;++c) { const float j=joints->values[row*4+c],w=weights->values[row*4+c]; if (!std::isfinite(j)||!std::isfinite(w)||j<0||j>=static_cast<float>(skin.joints.size())||std::floor(j)!=j||w<0) return false; } }
    for (std::size_t row=0;row<vertices;++row) { float total=0; for (std::uint32_t set:sets) { const auto* weights=find_channel(channels,VertexChannel::Semantic::BlendWeights,set); for (int c=0;c<4;++c) total+=weights->values[row*4+c]; } if (!std::isfinite(total)||(total<=0 && !allow_zero_totals)) return false; }
    for (std::size_t i=0;i<skin.joints.size();++i) { if (has_gltf&&!finite_matrix(skin.inverse_bind_matrices_gltf[i])) return false; if (has_stingray&&!finite_matrix(skin.inverse_bind_matrices_stingray[i])) return false; }
    return true;
}

bool lower_zero_weight_vertices(Scene& scene, Primitive& primitive, std::size_t skin_index,
                                std::size_t primitive_index, NativeProfileCanonicalizationReport& report,
                                std::string& error) {
    std::size_t vertices = 0;
    if (!valid_bake_source(scene, primitive, skin_index, vertices, true)) {
        error = "native profile cannot lower malformed or inconsistent skin source data";
        return false;
    }
    std::set<std::uint32_t> sets;
    for (const auto& channel : primitive.source_channels)
        if (channel.semantic == VertexChannel::Semantic::BlendIndices || channel.semantic == VertexChannel::Semantic::BlendWeights)
            sets.insert(channel.set);
    bool changed = false;
    for (std::size_t row = 0; row < vertices; ++row) {
        float total = 0.0f;
        bool current_first = true;
        for (const auto set : sets) {
            const auto* weights = find_channel(primitive.source_channels, VertexChannel::Semantic::BlendWeights, set);
            for (int c = 0; c < 4; ++c) total += weights->values[row * 4 + c];
        }
        if (total > 0.0f) continue;
        changed = true;
        bool first = true;
        for (const auto set : sets) {
            auto* joints = find_channel(primitive.source_channels, VertexChannel::Semantic::BlendIndices, set);
            auto* weights = find_channel(primitive.source_channels, VertexChannel::Semantic::BlendWeights, set);
            for (int c = 0; c < 4; ++c) {
                joints->values[row * 4 + c] = 0.0f;
                weights->values[row * 4 + c] = (first && c == 0) ? 1.0f : 0.0f;
            }
            first = false;
        }
        for (const auto set : sets) {
            auto* joints = find_channel(primitive.channels, VertexChannel::Semantic::BlendIndices, set);
            auto* weights = find_channel(primitive.channels, VertexChannel::Semantic::BlendWeights, set);
            if (!joints || !weights || joints->components != 4 || weights->components != 4 ||
                joints->values.size() != vertices * 4 || weights->values.size() != vertices * 4) continue;
            for (int c = 0; c < 4; ++c) {
                joints->values[row * 4 + c] = 0.0f;
                weights->values[row * 4 + c] = (current_first && c == 0) ? 1.0f : 0.0f;
            }
            current_first = false;
        }
    }
    if (changed) note(scene, report, primitive_index, "skinning_zero_weight_default_joint",
                      "primitive " + std::to_string(primitive_index) + ": assigned zero-weight vertices to skin joint 0");
    return true;
}

bool fits_native_profile(const Scene& scene, const Primitive& primitive, std::size_t skin_index, std::size_t& vertices) {
    const auto& skin=scene.skins[skin_index];
    if (skin.joints.size()>256 || (!skin.inverse_bind_matrices_gltf.empty() && skin.inverse_bind_matrices_gltf.size()!=skin.joints.size()) || (!skin.inverse_bind_matrices_stingray.empty() && skin.inverse_bind_matrices_stingray.size()!=skin.joints.size()) || !valid_bake_source(scene,primitive,skin_index,vertices)) return false;
    if (!find_channel(primitive.source_channels,VertexChannel::Semantic::BlendIndices,0) ||
        !find_channel(primitive.source_channels,VertexChannel::Semantic::BlendWeights,0)) return false;
    for (const auto& c:primitive.source_channels) {
        if ((c.semantic==VertexChannel::Semantic::Texcoord&&c.set>1) ||
            ((c.semantic==VertexChannel::Semantic::BlendIndices||c.semantic==VertexChannel::Semantic::BlendWeights)&&c.set>1)) return false;
        if (c.semantic==VertexChannel::Semantic::BlendWeights &&
            std::any_of(c.values.begin(),c.values.end(),[](float value){return value>1.0f;})) return false;
    }
    return true;
}

bool bake_static(Scene& scene, Primitive& primitive, std::size_t skin_index, std::size_t primitive_index, NativeProfileCanonicalizationReport& report, std::string& error) {
    std::size_t vertices=0; if (!valid_bake_source(scene,primitive,skin_index,vertices)) { error="native profile cannot bake malformed or inconsistent skin source data"; return false; }
    const auto& skin=scene.skins[skin_index]; const bool use_stingray=skin.inverse_bind_matrices_stingray.size()==skin.joints.size(); const auto& ibm=use_stingray?skin.inverse_bind_matrices_stingray:skin.inverse_bind_matrices_gltf; std::vector<Matrix4> transforms; transforms.reserve(skin.joints.size());
    for (std::size_t i=0;i<skin.joints.size();++i) { const auto& world=scene.nodes[static_cast<std::size_t>(skin.joints[i])].world_stingray; if (!finite_matrix(world)) { error="native profile skin joint has a non-finite world transform"; return false; } transforms.push_back(multiply(world,ibm[i])); }
    std::vector<VertexChannel> baked=primitive.source_channels;
    bool reported_tangent_parity=false;
    bool reported_tangent_fallback=false;
    bool reported_normal_fallback=false;
    std::vector<Matrix4> vertex_transforms(vertices);
    std::vector<float> vertex_totals(vertices, 0.0f);
    for (std::size_t row=0; row<vertices; ++row) {
        std::set<std::uint32_t> sets; for (const auto& source:primitive.source_channels) if (source.semantic==VertexChannel::Semantic::BlendIndices || source.semantic==VertexChannel::Semantic::BlendWeights) sets.insert(source.set);
        for (std::uint32_t set:sets) { const auto* joints=find_channel(primitive.source_channels,VertexChannel::Semantic::BlendIndices,set); const auto* weights=find_channel(primitive.source_channels,VertexChannel::Semantic::BlendWeights,set); if (!joints||!weights) continue; for (int c=0;c<4;++c) { const float w=weights->values[row*4+c]; const std::size_t j=static_cast<std::size_t>(joints->values[row*4+c]); for (std::size_t a=0;a<16;++a) vertex_transforms[row][a]+=w*transforms[j][a]; vertex_totals[row]+=w; } }
        if (!std::isfinite(vertex_totals[row]) || vertex_totals[row] <= 0.0f) { error="native profile skin weights have no influence"; return false; }
        for (float& value:vertex_transforms[row]) value/=vertex_totals[row];
    }
    for (auto& channel:baked) {
        if (channel.semantic!=VertexChannel::Semantic::Position && channel.semantic!=VertexChannel::Semantic::Normal && channel.semantic!=VertexChannel::Semantic::Tangent) continue;
        if (channel.components<3 || channel.values.size()!=vertices*channel.components) { error="native profile source channel has malformed rows"; return false; }
        for (std::size_t row=0;row<vertices;++row) { Vec3 value{channel.values[row*channel.components],channel.values[row*channel.components+1],channel.values[row*channel.components+2]}; if (!std::all_of(value.begin(),value.end(),[](float x){return std::isfinite(x);})) { error="native profile source channel contains non-finite data"; return false; } Vec3 transformed{}; if (channel.semantic==VertexChannel::Semantic::Position) transformed=point(vertex_transforms[row],value); else if (channel.semantic==VertexChannel::Semantic::Normal) { if (!inverse_transpose_linear(vertex_transforms[row],value,transformed)) { transformed=linear(vertex_transforms[row],value); if (std::sqrt(transformed[0]*transformed[0]+transformed[1]*transformed[1]+transformed[2]*transformed[2])<=1e-12f) transformed=normalized(value); reported_normal_fallback=true; } } else { transformed=linear(vertex_transforms[row],value); if (channel.semantic==VertexChannel::Semantic::Tangent && std::sqrt(transformed[0]*transformed[0]+transformed[1]*transformed[1]+transformed[2]*transformed[2])<=1e-12f) { transformed=normalized(value); reported_tangent_fallback=true; } } Vec3 output=channel.semantic==VertexChannel::Semantic::Position?transformed:normalized(transformed); if (!std::all_of(output.begin(),output.end(),[](float x){return std::isfinite(x);})) { error="native profile baked geometry is non-finite"; return false; } for (int a=0;a<3;++a) channel.values[row*channel.components+a]=output[a]; if (channel.semantic==VertexChannel::Semantic::Tangent && channel.components>=4) { const float determinant=determinant3(vertex_transforms[row]); const float parity=std::isfinite(determinant)&&determinant<0.0f?-1.0f:1.0f; channel.values[row*channel.components+3]*=parity; if (parity<0.0f) reported_tangent_parity=true; }
        }
    }
    if (reported_normal_fallback) note(scene,report,primitive_index,"skinning_normal_inverse_transpose_fallback","primitive "+std::to_string(primitive_index)+": singular skin normal transform used a deterministic linear/authored-normal fallback");
    if (reported_tangent_fallback) note(scene,report,primitive_index,"skinning_tangent_linear_fallback","primitive "+std::to_string(primitive_index)+": singular blended skin tangent used the normalized authored tangent");
    if (reported_tangent_parity) note(scene,report,primitive_index,"skinning_tangent_parity","primitive "+std::to_string(primitive_index)+": reflected skin tangents use blended-transform parity");
    auto current=primitive.channels.empty()?baked:primitive.channels; for (const auto& source:baked) { if (source.semantic==VertexChannel::Semantic::BlendIndices||source.semantic==VertexChannel::Semantic::BlendWeights) continue; auto* destination=find_channel(current,source.semantic,source.set); if (destination) *destination=source; else current.push_back(source); }
    primitive.source_channels=std::move(baked); primitive.channels=std::move(current); primitive.source_node=-1;
    auto remove=[](std::vector<VertexChannel>& channels){ channels.erase(std::remove_if(channels.begin(),channels.end(),[](const VertexChannel& c){return c.semantic==VertexChannel::Semantic::BlendIndices||c.semantic==VertexChannel::Semantic::BlendWeights;}),channels.end()); }; remove(primitive.source_channels); remove(primitive.channels);
    note(scene,report,primitive_index,"skinning_static_bake","primitive "+std::to_string(primitive_index)+": detached skinned geometry to world-baked static channels"); return true;
}

void update_bounds(Primitive& primitive) {
    auto update=[](const std::vector<VertexChannel>& channels,std::array<float,3>& minimum,std::array<float,3>& maximum,float& radius){ const auto* p=find_channel(channels,VertexChannel::Semantic::Position); if(!p||p->components<3||p->values.empty()) return; minimum={std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()}; maximum={-minimum[0],-minimum[1],-minimum[2]}; float r2=0; for(std::size_t row=0;row<p->values.size();row+=p->components){for(int a=0;a<3;++a){minimum[a]=std::min(minimum[a],p->values[row+a]);maximum[a]=std::max(maximum[a],p->values[row+a]);} r2=std::max(r2,p->values[row]*p->values[row]+p->values[row+1]*p->values[row+1]+p->values[row+2]*p->values[row+2]);} radius=std::sqrt(r2);}; update(primitive.source_channels,primitive.source_bounds_min,primitive.source_bounds_max,primitive.source_bounds_radius); update(primitive.channels,primitive.bounds_min,primitive.bounds_max,primitive.bounds_radius);
}

bool bake_rigid_static(Primitive& primitive, const Matrix4& transform, std::size_t primitive_index,
                       Scene& scene, NativeProfileCanonicalizationReport& report, std::string& error) {
    if (!finite_matrix(transform)) {
        error = "native profile cannot bake a primitive with an unsupported world transform";
        return false;
    }
    // Loader `channels` are already world-baked for rigid/static geometry.
    // Detaching them is sufficient; applying the node world matrix again
    // would double-transform the emitted mesh.
    if (primitive.channels.empty()) primitive.channels = primitive.source_channels;
    for (auto& channel : primitive.channels) {
        const bool vector = channel.semantic == VertexChannel::Semantic::Normal ||
            channel.semantic == VertexChannel::Semantic::Tangent || channel.semantic == VertexChannel::Semantic::Binormal;
        const bool position = channel.semantic == VertexChannel::Semantic::Position;
        if (!position && !vector) continue;
        if (channel.components < 3 || channel.values.size() % channel.components != 0) {
            error = "native profile static channel has malformed rows";
            return false;
        }
        for (std::size_t row = 0; row < channel.values.size(); row += channel.components) {
            Vec3 value{channel.values[row], channel.values[row + 1], channel.values[row + 2]};
            if (!std::all_of(value.begin(), value.end(), [](float x) { return std::isfinite(x); })) {
                error = "native profile static channel contains non-finite data";
                return false;
            }
        }
    }
    primitive.source_node = -1;
    note(scene, report, primitive_index, "unit_scene_graph_baked",
         "primitive " + std::to_string(primitive_index) + ": baked static world/rest-pose geometry before scene-graph collapse");
    return true;
}

bool lower_skinned_without_influences(Scene& scene, Primitive& primitive, std::size_t primitive_index,
                                      const Matrix4& transform, NativeProfileCanonicalizationReport& report,
                                      std::string& error) {
    if (!bake_rigid_static(primitive, transform, primitive_index, scene, report, error)) return false;
    auto remove_influences = [](std::vector<VertexChannel>& channels) {
        channels.erase(std::remove_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
            return channel.semantic == VertexChannel::Semantic::BlendIndices ||
                   channel.semantic == VertexChannel::Semantic::BlendWeights;
        }), channels.end());
    };
    remove_influences(primitive.source_channels);
    remove_influences(primitive.channels);
    note(scene, report, primitive_index, "skinning_missing_influences_rigid",
         "primitive " + std::to_string(primitive_index) + ": lowered skinned geometry without an influence pair as rigid world-baked geometry");
    return true;
}
} // namespace

bool canonicalize_native_profile(Scene& scene, NativeProfileCanonicalizationReport& report, std::string& error) {
    report={}; error.clear(); std::vector<std::size_t> primitive_skins(scene.primitives.size(),static_cast<std::size_t>(-1)); std::set<std::size_t> used_skins;
    // UNIT can omit authored references only for a genuinely ref-free source;
    // otherwise reserve one additional slot for the authored/root reference.
    const bool has_skinned_source = std::any_of(scene.primitives.begin(), scene.primitives.end(), [&](const Primitive& primitive) {
        return primitive.source_node >= 0 && static_cast<std::size_t>(primitive.source_node) < scene.nodes.size() &&
            scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin >= 0;
    });
    const bool has_node_physics = !scene.asset_definition.node_bodies.empty() || !scene.asset_definition.joints.empty();
    const bool can_omit_authored_refs = scene.source_features.node_count <= 1 &&
        scene.source_features.parent_edge_count == 0 && !has_skinned_source && !has_node_physics;
    std::set<int> physics_ancestry;
    auto protect = [&](int node) {
        std::set<int> visited;
        while (node >= 0) {
            if (static_cast<std::size_t>(node) >= scene.nodes.size() || !visited.insert(node).second) return false;
            physics_ancestry.insert(node);
            node = scene.nodes[node].parent;
        }
        return true;
    };
    for (const auto& body : scene.asset_definition.node_bodies)
        if (!protect(body.source_node)) { error = "invalid body node ancestry"; return false; }
    for (const auto& joint : scene.asset_definition.joints)
        if (!protect(joint.source_node)) { error = "invalid joint node ancestry"; return false; }
    const std::size_t kMaxEmittedPrimitives = can_omit_authored_refs ? 65534u : 65533u;
    if (scene.primitives.size() > kMaxEmittedPrimitives) {
        scene.primitives.resize(kMaxEmittedPrimitives);
        note(scene, report, kMaxEmittedPrimitives, "unit_primitive_cardinality_drop",
             "native profile retained the authored-order primitive prefix below the 16-bit UNIT cardinality limit");
        primitive_skins.resize(kMaxEmittedPrimitives, static_cast<std::size_t>(-1));
    }
    for(std::size_t i=0;i<scene.primitives.size();++i){auto& p=scene.primitives[i]; if(p.source_node<0||static_cast<std::size_t>(p.source_node)>=scene.nodes.size()){if(!clamp_packed_channels(p.channels,i,scene,report,error))return false;update_bounds(p);continue;} const int skin=scene.nodes[static_cast<std::size_t>(p.source_node)].skin; if(skin<0||static_cast<std::size_t>(skin)>=scene.skins.size()){if(!clamp_packed_channels(p.channels,i,scene,report,error))return false;update_bounds(p);continue;} primitive_skins[i]=static_cast<std::size_t>(skin);used_skins.insert(static_cast<std::size_t>(skin));}
    auto nodes=active_nodes(scene); for(const auto skin:used_skins)add_skin_ancestry(scene,skin,nodes);
    std::set<int> baked_nodes;
    for (const int node_index : nodes) {
        auto& node = scene.nodes[static_cast<std::size_t>(node_index)];
        const bool local_nonfinite = !finite_matrix(node.local_stingray);
        const bool world_nonfinite = !finite_matrix(node.world_stingray);
        const bool local_projective = !local_nonfinite && !zero_matrix(node.local_stingray) &&
            (std::abs(node.local_stingray[3]) > 1e-6f || std::abs(node.local_stingray[7]) > 1e-6f ||
             std::abs(node.local_stingray[11]) > 1e-6f || std::abs(node.local_stingray[15] - 1.0f) > 1e-6f);
        const bool world_projective = !world_nonfinite && !zero_matrix(node.world_stingray) &&
            (std::abs(node.world_stingray[3]) > 1e-6f || std::abs(node.world_stingray[7]) > 1e-6f ||
             std::abs(node.world_stingray[11]) > 1e-6f || std::abs(node.world_stingray[15] - 1.0f) > 1e-6f);
        if (local_nonfinite || world_nonfinite) {
            error = "native profile active UNIT node has a non-finite transform";
            return false;
        }
        if (local_projective || world_projective) {
            if (physics_ancestry.count(node_index)) {
                error = "body/joint ancestry requires affine UNIT transforms: " + node.name;
                return false;
            }
            // Projective terms cannot be represented by UNIT.  The mesh channels
            // are already world-baked, so retain only the affine top 3x4 while
            // forcing this node down the detach/identity path below.
            if (local_projective) {
                node.local_stingray[3] = 0.0f;
                node.local_stingray[7] = 0.0f;
                node.local_stingray[11] = 0.0f;
                node.local_stingray[15] = 1.0f;
            }
            if (world_projective) {
                node.world_stingray[3] = 0.0f;
                node.world_stingray[7] = 0.0f;
                node.world_stingray[11] = 0.0f;
                node.world_stingray[15] = 1.0f;
            }
            baked_nodes.insert(node_index);
            note(scene, report, static_cast<std::size_t>(node_index), "projective_transform_approximated",
                 "node " + std::to_string(node_index) + ": approximated finite projective transform by its affine top 3x4 and detached world-baked geometry");
        }
        if (!unit_transform_supported(node.local_stingray)) {
            if (physics_ancestry.count(node_index)) {
                error = "body/joint ancestry cannot be represented by UNIT TRS: " + node.name;
                return false;
            }
            baked_nodes.insert(node_index);
        }
    }
    const std::size_t emitted_authored_nodes = can_omit_authored_refs ? 0u : nodes.size();
    if (emitted_authored_nodes + scene.primitives.size() + 1u > 65535u) {
        if (has_node_physics) { error = "node-bound physics exceeds UNIT node capacity"; return false; }
        for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
            auto& primitive = scene.primitives[i];
            const int source_node = primitive.source_node;
            if (source_node >= 0 && static_cast<std::size_t>(source_node) < scene.nodes.size()) {
                const int skin = scene.nodes[static_cast<std::size_t>(source_node)].skin;
                if (skin >= 0 && static_cast<std::size_t>(skin) < scene.skins.size()) {
                    const bool has_influence_channel = std::any_of(primitive.source_channels.begin(), primitive.source_channels.end(), [](const VertexChannel& channel) {
                        return channel.semantic == VertexChannel::Semantic::BlendIndices || channel.semantic == VertexChannel::Semantic::BlendWeights;
                    });
                    if (!has_influence_channel) {
                        if (!lower_skinned_without_influences(scene, primitive, i,
                                                              scene.nodes[static_cast<std::size_t>(source_node)].world_stingray,
                                                              report, error)) return false;
                    } else {
                        if (!lower_zero_weight_vertices(scene, primitive, static_cast<std::size_t>(skin), i, report, error)) return false;
                        if (!bake_static(scene, primitive, static_cast<std::size_t>(skin), i, report, error)) return false;
                    }
                } else if (!bake_rigid_static(primitive, scene.nodes[static_cast<std::size_t>(source_node)].world_stingray,
                                               i, scene, report, error)) return false;
            } else if (!clamp_packed_channels(primitive.channels, i, scene, report, error)) return false;
            if (!clamp_packed_channels(primitive.channels, i, scene, report, error)) return false;
            update_bounds(primitive);
        }
        const bool retained_static_animations = !scene.animations.empty();
        for (auto& animation : scene.animations) {
            AnimationTrack track;
            track.target_node = 0;
            track.path = AnimationPath::Translation;
            track.interpolation = AnimationInterpolation::Linear;
            track.value_components = 3;
            track.times = {0.0f};
            track.values = {0.0f, 0.0f, 0.0f};
            animation.tracks = {std::move(track)};
        }
        scene.skins.clear();
        scene.nodes.resize(1);
        identity_node(scene.nodes[0]);
        scene.nodes[0].name = "__collapsed_scene_root__";
        scene.nodes[0].parent = -1;
        scene.nodes[0].children.clear();
        scene.nodes[0].skin = retained_static_animations ? 0 : -1;
        scene.scene_roots = {0};
        if (retained_static_animations) {
            SkinInfo skin;
            skin.name = "__collapsed_scene_skin__";
            skin.skeleton_root = 0;
            skin.joints = {0};
            skin.joint_names = {scene.nodes[0].name};
            skin.inverse_bind_matrices_stingray = {Matrix4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
            scene.skins.push_back(std::move(skin));
        }
        note(scene, report, 0, "unit_scene_graph_collapse",
             "native profile collapsed the oversized UNIT node table to an identity root; geometry retained its world/rest pose and animations retained static clips");
        return true;
    }
    std::set<std::size_t> transform_baked_skins;
    for (const auto skin : used_skins) {
        std::set<int> ancestry;
        add_skin_ancestry(scene, skin, ancestry);
        if (std::any_of(ancestry.begin(), ancestry.end(), [&](int node_index) {
                return baked_nodes.count(node_index) != 0;
            })) transform_baked_skins.insert(skin);
    }
    const bool multiple_skins=used_skins.size()>1;
    for(std::size_t i=0;i<scene.primitives.size();++i){const auto skin=primitive_skins[i]; const int source_node=scene.primitives[i].source_node; const bool transform_baked=source_node>=0&&baked_nodes.count(source_node)!=0;
        if(skin==static_cast<std::size_t>(-1)){if(transform_baked){scene.primitives[i].source_node=-1;note(scene,report,i,"unit_transform_baked","primitive "+std::to_string(i)+": detached geometry from unsupported UNIT transform");}if(!clamp_packed_channels(scene.primitives[i].channels,i,scene,report,error))return false;update_bounds(scene.primitives[i]);continue;}
        const bool skin_transform_baked=transform_baked_skins.count(skin)!=0;
        const bool has_influence_channel = std::any_of(scene.primitives[i].source_channels.begin(), scene.primitives[i].source_channels.end(), [](const VertexChannel& channel) {
            return channel.semantic == VertexChannel::Semantic::BlendIndices || channel.semantic == VertexChannel::Semantic::BlendWeights;
        });
        if (!has_influence_channel) {
            if (!lower_skinned_without_influences(scene, scene.primitives[i], i,
                                                  scene.nodes[static_cast<std::size_t>(source_node)].world_stingray,
                                                  report, error)) return false;
            if (!clamp_packed_channels(scene.primitives[i].channels, i, scene, report, error)) return false;
            update_bounds(scene.primitives[i]);
            continue;
        }
        if (!lower_zero_weight_vertices(scene, scene.primitives[i], skin, i, report, error)) return false;
        std::size_t vertices=0;const bool fit=fits_native_profile(scene,scene.primitives[i],skin,vertices);if(transform_baked||skin_transform_baked||multiple_skins||!fit){if(!bake_static(scene,scene.primitives[i],skin,i,report,error))return false;if(transform_baked||skin_transform_baked)note(scene,report,i,"unit_transform_baked","primitive "+std::to_string(i)+": baked geometry before detaching unsupported UNIT transform");if(!clamp_packed_channels(scene.primitives[i].channels,i,scene,report,error))return false;}else if(!clamp_packed_channels(scene.primitives[i].source_channels,i,scene,report,error))return false;update_bounds(scene.primitives[i]);}
    for (const int node_index : baked_nodes) { identity_node(scene.nodes[static_cast<std::size_t>(node_index)]); note(scene,report,static_cast<std::size_t>(node_index),"unit_transform_baked","node "+std::to_string(node_index)+": replaced unsupported UNIT transform with identity"); }
    for(const int node_index:nodes){auto& name=scene.nodes[static_cast<std::size_t>(node_index)].name;if(name.find('\0')==std::string::npos)continue;std::replace(name.begin(),name.end(),'\0','_');note(scene,report,static_cast<std::size_t>(node_index),"node_name_nul_replaced","node "+std::to_string(node_index)+": replaced embedded NUL in active node name");}return true;
}
} // namespace dtglb::processing
