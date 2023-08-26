#include "okage/scene_resource.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
namespace okage {
void reset_scene_camera(Animation& camera) {
    camera=Animation(3);
    const std::array<std::array<std::uint32_t,4>,2> defaults={{{0x415170a4,0xc1200000,0xc3510000,0x3f800000},
        {0x3f79c77a,0x3d1ba5e3,0,0x3f800000}}};
    for(unsigned i=0;i<2;++i) {
        Float4 value;for(unsigned lane=0;lane<4;++lane)value[lane]=std::bit_cast<float>(defaults[i][lane]);
        camera.set_channel(i,value);
    }
}
void initialize_scene_ui(BattleUiShared& shared) {
    if(std::any_of(shared.registry.begin(),shared.registry.end(),[](const auto& window){return bool(window);}))
        throw std::runtime_error("Fresh scene resource still has live UI windows");
    shared.resource.initialize_scene();
}
EmptySceneResource::EmptySceneResource(SceneClearColor& clear,SceneFade& fade,BattleUiShared& shared) {
    //100ef8 resets the draw environment and fade color, not fade intensity.
    clear.rgb={0,0,0};fade.color=0;
    initialize_scene_ui(shared);
}
void EmptySceneResource::tick(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,
    BattleUiShared& shared,std::shared_ptr<const std::vector<Font>> fonts,const SineTable& sine,GameRandom& random) {
    if(!live_)throw std::runtime_error("Dead empty resource callback");
    if(std::any_of(shared.registry.begin(),shared.registry.end(),[](const auto& window){return bool(window);}))
        throw std::runtime_error("Empty scene resource has unexpected UI owners");
    //1b9760 updates/publishes the default camera even with empty ranges.
    camera.tick_channels();camera_position_=camera.value(0);camera_rotation_=camera.value(1);
    projection.submit_empty_resource_frame(true,settings_.cached_depth_min,settings_.cached_depth_max);
    if(effects)effects->tick();
    if(shared.resource.advance_resource(shared.registry,{},shared.resource_accumulators,
        std::move(fonts),sine,random,ui_projection_))
        throw std::runtime_error("Empty scene resource unexpectedly queued UI");
    std::fill(image_.rgba.begin(),image_.rgba.end(),0);
    for(std::size_t i=3;i<image_.rgba.size();i+=4)image_.rgba[i]=64;
    frame_ready_=true;
}
void EmptySceneResource::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& shared) {
    if(!live_)throw std::runtime_error("Empty scene resource retired twice");
    if(effects)effects->mark();
    for(auto& window:shared.registry)window.reset();
    settings_=StageSettings(settings_.clear);projection.reset_scene();reset_scene_camera(camera);
    frame_ready_=false;live_=false;
}
SceneClearColor& DefeatFieldReturn::initialize(std::span<std::uint8_t> variables,
    const std::array<std::uint32_t,3>& command,SceneFade& fade,SceneClearColor& clear) {
    if(command[0]!=2||variables.size()<0x110)
        throw std::runtime_error("Defeat field constructor requires game command2 and bank0");
    if(fade.color!=0)throw std::runtime_error("Colored immediate defeat-return fade publication is unrecovered");
    for(unsigned at=0x110;at>0x10;)variables[--at]=0;
    fade.current=0;fade.request(0);
    return clear;
}
DefeatFieldReturn::DefeatFieldReturn(std::span<std::uint8_t> variables,std::array<std::uint32_t,3>& command,
    SceneFade& fade,SceneClearColor& clear,BattleUiShared& shared):resource_(initialize(variables,command,fade,clear),fade,shared) {
    phase_=0x14;constructor_flag_=1;command={};
}
void DefeatFieldReturn::tick(const std::function<void(GameModeSelection)>& request) {
    if(phase_!=0x14||constructor_flag_!=1||!resource_.live()||!request)
        throw std::runtime_error("Defeat return field callback ownership");
    request({8,4,0});
}
void DefeatFieldReturn::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,
    BattleUiShared& shared,SceneFade& fade) {
    resource_.retire(camera,projection,effects,shared);
    //133a38 flag1 skips map/RUW/PME/player frees; source final write remains.
    fade.step=std::bit_cast<float>(0x3ca3d70au);
}
}
