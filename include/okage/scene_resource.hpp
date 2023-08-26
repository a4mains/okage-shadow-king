#pragma once
#include "okage/animation.hpp"
#include "okage/battle_interface.hpp"
#include "okage/field_projection.hpp"
#include "okage/stage_settings.hpp"
#include "okage/game_mode.hpp"
#include "okage/ui_projection.hpp"
#include "okage/ruw.hpp"
#include "okage/dialogue.hpp"

namespace okage {
struct SceneStageScratch {
    std::array<std::uint8_t,0x278> first{};
    std::array<std::array<std::uint8_t,0x348>,3> other{};
    std::array<std::uint32_t,10> selectors{0x22,0x1c};
    std::string name="LOAD_DEFAULT";
    unsigned camera_settings_count=0;
    SceneStageScratch() {
        first[0]='S';first[1]='T';first[2]='G';first[4]=0x20;first[8]=0x20;
    }
};
void reset_scene_camera(Animation&);
void initialize_scene_ui(BattleUiShared&);

class EmptySceneResource {
public:
    EmptySceneResource(SceneClearColor&,SceneFade&,BattleUiShared&);
    void tick(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&,
              std::shared_ptr<const std::vector<Font>>,const SineTable&,GameRandom&);
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&);
    bool live() const {return live_;}
    const Texture* rendered_image() const {return live_&&frame_ready_?&image_:nullptr;}
    const Float4& camera_position() const {return camera_position_;}
    const Float4& camera_rotation() const {return camera_rotation_;}
private:
    bool live_=true,frame_ready_=false;
    SceneStageScratch scratch_;
    StageSettings settings_;
    Float4 camera_position_{},camera_rotation_{};
    ShadowScreenParameters ui_projection_=initial_ui_projection();
    Texture image_{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
};

//133830's command2 branch and133a98 phase14; no world/event/player creation.
class DefeatFieldReturn {
public:
    DefeatFieldReturn(std::span<std::uint8_t>,std::array<std::uint32_t,3>&,
                      SceneFade&,SceneClearColor&,BattleUiShared&);
    void tick(const std::function<void(GameModeSelection)>&);
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&,SceneFade&);
    EmptySceneResource& resource() {return resource_;}
private:
    //Constructor's resource is built after bank/fade writes via initialize().
    static SceneClearColor& initialize(std::span<std::uint8_t>,
        const std::array<std::uint32_t,3>&,SceneFade&,SceneClearColor&);
    unsigned phase_=7,constructor_flag_=0;
    EmptySceneResource resource_;
};
}
