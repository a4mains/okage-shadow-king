#include "okage/battle_results.hpp"
#include "okage/event.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <vector>

using namespace okage;
namespace {
void require(bool value,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("Battle results assertion line "+std::to_string(at.line()));
}
void put_half(std::span<std::uint8_t> b,unsigned at,std::uint16_t value) {
    b[at]=std::uint8_t(value);b[at+1]=std::uint8_t(value>>8);
}
void pair(std::span<std::uint8_t> b,unsigned index,std::int16_t id,std::int16_t count) {
    put_half(b,4+index*4,std::bit_cast<std::uint16_t>(id));
    put_half(b,6+index*4,std::bit_cast<std::uint16_t>(count));
}
void normal_transfer() {
    std::vector<std::uint8_t> variables(0x1000,0xa5);
    set_event_variable(variables,1,3,4);set_event_variable(variables,255,3,97);
    set_event_variable(variables,3,4,1000);
    std::array<std::uint8_t,0x50> reward{};
    put_half(reward,0,0xffff); // Not a transfer input.
    put_half(reward,2,30);pair(reward,0,1,3);pair(reward,7,255,4);
    for(unsigned i=0x24;i<reward.size();++i)reward[i]=0xbc;
    const auto original_reward=reward;
    auto expected=variables;expected[0x411]=7;expected[0x50f]=99;
    expected[0x51c]=6;expected[0x51d]=4;expected[0x51e]=expected[0x51f]=0; //1030.
    commit_battle_result_rewards(variables,reward);
    require(variables==expected&&reward==original_reward);
}
void sequential_signed_items() {
    std::vector<std::uint8_t> variables(0x520,0);
    std::array<std::uint8_t,0x24> reward{};
    set_event_variable(variables,7,3,98);
    pair(reward,0,7,10);pair(reward,1,7,-5); //99 then94, not combined99.
    pair(reward,2,8,-1);pair(reward,3,8,1); //Storedff must read as-1 next.
    pair(reward,4,9,std::numeric_limits<std::int16_t>::min());
    pair(reward,5,10,std::numeric_limits<std::int16_t>::max());
    pair(reward,6,0,std::numeric_limits<std::int16_t>::max());
    pair(reward,7,-1,100);
    variables[0x410]=23;variables[0x409]=31;
    const auto before=variables;
    commit_battle_result_rewards(variables,reward);
    auto expected=before;expected[0x417]=94;expected[0x41a]=99;
    require(variables==expected);
    reward={};pair(reward,0,11,-200);pair(reward,1,11,20);
    commit_battle_result_rewards(variables,reward);
    require(variables[0x41b]==76&&event_variable(variables,0x3000b)==76);
    // -200 stores56, then the next pair reads56 and adds20.
    reward={};pair(reward,0,12,-1);
    commit_battle_result_rewards(variables,reward);
    require(variables[0x41c]==255&&event_variable(variables,0x3000c)==-1);
}
void currency_word_semantics() {
    struct Case {std::int32_t before;std::uint16_t reward;std::int32_t after;};
    const Case cases[]={
        {999999998,1,999999999},{999999998,2,999999999},
        {std::numeric_limits<std::int32_t>::max(),1,std::numeric_limits<std::int32_t>::min()},
        {std::numeric_limits<std::int32_t>::max(),65535,-2147418114},
        {-10,65535,65525},{-100,0,-100},{1000000000,0,999999999},
        {std::numeric_limits<std::int32_t>::min(),65535,-2147418113}};
    for(const auto& c:cases) {
        std::vector<std::uint8_t> variables(0x600,0x6d);
        set_event_variable(variables,3,4,c.before);
        std::array<std::uint8_t,0x24> reward{};put_half(reward,2,c.reward);
        auto expected=variables;set_event_variable(expected,3,4,c.after);
        commit_battle_result_rewards(variables,reward);require(variables==expected);
    }
}
void malformed_inputs() {
    std::vector<std::uint8_t> variables(0x1000,0x15);
    std::array<std::uint8_t,0x24> reward{};pair(reward,0,1,5);pair(reward,7,256,1);
    const auto before=variables;
    const auto rejects=[&](std::span<std::uint8_t> vars,Bytes rewards) {
        bool rejected=false;try{commit_battle_result_rewards(vars,rewards);}
        catch(const std::runtime_error&){rejected=true;}require(rejected&&variables==before);
    };
    rejects(variables,reward);
    pair(reward,7,32767,1);rejects(variables,reward);
    pair(reward,7,255,1);rejects(variables,Bytes(reward).first(0x23));
    rejects(std::span(variables).first(0x51f),reward);
    reward={};pair(reward,0,std::numeric_limits<std::int16_t>::min(),5);
    commit_battle_result_rewards(variables,reward); //Negative ID is not an inventory access.
    require(variables==before);
}
}
int main() {
    try{normal_transfer();sequential_signed_items();currency_word_semantics();malformed_inputs();
        std::cout<<"Battle results reward transfer tests passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
