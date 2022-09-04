#include "okage/battle_ui_command_state.hpp"
#include "okage/ui_window.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F action) {
    try{action();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected explicit command-state rejection");
}
okage::UiWindow window() {
    return okage::UiWindow(1,1,0,std::make_shared<okage::UiWindowStream>(okage::UiWindowStream{1,{}}));
}
using Layer=std::array<std::uint32_t,0xc0/4>;
using Words=std::array<std::optional<std::uint32_t>,okage::UiWindow::scalar_bytes/4>;
Words snapshot(const okage::UiWindow& w) {
    Words result{};for(unsigned i=0;i<result.size();++i)result[i]=w.known_word(i*4);return result;
}
Layer empty_layer() {
    Layer layer{};std::fill(layer.begin()+0x64/4,layer.begin()+0xac/4,0xffffffffu);return layer;
}
void imports() {
    auto w=window();okage::BattleUiCommandState state;
    auto layer=empty_layer();std::uint32_t menu=123;std::int32_t inhibit=0;
    state.changed.fill(55);state.values.fill(77);state.command=19;
    w.set_layout_enabled(true);const auto flags=w.word(0x1900);
    const auto before=layer;
    state.import_layer(layer,menu,inhibit,w);
    require(layer==before&&menu==0xffffffffu&&state.command==0xffffffffu,"Imports leave layer intact and copy commands");
    require(inhibit==0&&w.word(0x1900)==(flags&~0x1000u),"Previous cached changes do not trigger inhibit");
    require(std::all_of(state.changed.begin(),state.changed.end(),[](auto x){return x==0xffffffffu;}),"All changed lanes overwritten");
    for(unsigned i=0;i<8;++i)layer[0x8c/4+i]=0x12345670+i;
    layer[0x6c/4+7]=0x80000000u;layer[0x68/4]=0xffffff9c;
    inhibit=std::numeric_limits<std::int32_t>::min();state.import_layer(layer,menu,inhibit,w);
    require(inhibit==1&&menu==0xffffff9c&&state.changed[7]==0x80000000u,"Negative imported change and signed negative inhibit");
    require(w.word(0x1900)==(flags|0x1000u)&&state.values[7]==0x12345677,"Last value copied and unrelated flags preserved");
    for(auto initial:{2,60,std::numeric_limits<std::int32_t>::max()}) {
        inhibit=initial;state.import_layer(layer,menu,inhibit,w);
        require(inhibit==initial-1,"Existing positive countdown is preserved then decremented");
    }
    layer[0x64/4]=0;inhibit=999;state.import_layer(layer,menu,inhibit,w);
    require(inhibit==59&&state.command==0,"Zero command is present and forces60 before decrement");
    layer[0x64/4]=0x80000000u;inhibit=-9;state.import_layer(layer,menu,inhibit,w);
    require(inhibit==59,"Every command other than -1 forces60");
    layer=empty_layer();inhibit=1;state.import_layer(layer,menu,inhibit,w);
    require(inhibit==0&&(w.word(0x1900)&0x1000u),"Positive predecrement enables layout even when final countdown is0");
    state.import_layer(layer,menu,inhibit,w);
    require(!(w.word(0x1900)&0x1000u),"Next zero-countdown call disables layout");
    inhibit=-4;state.import_layer(layer,menu,inhibit,w);require(inhibit==-4,"Unchanged negative countdown is not decremented");
    const auto saved=snapshot(w);menu=19;state.command=7;
    rejects([&]{state.import_layer(std::span(layer).first(0xa8/4),menu,inhibit,w);});
    require(menu==19&&state.command==7&&snapshot(w)==saved,"Short layer rejects before writes");
}
void movement() {
    auto w=window();Layer layer{};
    for(unsigned i=0;i<layer.size();++i)layer[i]=0x80000000u+i;
    const auto window_before=snapshot(w);
    const std::array<std::pair<std::int32_t,float>,18> targets{{
        {0,-31},{1,-31},{16,-31},{2,-49},{7,-49},{9,-49},{12,-49},{14,-49},
        {6,-79},{11,-79},{-100,-66},{19,-66},{20,-66},{-101,-17},{21,-17},
        {-1,-17},{std::numeric_limits<std::int32_t>::min(),-17},{std::numeric_limits<std::int32_t>::max(),-17}}};
    for(const auto [menu,target]:targets) {
        for(const auto offset:{-20.0f,-8.0f,-3.5f,0.0f,3.5f,8.0f,20.0f}) {
            layer[5]=__builtin_bit_cast(std::uint32_t, (target+offset));const auto before=layer;
            okage::BattleUiCommandState::move_layer_y(layer,__builtin_bit_cast(std::uint32_t, (menu)),w);
            const float expected=target+(offset>8?offset-8:offset< -8?offset+8:0);
            require(layer[5]==__builtin_bit_cast(std::uint32_t, (expected)),"Menu target, movement direction and strict overshoot clamp");
            for(unsigned i=0;i<layer.size();++i)if(i!=5)require(layer[i]==before[i],"Only layer Y changes");
            for(auto base:{0x2f0u,0x520u})for(unsigned i=0;i<3;++i)require(w.word(base+4*i)==layer[4+i],"First XYZ copies raw bits");
            for(auto base:{0x3f0u,0x550u})for(unsigned i=0;i<3;++i)require(w.word(base+4*i)==layer[8+i],"Second XYZ copies raw bits");
        }
    }
    const auto after=snapshot(w);
    for(unsigned i=0;i<after.size();++i) {
        const auto at=i*4;
        const bool written=at==0x4f4||(at>=0x2f0&&at<0x2fc)||(at>=0x520&&at<0x52c)||
            (at>=0x3f0&&at<0x3fc)||(at>=0x550&&at<0x55c);
        if(!written)require(after[i]==window_before[i],"Window W/history/unrelated known lanes unchanged");
    }
    layer[5]=__builtin_bit_cast(std::uint32_t, (-79.0f));
    okage::BattleUiCommandState::move_layer_y(layer,0,w);
    require(layer[5]==__builtin_bit_cast(std::uint32_t, (-71.0f)),"Reached first move from -79 to -71");
    for(auto bits:{0u,0x80000000u}) {
        layer[5]=bits;okage::BattleUiCommandState::move_layer_y(layer,0,w);
        require(layer[5]==__builtin_bit_cast(std::uint32_t, (-8.0f)),"Signed zero starts one downward step");
    }
}
void rounding_and_rejection() {
    std::fenv_t original;std::fegetenv(&original);
    auto w=window();Layer layer{};
    for(auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        std::fesetround(mode);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO|FE_OVERFLOW);
        const auto raised=std::fetestexcept(FE_ALL_EXCEPT);
        layer[5]=0x4d800000u;okage::BattleUiCommandState::move_layer_y(layer,0,w);
        require(layer[5]==0x4d7fffffu,"Inexact subtract uses toward-zero independently of caller mode");
        require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==raised,"Complete caller float environment restored");
        for(auto bits:{0x7f800000u,0xff800000u,0x7fc00001u,1u,0x80000001u}) {
            layer[5]=bits;const auto before=layer;const auto saved=snapshot(w);
            rejects([&]{okage::BattleUiCommandState::move_layer_y(layer,0,w);});
            require(layer==before&&snapshot(w)==saved,"Unsupported float rejects before layer/window mutation");
            require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==raised,"Failure preserves caller environment");
        }
    }
    std::fesetenv(&original);
    rejects([&]{okage::BattleUiCommandState::move_layer_y(std::span(layer).first(10),0,w);});
}
void parameter() {
    auto w=window();auto expected=snapshot(w);
    for(auto value:{16u,0u,0xffffffffu,0x80000000u}) {
        w.set_transform_parameter(value);expected[0x4f8/4]=value;
        require(snapshot(w)==expected,"1a56d0 copies entire word and changes no other scalar/knownness");
    }
}
}
int main(){try {
    imports();movement();rounding_and_rejection();parameter();
    std::cout<<"Battle UI command-state source-contract tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
