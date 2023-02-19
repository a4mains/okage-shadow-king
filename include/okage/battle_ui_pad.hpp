#pragma once
#include <array>
#include <cstdint>

namespace okage {
struct BattleUiPadInput {
    std::uint16_t held=0,pressed=0,released=0,repeat=0;
    std::uint8_t left_x=128,left_y=128; // Native neutral when no analog device.
};
struct BattleUiPadMasks {std::uint32_t repeat=0,pressed=0,released=0;};
struct BattleUiPadControl {
    // 33959e/33959d and the six bytes/halfwords at3395a6/3395ac,
    // with0x58 stride per pad. These do not gate normalized digital input.
    std::uint8_t enabled=1,refresh_pending=0;
    std::array<std::uint8_t,6> bytes{};
    std::array<std::uint16_t,6> halfwords{};
    std::uint16_t delay=0,interval=0; //339596/339598, raw repeat units.
};
class BattleUiPad {
public:
    static constexpr std::uint32_t selection_buttons=0x10|0x20|0x80|8|2|4|1;
    explicit BattleUiPad(unsigned display_rate=60);
    BattleUiPadMasks normalize(BattleUiPadInput input);
    //123820 returns whether it removed any source conflict bits.
    static bool normalize_directions(std::uint32_t& mask);
    //1c0500 clamps signed index to0..1, returns the previous enable byte.
    std::uint8_t set_enabled(int index,bool enabled);
    void set_repeat(unsigned index,std::uint32_t delay,std::uint32_t interval,unsigned display_rate);

    std::uint32_t analog_x_ticks=0,analog_y_ticks=0; //1f9c98/1f9c9c.
    std::array<BattleUiPadControl,2> controls{};
};
}
