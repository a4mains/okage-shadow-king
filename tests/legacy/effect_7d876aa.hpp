#pragma once
#include "okage/effect.hpp"
namespace okage::legacy {
struct MapEffectElement {
    Float4 position{},rotation{},scale{},color{},interval{};
    std::uint32_t id=0,shape=0,flags=0,primitive_flags=0,program=0,next_program=0;
    float change_time=0;
    std::array<Float4,4> rendered{};
    std::uint32_t texture=0,render_flags=0;
    std::array<std::uint16_t,4> uv{};
    std::vector<std::int16_t> ring_y;
    std::vector<std::uint16_t> cylinder_t;
    std::optional<std::uint32_t> cylinder_source_id;
    // Torch91 uses independent mutable flame quads and atlas coordinates.
    EffectQuadPositions quad_positions{};
    std::array<std::array<std::uint16_t,2>,4> quad_uv{};
    std::vector<std::array<std::int16_t,3>> strip_positions;
    std::vector<std::uint8_t> strip_tags;
    std::vector<std::array<std::uint16_t,2>> strip_uv;
    std::optional<std::uint32_t> strip_source_id;
    std::shared_ptr<EffectWaterSurface> water;
    //Shape33: three tagged quads, each retaining its four packed UV pairs.
    std::array<std::array<std::uint16_t,2>,12> number_uv{};
};
class MapEffect {
public:
    MapEffect(const EffectDefinition&,const Float4& position,const Float4& rotation,
              const Float4& scale,const EffectTextureSizes&,
              const std::function<std::uint32_t()>& random,EffectStart start=EffectStart::RandomPhase);
    void bind_flame_geometry(std::shared_ptr<EffectFlameGeometry>);
    const EffectQuadPositions& flame_positions(const MapEffectElement&) const;
    MapEffect snapshot() const;
    void set_parameter(unsigned selector,const Float4& payload);
    //0x1016a8 writes signed integer -> float into job parameters+50..5f.
    void set_numeric_parameter(std::int32_t);
    void rebind_water_texture(unsigned element,std::uint32_t native_texture_slot,
                              std::uint32_t width,std::uint32_t height);
    void tick(const std::function<std::uint32_t()>& random,bool clock_enabled=true,
              const Float4* camera_world_position=nullptr,
              const EffectProjectionContext* projection=nullptr);
    void request_removal(){flags|=0x100;}
    const EffectTextureSizes& texture_sizes() const{return texture_sizes_;}
    std::uint32_t id=0,flags=0,duration=0,phase=0;
    float inverse_duration=0;
    float near_clip=3; // Job constructor0x10d338, +0x78.
    unsigned clamp_mode=2; // Job+0x74; water initializer switches to region repeat.
    Float4 position{},rotation{},scale{},color{1,1,1,1},parameters{};
    bool source_position_w_available=true;
    bool source_color_rgb_available=true;
    // Mode0 attachment0x101118 copies this actor's position/rotation before
    // the effect update. Ownership follows the job, including removal/reuse.
    std::optional<int> actor_alias;
    std::vector<MapEffectElement> elements;
    std::shared_ptr<const std::vector<MapEffectVertex>> shape55_geometry;
    std::shared_ptr<EffectFlameGeometry> flame_geometry;
private:
    void set_texture(MapEffectElement&,unsigned);
    EffectTextureSizes texture_sizes_;
    inline static const SineTable sine_=make_sine_table();
};
struct MapEffectDraw {
    Matrix4 matrix;
    Float4 color;
    std::uint32_t texture;
    std::array<std::uint16_t,4> uv;
    // Local source positions, byte-range source colors and normalized GS S/T.
    std::vector<MapEffectVertex> vertices;
    bool additive=true;
    bool depth_test=true;
    bool subtractive=false;
    unsigned clamp_mode=2;
};
std::vector<MapEffectDraw> map_effect_draws(const MapEffect&,const Matrix4& view,
    const Float4& camera_rotation,float far_clip,const SineTable&,const Float4* light_tint=nullptr,
    EffectDrawPass pass=EffectDrawPass::Ordinary,const EffectTextureSizeResolver& texture_size={});
}
