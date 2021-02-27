#pragma once
#include "okage/pose.hpp"
#include <memory>

namespace okage {
struct EffectElement {
    std::uint32_t flags=0,id=0,shape=0,primitive_flags=0,channels=0,program=0;
    std::int32_t texture=-1;
    float change_time=0;
    std::uint32_t next_program=0;
    // EFD order: color, rotation, position, scale, interval/parameters.
    std::array<Float4,5> values{};
};
struct EffectDefinition {
    std::uint32_t id=0,duration=0,flags=0;
    Float4 parameters{};
    std::vector<EffectElement> elements;
};
// Structural EFD import; reference templates remain explicitly marked by flag
// 0x800. Reading a definition does not imply support for its runtime program.
std::vector<EffectDefinition> read_efd(Bytes bytes);

struct EffectSprite {
    Float4 position{},rotation{},scale{},color{};
    float damping=0,previous_time=0;
    std::uint32_t program=0,texture=0;
    std::array<std::uint16_t,4> uv{};
};
class BubbleEffect {
public:
    BubbleEffect(const EffectDefinition&,int parameter);
    void tick(const Float4& camera_rotation);
    void request_removal() {flags|=0x100;}
    std::uint32_t phase=1,flags=0;
    std::array<EffectSprite,2> sprites;
private:
    EffectDefinition definition_;
    int parameter_;
    SineTable sine_=make_sine_table();
};
struct AttachedEffect {
    BubbleEffect animation;
    int actor_alias;
    bool player_contact=false;
    Float4 position{};
    AttachedEffect(const EffectDefinition& d,int parameter,int actor):animation(d,parameter),actor_alias(actor){}
};
using AttachedEffects=std::array<std::unique_ptr<AttachedEffect>,84>;
}
