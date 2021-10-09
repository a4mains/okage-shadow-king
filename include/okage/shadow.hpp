#pragma once
#include "okage/pose.hpp"
#include <cstdint>
#include <optional>

namespace okage {
struct ShadowTransition {
    std::uint32_t phase=0,locked=0,follow_disabled=0;
    std::int32_t voice=-1;
    float factor=0;
    std::uint16_t hero_shadow=4;
    bool stun_visible=false;
    //0x1b7f88 writes a mode4 shadow-projection parameter, not HERO opacity.
    float projection_factor=1;
    Float4 stun_rotation{},stun_position{},stun_scale{1,1,1,1};
    void tick(const Float4& hero_rotation,const Float4& hero_position);
};
struct ShadowProjection {
    std::array<std::uint32_t,3> color{};
    bool has_matrices=false;
    std::array<Matrix4,2> matrices{};
    std::optional<Float4> direction;
};
struct ShadowProjectionContext {
    float slope=0,span=0;
    Matrix4 view{},inverse{};
    std::array<Matrix4,2> planes{};
    Float4 world_direction{},view_direction{};
    ShadowProjectionContext(const Float4& light_direction,const Matrix4& camera_view);
    ShadowProjection project(unsigned mode,float height,float factor,
        const std::array<std::uint32_t,3>& tint={255,64,192}) const;
};
//0x1b6b40 shares the main bone channels and remaps non-root shadow hierarchy
// entries by raw names. Geometry and parent links remain those of the shadow.
Model bind_shadow_model(const Model& main,const Model& shadow);
struct ShadowBatch {
    std::vector<std::uint16_t> positions; // First-use source position indices.
    std::vector<std::vector<std::uint16_t>> polygons; // Remapped triangles/quads.
};
std::vector<ShadowBatch> shadow_batches(const RenderBlock&,bool positive_material);
std::vector<Float4> shadow_posed_positions(const RenderBlock&,std::span<const Matrix4> pose);
// Full perspective/viewport setup from0x1acc10. Depth mapping planes are
// independent of clipping planes; field rendering uses both pairs.
struct ShadowScreenSettings {
    float focal,scale_x,scale_y,center_x,center_y,depth_min,depth_max;
    float depth_near,depth_far,clip_near,clip_far,fog_near,fog_far,half_width,half_height;
};
using ShadowScreenParameters=std::array<Float4,8>;
ShadowScreenParameters shadow_screen_parameters(const ShadowScreenSettings&);
struct ModelBlockBound {
    Float4 center;
    float radius;
    std::uint32_t hierarchy_node;
};
// Original0x181e40 command scan. Bounds are local to each selected hierarchy,
// dehomogenized before min/max; shadow compilation doubles the final radius.
std::vector<ModelBlockBound> model_block_bounds(const RenderBlock&,bool shadow);
struct ModelBlockSelection { std::uint32_t block; float depth; };
std::vector<ModelBlockSelection> visible_model_blocks(
    std::span<const std::vector<ModelBlockBound>>,std::span<const Matrix4> camera_pose,
    const ShadowScreenSettings&,bool culling=true);
struct ShadowVertexCache {
    std::vector<Float4> camera,screen;
    std::vector<std::uint32_t> clip;
    std::int32_t clip_status=0;
};
ShadowVertexCache shadow_vertex_cache(std::span<const Float4> positions,
    const std::array<Matrix4,2>& projections,const ShadowScreenParameters&);
struct ShadowPrismInput {
    std::array<Float4,6> camera{},screen{}; // Lower triangle, then upper triangle.
    std::array<std::uint32_t,6> clip{};
    Float4 direction{};
    std::int32_t clip_status=0;
};
struct ShadowPrism {
    bool emitted=false;
    std::array<std::array<std::array<std::uint32_t,4>,12>,3> passes{};
};
ShadowPrism shadow_prism(const ShadowPrismInput& input);
struct ShadowDraw {
    std::uint32_t block,batch,triangle;
    ShadowPrism prism;
};
std::vector<ShadowDraw> shadow_draws(const Model&,std::span<const Matrix4> camera_pose,
    const ShadowProjection&,const ShadowScreenParameters&,const Float4& default_direction,
    bool positive_material=false);
std::vector<ShadowDraw> shadow_draw_blocks(const Model&,std::span<const Matrix4> camera_pose,
    const ShadowProjection&,const ShadowScreenParameters&,const Float4& default_direction,
    std::span<const ModelBlockSelection>,bool positive_material=false);
}
