#pragma once
#include "okage/pose.hpp"
#include <memory>
#include <functional>
#include <optional>
#include <string_view>

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
    std::optional<std::uint32_t> runtime_id;
    Float4 position{},rotation{},scale{1,1,1,1},color{1,1,1,1};
};
// Structural EFD import; reference templates remain explicitly marked by flag
// 0x800. Reading a definition does not imply support for its runtime program.
std::vector<EffectDefinition> read_efd(Bytes bytes);
// Original0x10ba38 imports ordinary templates first, then references in file
// order. Header duration/flags/parameters of a reference record are not applied.
std::vector<EffectDefinition> resolve_effect_references(std::vector<EffectDefinition>);

// Retail ENV_TEX logical order; dimensions come from the owned XIM files.
inline constexpr std::array<std::string_view,31> effect_texture_names={
    "niji.xim","splash_a.xim","wat_4.xim","exp9.xim","f2.xim","h29_4.xim",
    "hit!.xim","spark.xim","smoke_4.xim","thunder.xim","oldskilt4.xim","water.xim",
    "suuji.xim","lgtballm4.xim","fuki32b.xim","all.xim","all2.xim","kemuri.xim",
    "thunderball.xim","6eff01.xim","6eff02.xim","5sen.xim","exp_c.xim","heart.xim",
    "onnpu.xim","wind.xim","treasure.xim","page.xim","odoro.xim","aura.xim","kyusyu16.xim"};
using EffectTextureSizes=std::array<std::array<std::uint32_t,2>,31>;
struct MapEffectElement {
    Float4 position{},rotation{},scale{},color{},interval{};
    std::uint32_t id=0,shape=0,flags=0,primitive_flags=0,program=0,next_program=0;
    float change_time=0;
    std::array<Float4,4> rendered{};
    std::uint32_t texture=0,render_flags=0;
    std::array<std::uint16_t,4> uv{};
    std::vector<std::int16_t> ring_y;
    // Torch91 uses independent mutable flame quads and atlas coordinates.
    std::array<std::array<std::int16_t,3>,4> quad_positions{};
    std::array<std::array<std::uint16_t,2>,4> quad_uv{};
};
enum class EffectStart {RandomPhase, FirstFrame};
class MapEffect {
public:
    MapEffect(const EffectDefinition&,const Float4& position,const Float4& rotation,
              const Float4& scale,const EffectTextureSizes&,
              const std::function<std::uint32_t()>& random,EffectStart start=EffectStart::RandomPhase);
    void set_parameter(unsigned selector,const Float4& payload);
    void tick(const std::function<std::uint32_t()>& random,bool clock_enabled=true,
              const Float4* camera_world_position=nullptr);
    void request_removal(){flags|=0x100;}
    const EffectTextureSizes& texture_sizes() const{return texture_sizes_;}
    std::uint32_t id=0,flags=0,duration=0,phase=0;
    float inverse_duration=0;
    Float4 position{},rotation{},scale{},color{1,1,1,1},parameters{};
    std::vector<MapEffectElement> elements;
private:
    void set_texture(MapEffectElement&,unsigned);
    EffectTextureSizes texture_sizes_;
    SineTable sine_=make_sine_table();
};
using MapEffects=std::array<std::unique_ptr<MapEffect>,84>;
struct MapEffectVertex {Float4 position,color,uv;};
struct MapEffectDraw {
    Matrix4 matrix;
    Float4 color;
    std::uint32_t texture;
    std::array<std::uint16_t,4> uv;
    // Local source positions, byte-range source colors and normalized GS S/T.
    std::vector<MapEffectVertex> vertices;
    bool additive=true;
};
std::vector<MapEffectDraw> map_effect_draws(const MapEffect&,const Matrix4& view,
    const Float4& camera_rotation,float far_clip,const SineTable&,const Float4* light_tint=nullptr);
Float4 effect_light_tint(const Float4& base,const Float4& ambient,const Float4& light);

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
