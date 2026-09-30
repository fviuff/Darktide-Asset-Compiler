#pragma once
#include <array>
#include <string>
namespace dtglb { struct AnimationTrack; }
namespace dtglb::gltf {
// Non-owning validated view. The source must remain alive and unchanged while
// sampling. Repeated queries validate their time, not the entire source array.
class PreparedAnimationSampler {
public:
    bool initialize(const AnimationTrack& track, std::string& error);
    bool sample(float time, std::array<float,4>& out, std::string& error) const;
    bool approximation_used() const { return approximation_used_; }
private:
    const AnimationTrack* track_ = nullptr;
    mutable bool approximation_used_ = false;
};
bool sample_animation_track(const AnimationTrack& track, float time,
                            std::array<float,4>& out, std::string& error);
}
