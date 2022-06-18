#include "okage/battle_ui.hpp"
#include "okage/event.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t word(Bytes data,unsigned at) {
    if(at>data.size()||data.size()-at<4)throw std::runtime_error("Battle UI data bounds");
    return unsigned(data[at])|(unsigned(data[at+1])<<8)|(unsigned(data[at+2])<<16)|(unsigned(data[at+3])<<24);
}
bool digest(Bytes data,unsigned at,std::string_view hex) {
    if(at>data.size()||data.size()-at<hex.size()/2)return false;
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<hex.size();++i)
        if(hex[i]!=digits[(data[at+i/2]>>(i%2?0:4))&15])return false;
    return true;
}
}
BattleUiData::BattleUiData(Bytes data) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','B','T','U','I','1',0};
    if(data.size()!=312||!std::equal(magic.begin(),magic.end(),data.begin())||
       word(data,8)!=1||word(data,12)!=2||word(data,16)!=28||word(data,20)!=112)
        throw std::runtime_error("Unsupported battle UI data format");
    if(!digest(data,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")||
       !digest(data,56,"5632c521fdd835a20c81960f9758447216733e2de4afc2d0e32b05ace9e8805b"))
        throw std::runtime_error("Unsupported battle UI source revision");
    for(unsigned i=0;i<layers_.size();++i) {
        auto& w=layers_[i].words;
        for(unsigned j=0;j<w.size();++j)w[j]=word(data,88+i*112+j*4);
        if(w[0]!=i+1||w[21]!=1||w[22]!=i+1||w[23]!=i+1||w[24]!=i+2||w[27])
            throw std::runtime_error("Unsupported battle UI host binding");
        if(w[1]!=40||w[2]!=6||w[3]!=2||w[20]>1||
           !w[25]||w[25]>32||!w[26]||w[26]>32)
            throw std::runtime_error("Battle UI window arguments outside recovered scope");
        for(unsigned j=4;j<16;++j)if(!std::isfinite(__builtin_bit_cast(float, (w[j]))))
            throw std::runtime_error("Nonfinite battle UI transform");
    }
}
std::int32_t UiPartySelection::at(unsigned row,unsigned column) const {
    if(row>=8||column>=8)throw std::runtime_error("UI party selection bounds");
    return rows_[row][column];
}
void UiPartySelection::refresh(std::span<std::uint8_t> variables,unsigned game_mode) {
    if(variables.size()<0xd78)throw std::runtime_error("UI party shared variable extent");
    const bool only_hero=event_variable(variables,0x40005)!=0;
    const auto half=[&](unsigned at) {
        return std::int32_t(__builtin_bit_cast(std::int16_t, (std::uint16_t(unsigned(variables[at])|(unsigned(variables[at+1])<<8)))));
    };
    counts_.fill(0);for(auto& row:rows_)row.fill(-1);
    for(unsigned row=0;row<8;++row) {
        unsigned party_ordinal=0;
        for(unsigned slot=0;slot<8;++slot) {
            if(slot==6||(only_hero&&slot!=0))continue;
            const unsigned record=0x5c4+slot*0x50;
            if(game_mode==4){variables[record+0x26]=100;variables[record+0x27]=0;}
            const auto membership=__builtin_bit_cast(std::int32_t, (word(variables,0x570+slot*4)));
            const bool party=membership==3;
            const bool ready=word(variables,record+8)!=0&&half(record+0x26)==100;
            bool include=false;std::int32_t value=slot;
            switch(row) {
            case 0:include=membership>0;value=half(record+2);break;
            case 1:include=membership>0;break;
            case 2:include=party;value=half(record+2);break;
            case 3:include=party;break;
            case 4:include=party&&ready;value=half(record+2);break;
            case 5:case 7:include=party&&ready;break;
            case 6:include=party&&ready;value=party_ordinal;if(party)++party_ordinal;break;
            }
            if(include)rows_[row][counts_[row]++]=value;
        }
    }
}
}
