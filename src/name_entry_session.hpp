#pragma once
#include "okage/name_entry.hpp"
#include "okage/scene_resource.hpp"
#include <filesystem>
namespace okage {
struct NameEntryConstruction {
    std::filesystem::path common_root,ui_root;
    std::shared_ptr<const std::vector<Font>> fonts;
    std::function<void()> stop_music;
};
class NameEntrySession {
public:
    NameEntrySession(SceneClearColor&,SceneFade&,BattleUiShared&,NameEntryShared&,NameEntryConstruction);
    ~NameEntrySession();
    void tick(BattleUiPad&,BattleUiPadInput,std::span<std::uint8_t>,const GameDataTables&,
        SceneFade&,Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&,
        unsigned& image_frame_counter,const SineTable&,GameRandom&,
        const std::function<void(unsigned)>& sound,const std::function<void()>& finish);
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&);
    bool resource_live() const;
    const Texture* rendered_image() const;
    std::vector<std::uint32_t> observation() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
