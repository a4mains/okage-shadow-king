#pragma once
#include "okage/name_entry.hpp"
#include "okage/scene_resource.hpp"
#include <filesystem>
namespace okage {
struct NameEntryConstruction {
    std::filesystem::path common_root,ui_root;
    std::shared_ptr<const std::vector<Font>> fonts;
    std::function<void()> stop_music;
    std::shared_ptr<const std::vector<std::uint8_t>> atlas_bytes;
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
    // Newgame per-traversal observation of the last tick (research reports; no behaviour depends on it).
    struct FrameObservation {
        bool manager_ran=false,died=false,unlinked=false,finished=false;
        std::int32_t phase_in=0,phase_out=0,length_out=0,selection_out=0,yes_out=0;
        std::uint16_t pressed=0,repeat=0;
        std::array<std::uint8_t,16> name{}; // variables 0x844.. after the callbacks
    };
    const FrameObservation& last_frame() const;
    int ui_window() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
