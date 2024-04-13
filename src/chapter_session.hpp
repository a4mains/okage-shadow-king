#pragma once
#include "okage/chapter.hpp"
#include "okage/scene_resource.hpp"
#include <memory>

namespace okage {
class ChapterSession {
public:
    ChapterSession(std::span<std::uint8_t> variables,const std::array<std::uint32_t,3>& command,
        GameModeSelection destination,SceneClearColor&,SceneFade&,BattleUiShared&,
        const std::function<std::vector<std::uint8_t>(unsigned)>& load_image);
    ~ChapterSession();
    void tick(std::array<std::uint32_t,3>& command,SceneFade&,Animation&,FieldProjection&,
        RetiringSceneEffects*,BattleUiShared&,std::shared_ptr<const std::vector<Font>>,
        const SineTable&,GameRandom&,std::uint32_t& image_frame_counter,
        const std::function<void(GameModeSelection)>&);
    void retire(Animation&,FieldProjection&,RetiringSceneEffects*,BattleUiShared&,SceneFade&);
    bool resource_live() const;
    const Texture* rendered_image() const;
    const ChapterState& state() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
