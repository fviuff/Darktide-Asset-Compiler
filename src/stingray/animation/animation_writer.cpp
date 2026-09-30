#include "stingray/animation/animation_writer.h"
#include "stingray/animation/animation_evaluator.h"
#include "scene/scene.h"
#include "stingray/cooked_resource.h"
#include "gltf/animation_sampler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_map>

namespace dtglb::stingray::animation {
namespace {
template<class T> void append(std::vector<std::uint8_t>& out,const T&v){const std::size_t old=out.size();out.resize(old+sizeof(T));std::memcpy(out.data()+old,&v,sizeof(T));}
template<class T> T read(const std::vector<std::uint8_t>&b,std::size_t o){T v{};std::memcpy(&v,b.data()+o,sizeof(T));return v;}

std::array<float,4> quat_from_matrix(const Matrix4&m,float sx,float sy,float sz){
    // Matrix4 is column-major. Remove scale first.
    const float r00=sx?m[0]/sx:1, r01=sy?m[4]/sy:0, r02=sz?m[8]/sz:0;
    const float r10=sx?m[1]/sx:0, r11=sy?m[5]/sy:1, r12=sz?m[9]/sz:0;
    const float r20=sx?m[2]/sx:0, r21=sy?m[6]/sy:0, r22=sz?m[10]/sz:1;
    float x=0,y=0,z=0,w=1; const float tr=r00+r11+r22;
    if(tr>0){float s=std::sqrt(tr+1.0f)*2.0f;w=0.25f*s;x=(r21-r12)/s;y=(r02-r20)/s;z=(r10-r01)/s;}
    else if(r00>r11&&r00>r22){float s=std::sqrt(1.0f+r00-r11-r22)*2.0f;w=(r21-r12)/s;x=0.25f*s;y=(r01+r10)/s;z=(r02+r20)/s;}
    else if(r11>r22){float s=std::sqrt(1.0f+r11-r00-r22)*2.0f;w=(r02-r20)/s;x=(r01+r10)/s;y=0.25f*s;z=(r12+r21)/s;}
    else{float s=std::sqrt(1.0f+r22-r00-r11)*2.0f;w=(r10-r01)/s;x=(r02+r20)/s;y=(r12+r21)/s;z=0.25f*s;}
    const float l=std::sqrt(x*x+y*y+z*z+w*w); if(l>1e-20f){x/=l;y/=l;z/=l;w/=l;} return{x,y,z,w};
}

struct State{std::array<float,3>p;std::array<float,4>q;std::array<float,3>s;};
bool normalize_quaternion(std::array<float,4>& q){
    float length2=0.0f;for(float v:q){if(!std::isfinite(v))return false;length2+=v*v;}
    if(!std::isfinite(length2)||length2<1e-20f)return false;
    const float inverse=1.0f/std::sqrt(length2);for(float&v:q)v*=inverse;return true;
}
bool finite_state(const State&s){for(float v:s.p)if(!std::isfinite(v))return false;for(float v:s.q)if(!std::isfinite(v))return false;for(float v:s.s)if(!std::isfinite(v))return false;return true;}
bool base_state(const NodeInfo&n,State&out){
    if(n.has_exact_trs){out={n.local_translation_stingray,n.local_rotation_stingray,n.local_scale_stingray};return finite_state(out)&&normalize_quaternion(out.q);}
    const auto&m=n.local_stingray; const float sx=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);const float sy=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]);const float sz=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
    if(!std::isfinite(sx)||!std::isfinite(sy)||!std::isfinite(sz)||sx<1e-20f||sy<1e-20f||sz<1e-20f)return false;
    const float d01=(m[0]*m[4]+m[1]*m[5]+m[2]*m[6])/(sx*sy),d02=(m[0]*m[8]+m[1]*m[9]+m[2]*m[10])/(sx*sz),d12=(m[4]*m[8]+m[5]*m[9]+m[6]*m[10])/(sy*sz);
    if(std::abs(d01)>1e-3f||std::abs(d02)>1e-3f||std::abs(d12)>1e-3f)return false;
    out={{m[12],m[13],m[14]},quat_from_matrix(m,sx,sy,sz),{sx,sy,sz}};return finite_state(out)&&normalize_quaternion(out.q);
}

