#include "okage/battle_ui_command_state.hpp"
#include "okage/ui_window.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON

namespace okage {
namespace {
void check(bool valid,const char* message) {
    if(!valid)throw std::runtime_error(message);
}
void require_float(std::uint32_t bits) {
    const auto exponent=bits&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),
          "Battle UI layer motion does not support nonfinite or subnormal floats");
}
class TowardZero {
public:
    TowardZero() {
        check(std::fegetenv(&saved_)==0,"Cannot save UI layer floating environment");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);
            throw std::runtime_error("Cannot set UI layer toward-zero rounding");
        }
    }
    ~TowardZero(){std::fesetenv(&saved_);}
    TowardZero(const TowardZero&)=delete;
    TowardZero& operator=(const TowardZero&)=delete;
private:
    std::fenv_t saved_{};
};
float target(std::uint32_t menu) {
    // Owned jump table247900, indexed by wrapping menu+100 when <=120.
    switch(__builtin_bit_cast(std::int32_t, (menu))) {
    case 0:case 1:case 16:return -31.0f;
    case 2:case 7:case 9:case 12:case 14:return -49.0f;
    case 6:case 11:return -79.0f;
    case -100:case 19:case 20:return -66.0f;
    default:return -17.0f;
    }
}
}
void BattleUiCommandState::import_layer(std::span<const std::uint32_t> layer,
        std::uint32_t& menu,std::int32_t& inhibit,UiWindow& window) {
    check(layer.size()>=0xac/4,"Battle UI command layer extent");
    window.set_layout_enabled(false);
    command=layer[0x64/4];menu=layer[0x68/4];
    for(unsigned i=0;i<8;++i) {
        changed[i]=layer[0x6c/4+i];values[i]=layer[0x8c/4+i];
        if(changed[i]!=0xffffffffu&&inhibit<2)inhibit=2;
    }
    if(command!=0xffffffffu)inhibit=60;
    if(inhibit>0) {
        --inhibit;
        window.set_layout_enabled(true);
    }
}
void BattleUiCommandState::move_layer_y(std::span<std::uint32_t> layer,
        std::uint32_t menu,UiWindow& window) {
    check(layer.size()>=0x2c/4,"Battle UI motion layer extent");
    require_float(layer[0x14/4]);
    TowardZero rounding;
    const float goal=target(menu);
    volatile float current=__builtin_bit_cast(float, (layer[0x14/4])),step=8.0f;
    check(std::feclearexcept(FE_OVERFLOW)==0,"Cannot clear UI layer overflow flag");
    const bool descending=goal<current;
    volatile float next=descending?current-step:current+step;
    check((std::fetestexcept(FE_OVERFLOW)&FE_OVERFLOW)==0,
          "Battle UI layer motion overflow is unsupported");
    require_float(__builtin_bit_cast(std::uint32_t, (float(next))));
    layer[0x14/4]=__builtin_bit_cast(std::uint32_t, (float(next)));
    if(descending?next<goal:goal<next)layer[0x14/4]=__builtin_bit_cast(std::uint32_t, (goal));
    window.set_transform_xyz({layer[4],layer[5],layer[6]},
                             {layer[8],layer[9],layer[10]});
}
}
