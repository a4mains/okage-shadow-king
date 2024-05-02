#pragma once
#include "okage/game_over.hpp"
namespace okage {
struct StartupTaskState {
    GameOverTaskState common;
    std::uint32_t image_index;
};
struct StartupUpdateServices {
    GameOverUpdateServices common;
    std::function<void()> initialize_shared;
};
void startup_logo_update(StartupTaskState&,GameOverInputState&,GameOverPadSample,
                         const StartupUpdateServices&);
}