struct Entry{float time;float gate;std::uint16_t bone;AnimationPath path;std::size_t key;std::vector<float>values;};
void set_channel(State& state, AnimationPath path, const std::array<float,4>& value) {
    if(path==AnimationPath::Translation) state.p={value[0],value[1],value[2]};
    else if(path==AnimationPath::Rotation) state.q=value;
    else state.s={value[0],value[1],value[2]};
}

// Equal timestamps are reserved for complete STEP boundary groups. The old pair
// supplies look-ahead before T; the new pair becomes eligible exactly at T.
bool validate_boundary_groups(const std::vector<Entry>& keys, std::vector<float> initial, std::string& error) {
    bool repeated=false;
    for(std::size_t i=1;i<keys.size();++i) repeated |= keys[i].time==keys[i-1].time;
    if(!repeated) return true;
    if(keys.size()>max_step_controls_per_track) {error="animation STEP boundary expansion exceeds control limit per track";return false;}
    if(keys.size()%4) {error="animation STEP boundary must contain four controls";return false;}
    float previous_time=0;
    for(std::size_t i=0;i<keys.size();i+=4) {
        const auto& a=keys[i];const auto& b=keys[i+1];const auto& c=keys[i+2];const auto& d=keys[i+3];
        if(a.time<=previous_time || b.time!=a.time || c.time!=a.time || d.time!=a.time ||
           a.values!=b.values || c.values!=d.values || a.values!=initial) {
            error="animation equal-time controls violate STEP old/old/new/new boundary grammar";return false;
        }
        previous_time=a.time;initial=d.values;
    }
    return true;
}
int order(AnimationPath p){return p==AnimationPath::Scale?0:(p==AnimationPath::Rotation?1:2);}
std::uint16_t subtype(AnimationPath p){return p==AnimationPath::Translation?4:(p==AnimationPath::Rotation?5:6);}
std::uint32_t width(AnimationPath p){return p==AnimationPath::Rotation?4:3;}

std::vector<float> bounded_sample_times(const std::vector<float>& candidates, std::size_t limit) {
    if (candidates.size() <= limit) return candidates;
    if (limit == 0) return {};
    if (limit == 1) return {candidates.back()};
    std::vector<float> result;
    result.reserve(limit);
    // Keep both endpoints and choose the remaining entries by stable index.
    result.push_back(candidates.front());
    for (std::size_t i = 1; i + 1 < limit; ++i) {
        const auto index = static_cast<std::size_t>(
            (static_cast<long double>(i) * static_cast<long double>(candidates.size() - 1)) /
            static_cast<long double>(limit - 1));
        result.push_back(candidates[index]);
    }
    result.push_back(candidates.back());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::uint8_t> build_core(const std::vector<State>&states,const std::vector<Entry>&entries,float duration,std::uint32_t file_size){
    std::vector<std::uint8_t> out; append(out,std::uint32_t{0});append(out,std::uint32_t{0});append(out,static_cast<std::uint32_t>(states.size()));append(out,duration);append(out,file_size);append(out,std::uint32_t{0});append(out,std::uint16_t{7});
    for(const auto&s:states){for(float v:s.p)append(out,v);for(float v:s.q)append(out,v);for(float v:s.s)append(out,v);}
    for(const auto&e:entries){append(out,subtype(e.path));append(out,e.bone);append(out,e.time);for(float v:e.values)append(out,v);}append(out,std::uint16_t{3});return out;
}
}

