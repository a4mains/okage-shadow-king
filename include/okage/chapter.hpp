#pragma once
#include "okage/game_over.hpp"

namespace okage {
struct ChapterState {
    ChapterState(std::uint32_t chapter, std::uint32_t scene, std::uint32_t entrance)
        : chapter_argument(chapter), destination_scene(scene), destination_entrance(entrance) {}
    std::uint32_t flag=1; // manager+20
    std::uint32_t chapter_argument; //+24
    std::uint32_t destination_scene, destination_entrance; //+28,+2c
    std::uint32_t counter=0;
    std::uint32_t phase=19; //+48
};
struct ChapterUpdateServices {
    std::function<void(const GameOverImageCall&)> submit_image; //1b3710
    std::function<void(float)> request_fade; //114cc0, target only
    std::function<bool()> fade_pending; //114d20: current!=target
    std::function<void()> clear_command; //1248d8(0,0,0)
    std::function<void(GameModeSelection)> request_mode; //124930
};
void chapter_update(ChapterState&, const ChapterUpdateServices&);
} // namespace okage
