#include "okage/battle_victory.hpp"
#include "okage/battle_command_queue.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
unsigned word(const BattleState& r,unsigned at) {
    unsigned v=0;for(unsigned i=0;i<4;++i)v|=unsigned(r.bytes[at+i])<<(8*i);return v;
}
void put(BattleState& r,unsigned at,unsigned value) {
    for(unsigned i=0;i<4;++i)r.bytes[at+i]=std::uint8_t(value>>(8*i));
}
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
float decrement(BattleState& r) {
    const float before=std::bit_cast<float>(word(r,0x81bc)),step=std::bit_cast<float>(word(r,0x2c));
    const auto supported=[](float f){return std::isfinite(f)&&std::fpclassify(f)!=FP_SUBNORMAL;};
    require(supported(before)&&supported(step),"Victory timer unsupported float input");
    std::fenv_t saved;
    require(!std::fegetenv(&saved)&&!std::fesetround(FE_TOWARDZERO),"Victory timer rounding unavailable");
    const float result=before-step;
    std::fesetenv(&saved);
    require(supported(result),"Victory timer unsupported float result");
    put(r,0x81bc,std::bit_cast<unsigned>(result));return result;
}
}
void battle_victory_update(BattleState& r,std::span<const BattleEntityBinding> entities,
                           SceneFade& fade,std::span<std::uint8_t> reward,
                           const BattleVictoryServices& services) {
    const auto phase=std::bit_cast<std::int32_t>(word(r,0x24));
    if(phase==0) {
        put(r,0x81bc,0x43960000); //300;151098 is an empty function.
        put(r,0x24,word(r,0x24)+1);
    } else if(phase==2) {
        if(!(decrement(r)<0))return;
        fade.step=std::bit_cast<float>(0x3ca3d70au);fade.request(0); //149f98
        if(word(r,0x8080)==1) { //14ace0/14ac78
            require(reward.size()>=4,"Victory reward owner extent");
            const auto doubled=(unsigned(reward[2])|(unsigned(reward[3])<<8))*2;
            const auto capped=std::min(doubled,60000u);
            reward[2]=std::uint8_t(capped);reward[3]=std::uint8_t(capped>>8);
        }
        put(r,0x24,word(r,0x24)+1);return;
    } else if(phase==3) {
        if(fade.current==fade.target)put(r,0x1c,word(r,0x1c)|0x80000000u);
        return;
    } else if(phase!=1)return;
    if(!(decrement(r)<0))return;
    put(r,0x81bc,0x43960000);put(r,0x24,word(r,0x24)+1);put(r,0x81b8,2);
    require(bool(services.random),"Victory shared RNG missing");
    unsigned selected=40;
    if((std::bit_cast<std::int32_t>(services.random())>>5)&1) {
        require(entities.size()>=4,"Victory party camera owners missing");
        std::array<unsigned,4> choices;
        for(unsigned i=0;i<4;++i)choices[i]=battle_actor_incapacitated(entities[i].actor)?40:i+43;
        selected=choices[(std::bit_cast<std::int32_t>(services.random())>>5)&3];
    }
    require(bool(services.camera),"Victory camera1510a0 missing");services.camera(selected);
    require(bool(services.stop_music),"Victory music stop missing");services.stop_music();
    require(bool(services.jingle),"Victory jingle service missing");services.jingle(4);
    require(bool(services.fade_ui),"Victory UI fade missing");services.fade_ui(1,-128);
}
void battle_defeat_update(BattleState& r,SceneFade& fade,
                          const std::function<void(unsigned,int)>& fade_ui) {
    const auto phase=std::bit_cast<std::int32_t>(word(r,0x24));
    if(phase==0) {
        // 0x14aa28 is the JAL delay slot, so the timer store precedes entry to
        // 0x11fdf8. Preserve that partial-write behavior if the UI service fails.
        put(r,0x81bc,0x43340000); //180
        require(bool(fade_ui),"Defeat UI fade missing");
        fade_ui(1,-128);
        put(r,0x24,word(r,0x24)+1);
    } else if(phase==1) {
        if(!(decrement(r)<0))return;
        fade.step=std::bit_cast<float>(0x3ba3d70au); //149f98 -> 114c90
        fade.request(0.f); //149f98 -> 114cc0
        put(r,0x24,word(r,0x24)+1);
        return;
    } else if(phase==2) {
        if(fade.current!=fade.target)return; //114d20 raw source equality
        put(r,0x1c,word(r,0x1c)|0x80000000u);
        put(r,0x24,word(r,0x24)+1);
        return;
    } else if(phase!=0) {
        return;
    }

    // Phase zero falls through into the same subtract/strict-negative test.
    if(!(decrement(r)<0))return;
    fade.step=std::bit_cast<float>(0x3ba3d70au);
    fade.request(0.f);
    put(r,0x24,word(r,0x24)+1);
}
}