bool build_skeletal_animation(const Scene&scene,const SkinInfo&skin,const AnimationInfo&source,std::string resource_name,BuiltAnimation&out,std::string&error,const FitOptions* fit_options){
    if(resource_name.empty()){error="animation resource name is empty";return false;} if(skin.joints.empty()||skin.joints.size()>65535){error="animation skin has invalid joint count";return false;}
    std::unordered_map<int,std::uint16_t> bone_by_node;std::vector<State>states;states.reserve(skin.joints.size());
    for(std::size_t i=0;i<skin.joints.size();++i){int node=skin.joints[i];if(node<0||static_cast<std::size_t>(node)>=scene.nodes.size()){error="animation skin joint node is out of range";return false;}bone_by_node[node]=static_cast<std::uint16_t>(i);State state;if(!base_state(scene.nodes[node],state)){error="animation joint base transform is not finite, positive-scale TRS";return false;}states.push_back(state);}
    std::vector<Entry>entries;float duration=0; for(const auto&t:source.tracks) for(float time:t.times) if(std::isfinite(time)&&time>=0) duration=std::max(duration,time);
    std::set<std::pair<std::uint16_t,AnimationPath>> emitted_channels;
    std::vector<BuiltAnimation::FitReport> fits;
    bool build_best_effort = false;
    for(const auto&t:source.tracks){auto it=bone_by_node.find(t.target_node);if(it==bone_by_node.end()||t.path==AnimationPath::Weights)continue;const auto w=width(t.path);const auto mult=t.interpolation==AnimationInterpolation::CubicSpline?3u:1u;if(t.value_components<w||t.times.empty()||t.values.size()<t.times.size()*t.value_components*mult){error="animation track has inconsistent input/output data";return false;}for(std::size_t i=0;i<t.times.size();++i){if(!std::isfinite(t.times[i])||t.times[i]<0||(i&&t.times[i]<=t.times[i-1])){error="animation track times must be finite, non-negative, and strictly increasing";return false;}duration=std::max(duration,t.times[i]);}
        if(!emitted_channels.insert({it->second,t.path}).second){error="animation contains duplicate channels for one bone/path";return false;}
        if(fit_options && t.interpolation!=AnimationInterpolation::Step) {
            TrackFit fitted;
            if(!fit_animation_track(t,*fit_options,fitted,error)) {
                error="animation node "+std::to_string(t.target_node)+" fit failed: "+error;return false;
            }
            set_channel(states[it->second],t.path,fitted.initial);
            for(std::size_t i=0;i<fitted.times.size();++i)
                entries.push_back({fitted.times[i],0,it->second,t.path,i,
                    std::vector<float>(fitted.values[i].begin(),fitted.values[i].begin()+w)});
            const double tolerance=t.path==AnimationPath::Translation?fit_options->translation_tolerance:
                t.path==AnimationPath::Scale?fit_options->scale_tolerance:fit_options->rotation_tolerance_radians;
            fits.push_back({t.target_node,t.path,tolerance,fitted.measured_max_error,fitted.measured_max_error_time,
                fitted.measured_error_available,
                fitted.conformant,fitted.best_effort,fitted.times.size(),fitted.refinements,fitted.evaluations,
                fitted.planned_probe_count,fitted.evaluated_probe_count});
            continue;
        }
        if(t.interpolation==AnimationInterpolation::Step) {
            const std::size_t max_transitions = max_step_controls_per_track / 4;
            std::vector<std::size_t> transition_indices;
            const std::size_t transition_count = t.times.size() - 1;
            transition_indices.reserve(std::min(transition_count, max_transitions));
            const bool decimated = transition_count > max_transitions;
            if (decimated) {
                for (std::size_t i = 0; i < max_transitions; ++i) {
                    transition_indices.push_back(1 + static_cast<std::size_t>(
                        (static_cast<long double>(i) * static_cast<long double>(transition_count - 1)) /
                        static_cast<long double>(max_transitions - 1)));
                }
                build_best_effort = true;
            } else for (std::size_t i = 1; i < t.times.size(); ++i) transition_indices.push_back(i);
            auto source_value=[&](float time,std::array<float,4>& value) {
                if(!gltf::sample_animation_track(t,time,value,error))return false;
                if(t.path==AnimationPath::Rotation&&!normalize_quaternion(value)) {error="animation STEP quaternion is invalid";return false;}
                return true;
            };
            std::array<float,4> previous{};
            if(!source_value(0,previous))return false;
            set_channel(states[it->second],t.path,previous);
            std::size_t ordinal=0;
            auto emit=[&](float time,const std::array<float,4>& value) {
                entries.push_back({time,0,it->second,t.path,ordinal++,std::vector<float>(value.begin(),value.begin()+w)});
            };
            if(t.times.size()==1) emit(t.times.front(),previous);
            for(const std::size_t i : transition_indices) {
                if (decimated && i > 1 && !source_value(t.times[i - 1], previous)) return false;
                std::array<float,4> next{};if(!source_value(t.times[i],next))return false;
                if(t.path==AnimationPath::Rotation) {
                    double dot=0;for(std::size_t c=0;c<4;++c)dot+=double(previous[c])*next[c];
                    if(dot<0)for(float& v:next)v=-v;
                }
                emit(t.times[i],previous);emit(t.times[i],previous);
                emit(t.times[i],next);emit(t.times[i],next);previous=next;
            }
            if (decimated && fit_options) {
                const double tolerance = t.path == AnimationPath::Translation ? fit_options->translation_tolerance :
                    t.path == AnimationPath::Scale ? fit_options->scale_tolerance : fit_options->rotation_tolerance_radians;
                fits.push_back({t.target_node, t.path, tolerance, 0.0, 0.0, false, false, true,
                    transition_indices.size() * 4, 0, 0, 0, 0});
            }
            continue;
        }
        const float track_duration=t.times.back(); std::vector<float> sample_times;
        // Native initial state is an implicit time-zero control. Two records at
        // [0,T] trigger the position/rotation endpoint shortcut; [T/2,T] with the
        // source initial value gives linear vector motion. Quaternion Hermite is
        // still approximate. A matched initial value also removes singleton scale ramps.
        const bool linear_source = t.interpolation == AnimationInterpolation::Linear;
        std::array<float,4> first{};
        // Animated glTF channels replace the node rest value, including the hold
        // before a delayed first key. Unanimated native channels retain rest TRS.
        if(linear_source) {
            if(!gltf::sample_animation_track(t,0,first,error))return false;
            set_channel(states[it->second],t.path,first);
        }
        const bool bounded_two_key = linear_source && t.times.size() == 2 && t.times[0] == 0.0f &&
            std::isfinite(track_duration) && track_duration > 0.0f;
        const bool linear_pair = linear_source && t.times.size()==2;
        const bool linear_singleton = linear_source && t.times.size() == 1 && t.times[0] >= 0.0f;
        if(linear_pair || linear_singleton){
            if(linear_singleton) sample_times=t.times;
            else {
                std::array<float,4> final{};
                if(!gltf::sample_animation_track(t,track_duration,final,error))return false;
                bool constant=true;
                if(t.path==AnimationPath::Rotation){
                    bool same=true, negated=true;
                    for(std::uint32_t c=0;c<4;++c){same&=final[c]==first[c];negated&=final[c]==-first[c];}
                    constant=same||negated;
                } else for(std::uint32_t c=0;c<w;++c) if(final[c]!=first[c]){constant=false;break;}
                if(constant) sample_times={track_duration};
                else if(bounded_two_key) {
                    const float midpoint=static_cast<float>(static_cast<double>(track_duration)/2.0);
                    if(midpoint>0.0f&&midpoint<track_duration) sample_times={midpoint,track_duration};
                    else { sample_times={track_duration}; build_best_effort=true; }
                } else sample_times=t.times;
            }
        } else if(t.interpolation==AnimationInterpolation::Linear){sample_times=t.times;}
        else {
            constexpr std::size_t sample_limit = 10000;
            const double desired_intervals = std::ceil(static_cast<double>(track_duration) * 30.0);
            const bool grid_decimated = desired_intervals + 1.0 > static_cast<double>(sample_limit);
            const std::size_t grid_points = grid_decimated ? sample_limit :
                static_cast<std::size_t>(desired_intervals) + 1u;
            sample_times.reserve(std::min(sample_limit, grid_points + std::min(t.times.size(), sample_limit)));
            if (grid_points <= 1) sample_times.push_back(track_duration);
            else for (std::size_t n = 0; n < grid_points; ++n) {
                const float t0 = static_cast<float>(static_cast<double>(track_duration) *
                    static_cast<double>(n) / static_cast<double>(grid_points - 1));
                sample_times.push_back(t0);
            }
            const auto authored = bounded_sample_times(t.times, sample_limit);
            sample_times.insert(sample_times.end(), authored.begin(), authored.end());
            std::sort(sample_times.begin(), sample_times.end());
            sample_times.erase(std::unique(sample_times.begin(), sample_times.end()), sample_times.end());
            if (sample_times.size() > sample_limit) sample_times = bounded_sample_times(sample_times, sample_limit);
            if (grid_decimated || t.times.size() > sample_limit) build_best_effort = true;
        }
        std::array<float,4> previous{0,0,0,1}; bool have_previous=false;
        for(std::size_t sample_index=0;sample_index<sample_times.size();++sample_index){const float time=sample_times[sample_index];std::size_t k=0; while(k+1<t.times.size()&&t.times[k+1]<=time)++k; std::vector<float> v(w,0.0f); if(bounded_two_key||linear_singleton){std::array<float,4> sampled{};if(!gltf::sample_animation_track(t,time,sampled,error))return false;for(std::uint32_t c=0;c<w;++c)v[c]=sampled[c];} else if(t.interpolation==AnimationInterpolation::CubicSpline){const float* base=t.values.data()+k*t.value_components*3; for(std::uint32_t c=0;c<w;++c)v[c]=base[t.value_components+c]; if(k+1<t.times.size()&&time>t.times[k]){const float h=t.times[k+1]-t.times[k], u=(time-t.times[k])/h;const float* next=t.values.data()+(k+1)*t.value_components*3;for(std::uint32_t c=0;c<w;++c){float p0=base[t.value_components+c],p1=next[t.value_components+c],m0=base[2*t.value_components+c],m1=next[c];float u2=u*u,u3=u2*u;v[c]=(2*u3-3*u2+1)*p0+(u3-2*u2+u)*h*m0+(-2*u3+3*u2)*p1+(u3-u2)*h*m1;}}} else {const auto off=k*t.value_components;for(std::uint32_t c=0;c<w;++c)v[c]=t.values[off+c];}
            if(t.interpolation==AnimationInterpolation::CubicSpline){bool valid=true;for(float x:v)valid=valid&&std::isfinite(x);if(t.path==AnimationPath::Rotation){float n=0;for(float x:v)n+=x*x;valid=valid&&std::isfinite(n)&&n>=1e-20f;}if(!valid){build_best_effort=true;const std::size_t endpoint=(k+1<t.times.size()&&time-t.times[k]>=(t.times[k+1]-t.times[k])*0.5f)?k+1:k;const auto* p=t.values.data()+endpoint*t.value_components*3+t.value_components;for(std::uint32_t c=0;c<w;++c)v[c]=p[c];if(t.path==AnimationPath::Rotation){std::array<float,4> q{v[0],v[1],v[2],v[3]};if(!normalize_quaternion(q))q={0,0,0,1};for(std::uint32_t c=0;c<4;++c)v[c]=q[c];}}}
            if(t.path==AnimationPath::Rotation){std::array<float,4> q{v[0],v[1],v[2],v[3]};if(!normalize_quaternion(q)){error="animation contains a non-finite or zero-length quaternion";return false;}if(have_previous&&(q[0]*previous[0]+q[1]*previous[1]+q[2]*previous[2]+q[3]*previous[3])<0)for(float& x:q)x=-x;for(std::uint32_t c=0;c<4;++c)v[c]=q[c];previous=q;have_previous=true;}
            entries.push_back(Entry{time,0.0f,it->second,t.path,sample_index,std::move(v)});
        }
    }
    if(entries.empty()){error="animation has no supported tracks targeting this skin";return false;}
    if(entries.size()>std::numeric_limits<std::uint32_t>::max()) {error="animation entry count exceeds native uint32 limit";return false;}
    std::unordered_map<std::uint32_t,std::vector<float>> prior; for(auto&e:entries){auto key=(static_cast<std::uint32_t>(e.bone)<<8)|static_cast<std::uint32_t>(e.path);auto& times=prior[key];e.gate=times.size()<2?0.0f:times[times.size()-2];times.push_back(e.time);}
    std::stable_sort(entries.begin(),entries.end(),[](const Entry&a,const Entry&b){if(a.gate!=b.gate)return a.gate<b.gate;if(order(a.path)!=order(b.path))return order(a.path)<order(b.path);if(a.bone!=b.bone)return a.bone<b.bone;return a.key<b.key;});
    auto first=build_core(states,entries,duration,0);
    if(first.size()>std::numeric_limits<std::uint32_t>::max()) {error="animation body exceeds native uint32 size limit";return false;}
    auto body=build_core(states,entries,duration,static_cast<std::uint32_t>(first.size()));
    if(body.size()!=first.size()){error="animation body size instability";return false;}
    auto cooked=wrap_cooked_resource("animation",resource_name,body);if(!validate_skeletal_animation(cooked,static_cast<std::uint32_t>(skin.joints.size()),error))return false;
    if(fit_options && !fits.empty()) {
        // Verify that serialization and the shared cursor preserve the exact
        // controls evaluated by the fitter, including every final probe. This
        // avoids repeating an O(bones * keys) full-pose query for each probe.
        EvaluationClip decoded;
        if(!decode_evaluation_clip(cooked,decoded,error))return false;
        if(decoded.initial.size()!=states.size() || decoded.keys.size()!=entries.size()) {
            error="serialized animation changed fitted control counts";return false;
        }
        for(std::size_t i=0;i<states.size();++i) {
            const auto& a=decoded.initial[i];const auto& b=states[i];
            if(!std::equal(b.p.begin(),b.p.end(),a.position.begin()) || a.rotation!=b.q ||
               !std::equal(b.s.begin(),b.s.end(),a.scale.begin())) {
                error="serialized animation changed fitted initial state";return false;
            }
        }
        for(std::size_t i=0;i<entries.size();++i) {
            const auto& a=decoded.keys[i];const auto& b=entries[i];
            if(a.bone!=b.bone || a.path!=b.path || a.time!=b.time || a.gate!=b.gate ||
               !std::equal(b.values.begin(),b.values.end(),a.value.begin())) {
                error="serialized animation changed fitted controls or eligibility";return false;
            }
        }
    }
    out.resource_name=std::move(resource_name);out.clip_name=source.name;out.cooked=std::move(cooked);out.duration=duration;out.track_entries=static_cast<std::uint32_t>(entries.size());out.best_effort=build_best_effort;
    out.fit_options=(fit_options && !fits.empty())?std::optional<FitOptions>(*fit_options):std::nullopt;out.fits=std::move(fits);return true;
}

