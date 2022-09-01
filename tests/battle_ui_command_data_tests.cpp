#include "okage/battle_ui_command_data.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
std::vector<std::uint8_t> fixture(){
    std::vector<std::uint8_t>b(144);const std::string magic="OKBCMD1";std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1);put(b,12,64);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)b[16+i]=std::stoul(hash.substr(i*2,2),nullptr,16);
    const std::int32_t words[]={5,19,20,6,2,0,4,6,12,24,6,18,-1000,0,512,2048};
    for(unsigned i=0;i<16;++i)put(b,80+i*4,static_cast<std::uint32_t>(words[i]));
    return b;
}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected rejection");}
}
int main(){
    auto data=fixture();okage::BattleUiCommandData parsed(data);
    require(parsed.category_zero_commands()==std::array<std::int32_t,2>{5,19},"category-zero table");
    require(parsed.category_nonzero_commands()==std::array<std::int32_t,2>{20,6},"category-nonzero table");
    require(parsed.selection_commands()==std::array<std::int32_t,4>{2,0,4,6},"selection table");
    require(parsed.authored_metrics_default()==std::array<std::int32_t,4>{12,24,6,18},"metrics default");
    require(parsed.authored_clip_default()==std::array<std::int32_t,4>{-1000,0,512,2048},"clip default");
    data[80]=4;rejects([&]{okage::BattleUiCommandData bad(data);});
    data=fixture();data.pop_back();rejects([&]{okage::BattleUiCommandData bad(data);});
    std::cout<<"battle UI command data tests passed\n";
}
