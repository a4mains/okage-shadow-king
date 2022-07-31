#include "okage/battle_ui_pad.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected pad input rejection");
}
void analog_repeat() {
    okage::BattleUiPad pad;okage::BattleUiPadInput in;
    auto out=pad.normalize(in);
    require(!out.repeat&&!out.pressed&&!out.released&&!pad.analog_x_ticks&&!pad.analog_y_ticks,"Neutral input");
    in.left_x=63;in.left_y=0;
    for(unsigned tick=0;tick<34;++tick) {
        out=pad.normalize(in);
        require(out.repeat==((tick==0||tick==24||tick==32)?0x8000u:0u),"Analog first/strict delay/eight-call repeat");
        require(pad.analog_x_ticks==tick+1&&!pad.analog_y_ticks,"Horizontal priority suppresses vertical counter");
    }
    in.left_x=193;
    require(pad.normalize(in).repeat==0&&pad.analog_x_ticks==35,"Direction reversal keeps axis repeat history");
    pad.analog_x_ticks=24;pad.analog_y_ticks=17;
    in.held=0x1000;in.repeat=0x2000;
    require(pad.normalize(in).repeat==0x2000&&pad.analog_x_ticks==24&&pad.analog_y_ticks==17,
            "Held digital direction bypasses analog without clearing counters");
    in.held=0;in.repeat=0;in.left_x=192;in.left_y=95;
    require(pad.normalize(in).repeat==0&&!pad.analog_x_ticks&&pad.analog_y_ticks==18,"Inclusive horizontal neutral upper threshold");
    in.left_x=64;in.left_y=96;
    pad.normalize(in);require(!pad.analog_x_ticks&&!pad.analog_y_ticks,"Inclusive lower neutral thresholds");
    in.left_y=160;pad.normalize(in);require(!pad.analog_y_ticks,"Inclusive upper Y neutral threshold");
    in.left_y=161;require(pad.normalize(in).repeat==0x4000&&pad.analog_y_ticks==1,"Y down threshold");
    in.left_y=0;require(pad.normalize(in).repeat==0&&pad.analog_y_ticks==2,"Vertical reversal retains counter");
    pad.analog_x_ticks=0xffffffff;pad.analog_y_ticks=9;in.left_x=0;
    require(!pad.normalize(in).repeat&&!pad.analog_x_ticks&&pad.analog_y_ticks==9,"Signed negative counter wraps without a pulse");
    require(pad.normalize(in).repeat==0x8000&&pad.analog_x_ticks==1,"Wrapped zero counter pulses");
    pad.analog_x_ticks=0x80000000;
    require(!pad.normalize(in).repeat&&pad.analog_x_ticks==0x80000001,"Counter comparison is signed");
}
void mask_priority() {
    for(const auto [input,expected]:{
        std::pair<std::uint32_t,std::uint32_t>{0xf000,0x6000},{0xf040,0x40},
        {0xf020,0x20},{0x60,0x20},{0x48,0x8},{0x44,0x4},{0xc,0x4},
        {0x1044,0x4},{0x206c,0x24},{0x90008001,0x90008001}}) {
        auto actual=input;const bool changed=okage::BattleUiPad::normalize_directions(actual);
        require(actual==expected&&changed==(input!=expected),"Ordered direction/action conflict priority");
    }
    constexpr std::uint32_t relevant=0xf06c;
    for(std::uint32_t value=0;value<65536;++value) {
        auto first=value;okage::BattleUiPad::normalize_directions(first);
        require((first&~value)==0&&(first&~relevant)==(value&~relevant),"Unrelated input bits preserved");
        auto twice=first;
        require(!okage::BattleUiPad::normalize_directions(twice)&&twice==first,"Normalization idempotent");
    }
    okage::BattleUiPad pad;okage::BattleUiPadInput in;
    in.repeat=0xa000;in.pressed=0x5000;in.released=0x1044;in.held=0x1000;
    const auto out=pad.normalize(in);
    require(out.repeat==0x2000&&out.pressed==0x4000&&out.released==4,"All three actual latches normalized separately");
    require(in.repeat==0xa000&&in.pressed==0x5000&&in.released==0x1044,"Caller latches not mutated");
    pad.controls[0].enabled=0;
    require(pad.normalize(in).repeat==0x2000,"Enable setting is not an invented digital input gate");
}
void control_setters() {
    okage::BattleUiPad pad;
    for(const auto& p:pad.controls)require(p.enabled==1&&!p.refresh_pending&&p.delay==2400&&p.interval==480,"Source startup state");
    okage::BattleUiPad pal(50);require(pal.controls[0].delay==2000&&pal.controls[1].interval==400,"PAL startup units");
    rejects([]{okage::BattleUiPad invalid(59);});
    auto& p=pad.controls[0];p.bytes.fill(0xa5);p.halfwords.fill(0xabcd);
    require(pad.set_enabled(-5,true)==1&&p.bytes[3]==0xa5&&!p.refresh_pending,"Unchanged enable does nothing");
    require(pad.set_enabled(std::numeric_limits<int>::min(),false)==1&&!p.enabled&&p.refresh_pending==1,"Disable clamps low index and returns old value");
    for(auto x:p.bytes)require(!x,"Disable clears all six bytes");
    for(auto x:p.halfwords)require(!x,"Disable clears all six halfwords");
    require(p.delay==2400&&p.interval==480&&pad.controls[1].enabled==1,"Disable preserves timing and other pad");
    p.bytes[0]=7;p.halfwords[5]=9;p.refresh_pending=0;
    require(pad.set_enabled(0,false)==0&&p.bytes[0]==7&&p.halfwords[5]==9&&!p.refresh_pending,"Repeated disable does not repeat clears");
    require(pad.set_enabled(0,true)==0&&p.bytes[0]==7&&p.halfwords[5]==9&&!p.refresh_pending,"Enable preserves control arrays");
    p.enabled=2;require(pad.set_enabled(0,true)==2&&p.enabled==1,"Original old byte is returned without Boolean normalization");
    require(pad.set_enabled(std::numeric_limits<int>::max(),false)==1&&!pad.controls[1].enabled&&p.enabled==1,"Enable index clamps high");
    pad.analog_x_ticks=9;pad.analog_y_ticks=17;
    pad.set_repeat(0,30,15,60);require(p.delay==1800&&p.interval==900,"Battle NTSC raw timing units");
    pad.set_repeat(1,30,15,50);require(pad.controls[1].delay==1500&&pad.controls[1].interval==750,"Battle PAL raw timing units");
    pad.set_repeat(0,0xffffffff,0x80000100,60);
    require(p.delay==65476&&p.interval==15360,"Repeat products wrap to low sixteen bits");
    const auto delay=p.delay,interval=p.interval;
    rejects([&]{pad.set_repeat(2,0,0,60);});rejects([&]{pad.set_repeat(0,0,0,0);});
    require(p.delay==delay&&p.interval==interval&&pad.analog_x_ticks==9&&pad.analog_y_ticks==17,"Rejected timing leaves state and analog counters untouched");
}
}
int main(){try {
    analog_repeat();mask_priority();control_setters();
    std::cout<<"Battle UI pad prefix/control tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
