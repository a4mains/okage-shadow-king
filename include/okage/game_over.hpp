#pragma once

#include "okage/game_mode.hpp"
#include <array>
#include <cstdint>
#include <functional>

namespace okage {

// Explicit samples returned by the source pad accessors 1c01d0(+0xa) and
// 1c0160(+0x6), plus the four bytes copied by 1c0330. These are raw values;
// this API assigns no human-facing button names.
struct GameOverPadSample {
    std::uint16_t repeat_word;
    std::uint16_t pressed_word;
    std::array<std::uint8_t, 4> analog_bytes;
};

struct GameOverInputState {
    std::uint32_t repeat_ticks;
    std::array<std::uint32_t, 4> key_masks;
    std::array<std::uint32_t, 4> direction_flags;
    std::array<std::int32_t, 4> analog_words;
};

struct GameOverTaskState {
    std::uint32_t submode;
    std::uint32_t mode_state;
    std::uint32_t tick_counter;
    std::uint32_t pressed_word;
    std::uint32_t generic_alpha_bits;
    std::uint32_t generic_alpha_rate_bits;
    std::uint32_t image_alpha_bits;
};

// The exact a0 and F12..F17 words delivered by mode4's tail-call to 1b3710.
struct GameOverImageCall {
    std::uint32_t a0;
    std::array<std::uint32_t, 6> fp_words;
};

struct GameOverUpdateServices {
    std::function<void(float)> fade;
    std::function<void(GameModeSelection)> request_mode;
    std::function<void(const GameOverImageCall&)> submit_image;
};

void mode8_update_prefix(GameOverTaskState& task, GameOverInputState& input,
                         GameOverPadSample sample,
                         const GameOverUpdateServices& services);

void game_over_submode4_update(GameOverTaskState& task,
                               GameOverInputState& input,
                               GameOverPadSample sample,
                               const GameOverUpdateServices& services);

} // namespace okage
