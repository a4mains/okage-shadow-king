#include "okage/battle_enemy_commands.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected rejection");}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(i*8);}
std::uint32_t word(okage::Bytes b,unsigned at){return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t>b(56+84*7*4);
    const std::string_view magic="OKBECMD1";std::copy(magic.begin(),magic.end(),b.begin());
    for(auto [at,value]:{std::pair{8u,1u},{12u,84u},{16u,7u},{20u,4u}})put(b,at,value);
    const std::string_view hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)b[24+i]=std::stoul(std::string(hash.substr(i*2,2)),nullptr,16);
    for(unsigned i=0;i<84*7;++i)put(b,56+i*4,(i*0x10203u)^0xa5960000u);
    put(b,56,0x1234ffff);put(b,56+4,0x98767fff);put(b,unsigned(b.size()-4),0xbeef8000);
    return b;
}
void verify_all(const std::vector<std::uint8_t>& b) {
    okage::BattleEnemyCommands table(b);
    for(std::int16_t r=0;r<84;++r)for(std::int16_t s=0;s<7;++s) {
        auto expected=word(b,56+(unsigned(r)*7+unsigned(s))*4);
        require(table.cell(r,s)==expected,"Entire cell retained in row-major order");
        require(table.command(r,s)==std::bit_cast<std::int16_t>(std::uint16_t(expected)),"Signed low halfword lookup");
    }
}
}
int main(int argc,char**argv) {
    try {
        auto b=fixture();okage::BattleEnemyCommands table(b);verify_all(b);
        require(table.command(0,0)==-1&&table.command(0,1)==32767&&table.command(83,6)==-32768,"Low-half signed boundaries ignore high bits");
        //The native table owns the cells, not a borrowed input span.
        std::fill(b.begin(),b.end(),0);require(table.cell(83,6)==0xbeef8000,"Input lifetime independence");
        for(auto [r,s]:{std::pair{-1,0},{84,0},{0,-1},{0,7},{-32768,0},{0,32767}}) {
            rejects([&]{(void)table.command(std::int16_t(r),std::int16_t(s));});
            rejects([&]{(void)table.cell(std::int16_t(r),std::int16_t(s));});
        }
        b=fixture();
        for(unsigned at:{0u,8u,12u,16u,20u,24u,55u}) {
            auto bad=b;bad[at]^=1;rejects([&]{okage::BattleEnemyCommands invalid(bad);});
        }
        for(unsigned size:{0u,7u,55u,56u,unsigned(b.size()-1)}) {
            rejects([&]{okage::BattleEnemyCommands invalid(okage::Bytes(b).first(size));});
        }
        b.push_back(0);rejects([&]{okage::BattleEnemyCommands invalid(b);});
        if(argc==2) {
            std::ifstream f(argv[1],std::ios::binary);require(bool(f),"Owned artifact missing");
            std::vector<std::uint8_t> owned{std::istreambuf_iterator<char>(f),{}};verify_all(owned);
            std::cout<<"Owned588-cell accessor smoke passed\n";
        } else require(argc==1,"Expected optional owned artifact path");
        std::cout<<"Battle enemy commands tests passed\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