bool validate_skeletal_animation(const std::vector<std::uint8_t>&cooked,std::uint32_t expected_bones,std::string&error){
    std::vector<std::uint8_t>body;std::string stream;if(!parse_cooked_resource_envelope(cooked,"animation",body,stream,error))return false;if(!stream.empty()){error="animation unexpectedly declares an external stream";return false;}if(body.size()<28){error="animation body is truncated";return false;}const std::uint32_t bones=read<std::uint32_t>(body,8);const float duration=read<float>(body,12);const std::uint32_t file_size=read<std::uint32_t>(body,16);const std::uint16_t marker=read<std::uint16_t>(body,24);if(marker!=7||bones!=expected_bones||file_size!=body.size()||!std::isfinite(duration)||duration<0){error="animation marker/header mismatch";return false;}std::size_t p=26ull+bones*40ull;if(p>body.size()){error="animation base-state table is truncated";return false;}for(std::uint32_t bone=0;bone<bones;++bone){const std::size_t base=26ull+static_cast<std::size_t>(bone)*40ull;for(std::size_t i=0;i<10;++i)if(!std::isfinite(read<float>(body,base+i*4))){error="animation base-state value is invalid";return false;}float norm=0;for(std::size_t i=3;i<7;++i){const float v=read<float>(body,base+i*4);norm+=v*v;}if(std::abs(std::sqrt(norm)-1.0f)>1e-3f){error="animation base-state quaternion is not normalized";return false;}}
    std::unordered_map<std::uint32_t,std::vector<Entry>> channel_records;std::unordered_map<std::uint32_t,std::vector<float>> history;std::unordered_map<std::uint32_t,std::array<float,4>> previous;std::tuple<float,int,std::uint16_t,std::size_t> last_schedule{};bool first=true;while(p+2<=body.size()){auto t=read<std::uint16_t>(body,p);p+=2;if(t==3){
        if(p!=body.size()){error="animation has trailing bytes";return false;}
        for(const auto& channel:channel_records){
            const auto bone=channel.first>>8;const auto kind=channel.first&255;
            const std::size_t count=kind==5?4:3;
            const std::size_t offset=26ull+bone*40ull+(kind==4?0:kind==5?12:28);
            std::vector<float> initial(count);
            for(std::size_t c=0;c<count;++c)initial[c]=read<float>(body,offset+4*c);
            if(!validate_boundary_groups(channel.second,std::move(initial),error))return false;
        }
        return true;
    }if(t<4||t>6||p+6>body.size()){error="animation track header is invalid";return false;}auto bone=read<std::uint16_t>(body,p);p+=2;if(bone>=bones){error="animation track bone is out of range";return false;}const float time=read<float>(body,p);p+=4;if(!std::isfinite(time)||time<0||time>duration+1e-5f){error="animation record time is invalid";return false;}const std::size_t floats=t==5?4:3;if(p+floats*4>body.size()){error="animation track value is truncated";return false;}std::vector<float> value(floats);for(float&v:value){v=read<float>(body,p);p+=4;if(!std::isfinite(v)){error="animation record value is invalid";return false;}}const auto key=(static_cast<std::uint32_t>(bone)<<8)|t;auto& h=history[key];if(!h.empty()&&time<h.back()){error="animation track times decrease";return false;}channel_records[key].push_back({time,0,bone,t==4?AnimationPath::Translation:t==5?AnimationPath::Rotation:AnimationPath::Scale,h.size(),value});const float gate=h.size()<2?0.0f:h[h.size()-2];const int path_order=t==6?0:(t==5?1:2);const auto schedule=std::make_tuple(gate,path_order,bone,h.size());if(!first&&schedule<last_schedule){error="animation interleaving violates deterministic shared-cursor eligibility order";return false;}first=false;last_schedule=schedule;h.push_back(time);if(t==5){float norm=0;for(float v:value)norm+=v*v;if(std::abs(std::sqrt(norm)-1.0f)>1e-3f){error="animation quaternion is not normalized";return false;}auto q=std::array<float,4>{value[0],value[1],value[2],value[3]};auto pi=previous.find(key);if(pi!=previous.end()&&q[0]*pi->second[0]+q[1]*pi->second[1]+q[2]*pi->second[2]+q[3]*pi->second[3]<-1e-5f){error="animation quaternion sign continuity violated";return false;}previous[key]=q;}}error="animation terminator is missing";return false;
}

bool write_skeletal_animation(const BuiltAnimation&a,const std::filesystem::path&path,std::string&error){if(a.cooked.empty()){error="animation is empty";return false;}std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);std::ofstream f(path,std::ios::binary);if(!f){error="cannot open animation output: "+path.string();return false;}f.write(reinterpret_cast<const char*>(a.cooked.data()),static_cast<std::streamsize>(a.cooked.size()));if(!f){error="failed writing animation: "+path.string();return false;}return true;}

} // namespace dtglb::stingray::animation
