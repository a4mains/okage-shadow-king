#pragma once
#include "okage/actor.hpp"
#include "okage/field_projection.hpp"

namespace okage {
struct FieldShadowSettings {
    Float4 direction{1,1,0,0};
    std::array<std::uint32_t,3> tint{255,64,192};
    std::uint32_t threshold=10;
};
FieldShadowSettings field_shadow_settings(Bytes stage);
unsigned field_depth_bucket(const Matrix4& view,const Float4& position,const ShadowScreenSettings&,unsigned depth_shift=19);
unsigned field_ordinary_bucket(std::uint32_t flags,unsigned depth,bool special_actor=false);
struct FieldShadowActor {
    int alias=0;
    const FieldActor* actor=nullptr;
    std::uint32_t flags=0;
    std::optional<unsigned> selector;
    std::optional<Model> companion;
    std::vector<std::vector<ModelBlockBound>> bounds;
    bool ordinary_after_shadows() const {return ((flags>>5)&3)!=0||(flags&0x40000);}
};
FieldShadowActor make_field_shadow_actor(int alias,std::string_view model_name,
    const FieldActor&,const ModelFile* companion,bool height_enabled=true);
// Explicit instance+24 flags and constructor337e70 height mode; no alias rules.
FieldShadowActor make_instance_shadow_actor(int alias,const FieldActor&,
    const ModelFile* companion,std::uint32_t flags,bool height_enabled);
struct FieldShadowDraw {ShadowPrism prism;std::array<std::uint32_t,3> color;};
struct FieldShadowSubmission {
    int alias;
    unsigned selector,bucket,blocks,triangles,emitted;
};
struct FieldShadowFrame {
    std::vector<FieldShadowDraw> draws;
    std::vector<FieldShadowSubmission> submissions; // GPU execution order.
};
FieldShadowFrame field_shadow_frame(std::span<const FieldShadowActor>,const Matrix4& view,
    const FieldProjectionFrame&,const FieldShadowSettings&,float player_factor,const SineTable&,unsigned depth_shift=19);
}
