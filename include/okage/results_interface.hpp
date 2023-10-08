#pragma once
#include "okage/results_animation.hpp"
#include "okage/results_progress.hpp"
#include "okage/results_text.hpp"
#include "okage/ui_resource.hpp"

namespace okage {
struct ResultsUiShared {
    std::shared_ptr<UiWindowStream> stream=std::make_shared<UiWindowStream>(UiWindowStream{1,{}});
    std::vector<UiWindow::AtlasTuple> atlas{UiWindow::AtlasTuple{}};
    ResultsAnimation animation;
    ResultsProgress progress;
    std::uint32_t loader_ready=0,skip=0,item_count=0;
    std::uint32_t reward_display=0,money_display=0,money_before=0;
    std::uint32_t command=0;
    std::array<std::uint32_t,8> arguments{};
};
struct ResultsUiContext {
    std::span<std::uint8_t> variables,reward;
    const GameDataTables& tables;
    UiPartySelection& party;
    const BattleHudData& numbers;
    const ResultsTextData& text;
    std::function<void(unsigned)> play_jingle;
    unsigned game_mode=2;
};
class ResultsInterface {
public:
    ResultsInterface(UiResourceCoordinator::Registry&,UiResourceCoordinator&,
        std::array<std::uint32_t,16>& resource_accumulators,ResultsUiShared&,
        const ResultsUiData&,std::shared_ptr<RuwResource>,std::shared_ptr<const void> texture,
        std::uint32_t parent_flags,ResultsUiContext);
    ~ResultsInterface();
    ResultsInterface(const ResultsInterface&)=delete;
    ResultsInterface& operator=(const ResultsInterface&)=delete;
    const auto& words() const {return words_;}
    unsigned handle() const;
    const UiWindow& window() const;
    bool dead() const {return (words_[0x1c/4]&0x80000000u)!=0;}
    void first_update(std::uint32_t pressed,ResultsUiContext);
    void second_update(ResultsUiContext);
private:
    void initialize(ResultsUiContext);
    void currency(bool quick,ResultsUiContext);
    void party_alpha(bool quick,ResultsUiContext);
    void notice_motion();
    void release();
    UiWindow& mutable_window();
    UiResourceCoordinator::Registry& registry_;
    std::array<std::uint32_t,16>& accumulators_;
    ResultsUiShared& shared_;
    const ResultsUiData& data_;
    std::shared_ptr<RuwResource> resource_;
    int handle_=-1;
    std::array<std::uint32_t,0xef0/4> words_{};
};
}
