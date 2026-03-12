#pragma once
#include "okage/pose.hpp"
#include <memory>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>

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
struct EffectWaterPacket {
    std::array<std::int16_t,1024> y{};
    std::array<std::array<std::uint16_t,2>,1024> uv{};
};
struct EffectWaterSurface {
    std::array<std::array<std::uint32_t,1024>,2> heights{};
    std::array<std::array<std::int32_t,2>,1024> coordinates{};
    std::array<std::int16_t,1024> packed_y{};
    std::array<std::array<std::uint16_t,2>,1024> packed_uv{};
    std::array<EffectWaterPacket,2> packets{};
    unsigned selected_packet=0;
    unsigned active=0,rate=0,rate_counter=0,period=0,period_counter=0;
    bool initialized=false;
};
using EffectQuadPositions=std::array<std::array<std::int16_t,3>,4>;
// Shape28 factory packet: template34 retains its shared mutable XYZ storage.
struct EffectFlameGeometry {
    EffectQuadPositions positions{{{32,-64,0},{-32,-64,0},{32,0,0},{-32,0,0}}};
};
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
    //0x105518 geometry links (mode3 / time17) for shapes30/31 and25: the
    //element draws the source element's current UV packet / ring heights.
    std::optional<std::uint32_t> quad_source_id,ring_source_id;
};
std::uint32_t effect_shape_texture(unsigned shape);
enum class EffectStart {RandomPhase, FirstFrame};
struct MapEffectVertex {Float4 position,color,uv;};
using EffectNoiseGrid=std::array<std::uint32_t,8*8*6>;
EffectNoiseGrid make_effect_noise_grid(const std::function<std::uint32_t()>& random);
std::vector<MapEffectVertex> make_effect_noise_shape(unsigned shape,const EffectNoiseGrid&);
struct ProceduralEffectGeometry {
    std::vector<MapEffectVertex> vertices;
    // Expanded alongside triangle vertices; animation tags are not homogeneous W.
    std::vector<std::uint8_t> position_tags,uv_tags;
};
// First-use ray/surface/spray factories. Texture dimensions are owned XIM data.
ProceduralEffectGeometry make_procedural_effect_shape(unsigned shape,const SineTable&,
    const std::function<std::uint32_t()>& random,const std::array<std::uint32_t,2>& texture_size={});
// Optional observation of one preparation (0x10bc78 -> 0x10e098 grid, then the
// 0x10ba38 template pass). Never changes what is prepared or drawn.
struct EffectPreparationTrace {
    struct Request {std::uint32_t definition,element,shape;bool constructed;};
    // Called once after the 384-cell grid, before the first template request.
    std::function<void()> grid_complete;
    std::vector<Request> requests;                // template requests in pass order
    std::vector<std::uint32_t> construction_order;   // shapes in first-construction order
    std::array<std::uint32_t,66> construction_draws{};
    std::uint32_t grid_draws=0;
};
struct EffectTemplatePreparation {
    std::shared_ptr<EffectFlameGeometry> flame=std::make_shared<EffectFlameGeometry>();
    EffectNoiseGrid noise;
    std::array<std::array<std::uint32_t,2>,66> cache_owners{};
    std::array<std::shared_ptr<const ProceduralEffectGeometry>,66> geometry{};
    EffectTemplatePreparation(const std::vector<EffectDefinition>&,const EffectTextureSizes&,
        const SineTable&,const std::function<std::uint32_t()>& random,EffectPreparationTrace* trace=nullptr);
};
//0x1086c4: actual live view and1842a0 projection parameter records.
//The caller supplies the current resource state before the job near setter.
struct EffectProjectionContext {
    Matrix4 view;
    std::array<Float4,8> screen;
    float focal; //0x183e88
};
class MapEffect {
public:
    MapEffect(const EffectDefinition&,const Float4& position,const Float4& rotation,
              const Float4& scale,const EffectTextureSizes&,
              const std::function<std::uint32_t()>& random,EffectStart start=EffectStart::RandomPhase);
    void bind_flame_geometry(std::shared_ptr<EffectFlameGeometry>);
    //Shared session preparation (0x10bc78): procedural shapes10..17/43/45/
    //55/56/62 draw its cached geometry. Also binds shape55 and the flame packet.
    void bind_preparation(std::shared_ptr<const EffectTemplatePreparation>);
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
    std::shared_ptr<const EffectTemplatePreparation> preparation;
    //0x101960: job flag1/element0x10 give a private XYZ packet, job flag2/
    //element0x40 a private UV packet; otherwise the element uses the shared
    //0x24e790 shape cache.
    bool private_positions(const MapEffectElement& e) const{return (flags&1)||(e.flags&0x10);}
    bool private_uvs(const MapEffectElement& e) const{return (flags&2)||(e.flags&0x40);}
private:
    void set_texture(MapEffectElement&,unsigned);
    EffectTextureSizes texture_sizes_;
    inline static const SineTable sine_=make_sine_table();
};
using MapEffects=std::array<std::unique_ptr<MapEffect>,84>;
std::vector<MapEffectVertex> make_shadow_effect_shape(unsigned shape,const SineTable&,
    const std::function<std::uint32_t()>& random);
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
enum class EffectDrawPass {Ordinary, Deferred};
struct EffectMaterial {std::uint64_t zbuf=0,alpha=0,test=0;};
EffectMaterial compile_effect_material(std::uint32_t primitive_flags);
struct EffectDrawState {bool additive=false,subtractive=false,depth_test=false;};
EffectDrawState effect_draw_state(std::uint32_t primitive_flags);
using EffectTextureSizeResolver=std::function<std::array<std::uint32_t,2>(std::uint32_t)>;
std::vector<MapEffectDraw> map_effect_draws(const MapEffect&,const Matrix4& view,
    const Float4& camera_rotation,float far_clip,const SineTable&,const Float4* light_tint=nullptr,
    EffectDrawPass pass=EffectDrawPass::Ordinary,const EffectTextureSizeResolver& texture_size={});
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
    inline static const SineTable sine_=make_sine_table();
};
struct AttachedEffect {
    BubbleEffect animation;
    int actor_alias;
    Float4 position{};
    float near_clip=3;
    AttachedEffect(const EffectDefinition& d,int parameter,int actor):animation(d,parameter),actor_alias(actor){}
};
using AttachedEffects=std::array<std::unique_ptr<AttachedEffect>,84>;
//0x1036d8 marks scene slots for the ordinary effect deletion callbacks. No
// animation, RNG or actor lookup is legal after their field owners retire.
struct RetiringSceneEffects {
    MapEffects map;
    AttachedEffects attached;
    void mark();
    void tick();
    std::array<std::uint32_t,84> flags() const;
    // Slots the last tick() reclaimed (0x102218 0x1023c4..0x102460); observation only.
    std::vector<unsigned> last_reclaimed;
};
struct EffectSubmission {
    unsigned handle;
    std::variant<MapEffect,AttachedEffect> effect;
};
using EffectSubmissions=std::vector<EffectSubmission>;
}
