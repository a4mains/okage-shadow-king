#pragma once
#include "okage/game_over.hpp"

namespace okage {

struct TitleTaskState {
    GameOverTaskState common; //24,28,2c,30,3c,40,b8
    std::uint32_t yaw; //34
    std::uint32_t selection;
    std::array<std::uint32_t,7> x, y, alpha; //48..60,64..7c,80..98
    std::array<std::uint32_t,7> visible; //9c..b4
    std::uint32_t pulse, pulse_rising, pulse_channel; //bc,c0,c4
    std::uint32_t water, texture;
};

struct TitleScratch {
    std::uint32_t camera_w;
    std::array<std::uint32_t,3> water_transform_w; //position, rotation, scale
    std::array<std::uint32_t,3> water_rgb;
};
using TitleVector = std::array<std::uint32_t,4>;
struct TitleWaterCall {
    std::uint32_t definition;
    TitleVector position, rotation, scale;
};
struct TitleUpdateServices {
    GameOverUpdateServices common;
    std::function<void(unsigned,const TitleVector&)> set_camera;
    std::function<std::uint32_t()> model_channel_base; //1bb5b8(0)
    std::function<TitleVector(std::uint32_t)> read_model_channel;
    std::function<void(std::uint32_t,const TitleVector&)> set_model_channel;
    std::function<std::uint32_t(const TitleWaterCall&)> create_water;
    std::function<void(std::uint32_t,unsigned,std::uint32_t)> rebind_water;
    std::function<void(std::uint32_t,const TitleVector&)> set_water_color;
    std::function<void(std::uint32_t,unsigned)> set_water_clamp;
    std::function<void(std::uint32_t)> stop_water;
    std::function<void(unsigned)> play_sound;
    std::function<void()> reset_game; //124f10
    std::function<void()> reset_progress; //1251d8, after reset_game
    std::function<void(unsigned)> select_saved_field; //126788
    std::function<std::array<std::uint32_t,2>(unsigned)> read_saved_field; //1252a8
};

void title_update(TitleTaskState&, GameOverInputState&, GameOverPadSample,
                  std::uint32_t confirm_mask, const TitleScratch&,
                  const TitleUpdateServices&);
} // namespace okage
