#include "okage/battle_ui_pad.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::uint32_t up=0x1000,right=0x2000,down=0x4000,left=0x8000;
constexpr std::uint32_t directions=up|right|down|left;
void analog(std::uint32_t& mask,std::uint32_t& ticks,std::uint32_t direction) {
    if((ticks==0||__builtin_bit_cast(std::int32_t, (ticks))>20)&&(ticks&7)==0)mask|=direction;
    ++ticks;
}
}
BattleUiPad::BattleUiPad(unsigned rate) {
    //1bf290 startup settings, distinct from battle's later30/15 request.
    set_repeat(0,40,8,rate);set_repeat(1,40,8,rate);
}
BattleUiPadMasks BattleUiPad::normalize(BattleUiPadInput in) {
    BattleUiPadMasks out{in.repeat,in.pressed,in.released};
    if(!(in.held&directions)) {
        if(in.left_x<64)analog(out.repeat,analog_x_ticks,left);
        else if(in.left_x>192)analog(out.repeat,analog_x_ticks,right);
        else {
            analog_x_ticks=0;
            if(in.left_y<96)analog(out.repeat,analog_y_ticks,up);
            else if(in.left_y>160)analog(out.repeat,analog_y_ticks,down);
            else analog_y_ticks=0;
        }
    }
    normalize_directions(out.repeat);
    normalize_directions(out.pressed);
    normalize_directions(out.released);
    return out;
}
bool BattleUiPad::normalize_directions(std::uint32_t& mask) {
    bool changed=false;
    const auto remove_if=[&](std::uint32_t removed,std::uint32_t preferred) {
        if((mask&removed)&&(mask&preferred)){mask&=~removed;changed=true;}
    };
    //Ordered123820 conflicts; later conditions see the already-masked word.
    remove_if(left,right);remove_if(up,down);
    remove_if(up,0x40);remove_if(down,0x40);remove_if(right,0x40);remove_if(left,0x40);
    remove_if(up,0x20);remove_if(down,0x20);remove_if(right,0x20);remove_if(left,0x20);
    remove_if(0x40,0x20);remove_if(0x40,0x8);remove_if(0x40,0x4);remove_if(0x8,0x4);
    return changed;
}
std::uint8_t BattleUiPad::set_enabled(int index,bool enabled) {
    auto& state=controls[std::clamp(index,0,1)];
    const auto previous=state.enabled;
    if(previous==std::uint8_t(enabled))return previous;
    state.enabled=std::uint8_t(enabled);
    if(!enabled) {
        state.bytes.fill(0);state.halfwords.fill(0);state.refresh_pending=1;
    }
    return previous;
}
void BattleUiPad::set_repeat(unsigned index,std::uint32_t delay,std::uint32_t interval,unsigned rate) {
    if(index>=controls.size())throw std::runtime_error("Battle UI pad index outside owned state");
    if(rate!=50&&rate!=60)throw std::runtime_error("Battle UI pad display rate must be50 or60");
    controls[index].delay=std::uint16_t(delay*rate);
    controls[index].interval=std::uint16_t(interval*rate);
}
}
