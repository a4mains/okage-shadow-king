#include "okage/battle_actor_math.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

void require(bool ok){if(!ok)throw std::runtime_error("Battle actor math assertion failed");}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}require(caught);}
std::uint32_t word(std::span<const std::uint8_t>b,unsigned a){return unsigned(b[a])|(unsigned(b[a+1])<<8)|(unsigned(b[a+2])<<16)|(unsigned(b[a+3])<<24);}
void half(std::span<std::uint8_t>b,unsigned a,std::uint16_t v){b[a]=v;b[a+1]=v>>8;}
struct Random {
    std::vector<std::uint32_t> values;
    unsigned count=0;
    std::uint32_t next(){if(count==values.size())throw std::runtime_error("Unexpected RNG draw");return values[count++];}
};
int main(){try {
    struct Timer {int parameter;unsigned kind,shifted;std::uint32_t duration,initial;};
    const std::array cases={
        Timer{55,0,1,0x434b68b9,0x42cf7a2d},
        Timer{0,0,0,0x4449e4d8,0x43c9e4d7},
        Timer{100,0,39,0x42480000,0x42320000},
        Timer{-32768,1,49,0x4c9c199a,0x4c18fa5e},
        Timer{std::numeric_limits<int>::max(),0,39,0x5c9b26c6,0x5c8a15b5},
        Timer{std::numeric_limits<int>::min(),1,49,0x5c9b26c9,0x5c180c68},
        Timer{101,1,0,0x42484d92,0},
        Timer{67,0,0,0x43048000,0x42848000}};
    const auto old=std::fegetround();
    for(int rounding:{FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        require(!std::fesetround(rounding));
        for(auto t:cases){
            std::array<std::uint8_t,52>b;b.fill(0xa5);const auto before=b;
            Random rng{{t.shifted<<5}};
            auto span=std::span(b).subspan(16,24);
            okage::initialize_battle_actor_timer(span,t.parameter,t.kind,0,[&]{return rng.next();});
            require(rng.count==1&&std::fegetround()==rounding);
            require(word(span,0)==t.initial&&word(span,4)==t.duration&&word(span,8)==0&&word(span,16)==0x3f800000);
            for(unsigned i=0;i<b.size();++i)if(i<16||i>=36||(i>=28&&i<32))require(b[i]==before[i]);
        }
    }
    require(!std::fesetround(old));
    std::array<std::uint8_t,0x14> timer;timer.fill(0xa5);
    for(unsigned kind:{0u,2u,0xffffffffu}) {
        okage::initialize_battle_actor_timer(timer,100,kind,1,{});
        require(word(timer,0)==0&&word(timer,4)==0x42480000&&word(timer,12)==0xa5a5a5a5);
    }
    Random enemy{{32}};
    okage::initialize_battle_actor_timer(timer,100,1,1,[&]{return enemy.next();});
    require(enemy.count==1&&word(timer,0)==0x3f000000); // Enemy ignores party zero-timer gate.
    const auto before=timer;
    rejects([&]{okage::initialize_battle_actor_timer(std::span(timer).first(19),0,0,0,{});});require(timer==before);
    rejects([&]{okage::initialize_battle_actor_timer(timer,0,0,0,{});});require(timer==before);
    require(!std::fesetround(FE_UPWARD));
    rejects([&]{okage::initialize_battle_actor_timer(timer,55,0,0,[]{throw std::runtime_error("RNG failure");return 0u;});});
    require(std::fegetround()==FE_UPWARD);require(!std::fesetround(old));

    auto selector=[](std::int16_t selected,std::int32_t current,std::int16_t denominator,
                    int formation,std::vector<std::uint32_t> draws,int expected,int final_selector) {
        std::array<std::uint8_t,100>b;b.fill(0xa5);auto span=std::span(b).subspan(16,72);
        half(span,0x18,std::uint16_t(denominator));half(span,0x3c,std::uint16_t(selected));const auto before=b;
        Random rng{draws};
        okage::initialize_battle_enemy_selector(span,current,formation,[&]{return rng.next();});
        require(rng.count==draws.size());require((word(span,0x3c)&65535)==std::uint16_t(final_selector));
        require(word(span,0x40)==unsigned(expected));
        for(unsigned i=0;i<b.size();++i)if(i<16+0x3c||i>=16+0x44||i==16+0x3e||i==16+0x3f)require(b[i]==before[i]);
    };
    for(auto s:{std::int16_t(-1),std::int16_t(0),std::int16_t(10)})selector(s,0,0,3,{32},3,s);
    selector(1,0,0,3,{32,96},0,1);selector(1,0,0,3,{32,32},1,1);
    selector(2,0,0,3,{32,96},6,2);selector(2,0,0,3,{32,32},5,2);
    selector(3,0,0,3,{32,128},5,3);selector(4,0,0,3,{32},0,4);
    selector(5,0,0,3,{32},6,5);selector(6,0,0,3,{32,160},2,6);
    for(auto [count,result]:{std::pair{0,0},std::pair{3,2},std::pair{20,6},std::pair{65535,0}})
        selector(7,0,0,count,{32},result,7);
    selector(8,4,7,3,{32},3,8);selector(9,4,7,3,{32},3,9);
    selector(8,9363,1,3,{32},4,8);selector(9,9363,1,3,{32},2,9);
    selector(8,0x40000000,-1,3,{32},1,8); // Signed numerator wraps before division.
    selector(8,__builtin_bit_cast(std::int32_t, (0x36db6db7u)),-1,3,{32},0,8); // INT_MIN/-1.
    // Replacement precedes dispatch, which can request a third draw.
    selector(9,0,0,3,{0,32,64},1,1);selector(1,0,0,3,{0,128},0,4);
    selector(1,0,0,3,{0,288},3,0);
    // Signed SRA/remainder, even for synthetic values outside actual rand's 31-bit range.
    selector(3,0,0,3,{32,0xffffffe0u},0,3);
    std::array<std::uint8_t,0x44>b;b.fill(0xa5);const auto untouched=b;unsigned draws=0;
    auto rand=[&]{++draws;return 32u;};
    rejects([&]{okage::initialize_battle_enemy_selector(std::span(b).first(0x43),0,3,rand);});
    rejects([&]{okage::initialize_battle_enemy_selector(b,0,-1,rand);});
    rejects([&]{okage::initialize_battle_enemy_selector(b,0,65536,rand);});
    rejects([&]{okage::initialize_battle_enemy_selector(b,0,3,{});});require(draws==0&&b==untouched);
    half(b,0x3c,8);half(b,0x18,0);const auto trap=b;
    rejects([&]{okage::initialize_battle_enemy_selector(b,5,3,rand);});require(draws==1&&b==trap);
    half(b,0x3c,1);Random replacement{{0,256}};
    rejects([&]{okage::initialize_battle_enemy_selector(b,5,3,[&]{return replacement.next();});});
    require(replacement.count==2&&(word(b,0x3c)&65535)==8&&word(b,0x40)==0xa5a5a5a5);
    std::cout<<"Battle actor math tests passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
