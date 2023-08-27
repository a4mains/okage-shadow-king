#include "okage/game_over.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
constexpr std::uint32_t exit_mask = 0x40; // 001002f8 initializes DAT_1f85dc.
constexpr std::uint32_t float_one = 0x3f800000;
constexpr std::uint32_t image_fade_in_step = 0x3b30f27c;
constexpr std::uint32_t image_fade_out_step = 0x3c23d70a;

class TowardZero {
public:
    TowardZero() {
        if (std::fegetenv(&saved_) != 0)
            throw std::runtime_error("Cannot save game-over floating environment");
        if (std::fesetround(FE_TOWARDZERO) != 0) {
            std::fesetenv(&saved_);
            throw std::runtime_error("Cannot set game-over toward-zero rounding");
        }
    }
    TowardZero(const TowardZero&) = delete;
    TowardZero& operator=(const TowardZero&) = delete;
    ~TowardZero() { std::fesetenv(&saved_); }
private:
    std::fenv_t saved_{};
};

float as_float(std::uint32_t bits) { return std::bit_cast<float>(bits); }
std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

float add_single(std::uint32_t lhs, std::uint32_t rhs) {
    volatile float a = as_float(lhs);
    volatile float b = as_float(rhs);
    volatile float result = a + b;
    return result;
}

float subtract_single(std::uint32_t lhs, std::uint32_t rhs) {
    volatile float a = as_float(lhs);
    volatile float b = as_float(rhs);
    volatile float result = a - b;
    return result;
}

void update_generic_prefix(GameOverTaskState& task, GameOverInputState& input,
                           const GameOverPadSample& sample,
                           const GameOverUpdateServices& services) {
    input.direction_flags.fill(0); // 12bfc8 clears 259964..259970.
    const auto centered_x = static_cast<std::int32_t>(sample.analog_bytes[0]) - 0x80;
    const auto centered_y = static_cast<std::int32_t>(sample.analog_bytes[1]) - 0x80;
    input.analog_words = {centered_x, centered_y,
                          sample.analog_bytes[2], sample.analog_bytes[3]};

    if (input.repeat_ticks == 0) {
        // Strict source thresholds, in source flag order.
        if (centered_y < -100) input.direction_flags[0] = 1;
        if (centered_x > 100) input.direction_flags[1] = 1;
        if (centered_y > 100) input.direction_flags[2] = 1;
        if (centered_x < -100) input.direction_flags[3] = 1;

        constexpr std::array<std::uint32_t, 4> expected{0x1000, 0x2000, 0x4000, 0x8000};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if ((std::uint32_t(sample.repeat_word) & input.key_masks[i]) == expected[i])
                input.direction_flags[i] = 1;
        }
        if (input.direction_flags[0] == 1 || input.direction_flags[1] == 1 ||
            input.direction_flags[2] == 1 || input.direction_flags[3] == 1)
            input.repeat_ticks = 8;
    } else {
        --input.repeat_ticks;
        // Source's unsigned test is raw analog byte0 in [0x1d, 0xe3].
        const auto x = std::uint32_t(sample.analog_bytes[0]);
        if (x - 0x1du < 0xc7u) {
            if (centered_y >= -99 && centered_y < 100)
                input.repeat_ticks = 0;
        }
    }

    auto alpha = add_single(task.generic_alpha_bits, task.generic_alpha_rate_bits);
    if (alpha < 0.0f) alpha = 0.0f;
    task.generic_alpha_bits = bits(alpha);
    if (1.0f < as_float(task.generic_alpha_bits))
        task.generic_alpha_bits = float_one;

    // 12c1c8 happens before 1c0160 and before dispatch.
    services.fade(as_float(task.generic_alpha_bits));
    task.pressed_word = sample.pressed_word;
}

void update_mode4(GameOverTaskState& task, const GameOverUpdateServices& services) {
    ++task.tick_counter; // MIPS addiu wraparound.
    if (task.mode_state == 0) {
        if (task.tick_counter > 0x3c) {
            task.mode_state = 10;
            task.tick_counter = 0;
        }
    } else if (task.mode_state == 10) {
        if (as_float(task.image_alpha_bits) < 1.0f)
            task.image_alpha_bits = bits(add_single(task.image_alpha_bits, image_fade_in_step));
        if (task.tick_counter > 0x2d0) task.mode_state = 0x14;
        if (task.tick_counter > 0x78 && (task.pressed_word & exit_mask) != 0)
            task.mode_state = 0x14;
    } else if (task.mode_state == 0x14) {
        if (as_float(task.image_alpha_bits) > 0.0f) {
            task.image_alpha_bits = bits(subtract_single(task.image_alpha_bits, image_fade_out_step));
        } else {
            services.request_mode({8, 1, 0});
        }
    }

    // Source's delay-slot-filled tail call: update alpha, then submit image.
    services.fade(as_float(task.image_alpha_bits));
    static constexpr GameOverImageCall call{
        0, {0, 0, 0x3f800000, 0x3f800000, 0, 0x3f800000}};
    services.submit_image(call);
}
} // namespace

void game_over_submode4_update(GameOverTaskState& task,
                               GameOverInputState& input,
                               GameOverPadSample sample,
                               const GameOverUpdateServices& services) {
    if (task.submode != 4) throw std::runtime_error("game-over helper requires submode4");
    TowardZero rounding;
    update_generic_prefix(task, input, sample, services);
    update_mode4(task, services);
}
} // namespace okage
