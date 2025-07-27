#pragma once
#include "okage/scene_resource.hpp"
#include "okage/battle_ui_pad.hpp"
#include <filesystem>
namespace okage {
struct RestoreShared {
    int state=0,selected=0,repeat_delay=0;
    std::array<int,3> alpha{};
    float previous_opacity=0;
};
struct RestoreConstruction {
    std::filesystem::path common_root,ui_root,save_directory;
    std::shared_ptr<const std::vector<Font>> fonts;
    std::function<void()> stop_music;
    bool save=false;
    std::shared_ptr<RuwResource> field_ruw;
    std::shared_ptr<const Texture> field_atlas;
    std::shared_ptr<const std::vector<std::uint8_t>> atlas_bytes;
    unsigned storage_latency_ms=0;
};
class RestoreSession {
public:
    RestoreSession(SceneClearColor&,SceneFade&,BattleUiShared&,RestoreShared&,RestoreConstruction);
    ~RestoreSession();
    void tick(BattleUiPadInput,std::span<std::uint8_t>,const GameDataTables&,
        SceneFade&,Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&,
        unsigned& image_counter,const SineTable&,GameRandom&,
        const std::function<void(unsigned)>& sound,const std::function<void(bool)>& finish);
    void tick_save(BattleUiPadInput,std::span<std::uint8_t>,const GameDataTables&,
        SceneFade&,const Texture& captured_background,const SineTable&,GameRandom&,
        const std::function<void(unsigned)>& sound,const std::function<void(bool)>& publish_result);
    bool dead() const;
    // Any saved slot was found (saved-slot display; not part of the empty-card Restore route).
    bool saved_slots_present() const;
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&);
    bool resource_live() const;
    const Texture* rendered_image() const;
    std::vector<std::uint32_t> observation() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
