#pragma once
#include "scene/scene.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::animation {
struct EvaluatedPose {
    std::array<float,4> position{};
    std::array<float,4> rotation{0,0,0,1};
    std::array<float,4> scale{1,1,1,0};
};
struct EvaluationKey {
    std::uint16_t bone = 0;
    AnimationPath path = AnimationPath::Translation;
    float time = 0;
    float gate = 0;
    std::array<float,4> value{};
};
// Only the compiler's validated marker-7/full-float profile is decoded here.
// Mathematical consumer model; not a bitwise reproduction of engine arithmetic.
struct EvaluationClip {
    float duration = 0;
    std::vector<EvaluatedPose> initial;
    std::vector<EvaluationKey> keys;
};
struct SampledAnimationError {
    bool available = false;
    std::string error;
    std::uint32_t samples = 0;
    double translation = 0;
    double scale = 0;
    double rotation_radians = 0;
};
bool decode_evaluation_clip(const std::vector<std::uint8_t>& cooked, EvaluationClip& out, std::string& error);
bool sample_native_animation(const EvaluationClip& clip, float time, std::vector<EvaluatedPose>& out, std::string& error);
// Samples one emitted continuous track using the same rolling four-control
// window as the full evaluator. `times` and `values` are the emitted
// positive-time controls; `initial` is the implicit control at time zero.
// The caller has already validated that times are finite, positive, and
// strictly increasing, and that all relevant values are finite. This keeps a
// query logarithmic rather than re-validating an entire track per sample. STEP
// tracks use their separate boundary-pair representation and are not sampled
// through this helper.
bool sample_native_track(const std::array<float,4>& initial, AnimationPath path,
                         const std::vector<float>& times,
                         const std::vector<std::array<float,4>>& values,
                         float time, std::array<float,4>& out, std::string& error);
bool measure_animation_error(const std::vector<std::uint8_t>& cooked, const SkinInfo& skin,
                             const AnimationInfo& source, SampledAnimationError& out, std::string& error);
}
