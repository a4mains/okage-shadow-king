#include "okage/battle_action_data.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t p) {
    if(p>b.size()||b.size()-p<4)throw std::runtime_error("Battle action data extent");
    return unsigned(b[p])|(unsigned(b[p+1])<<8)|(unsigned(b[p+2])<<16)|(unsigned(b[p+3])<<24);
}
std::int16_t narrow(std::int32_t x){return std::bit_cast<std::int16_t>(std::uint16_t(x));}
bool digest(Bytes b,std::size_t at,std::string_view value) {
    constexpr std::string_view hex="0123456789abcdef";
    for(unsigned i=0;i<value.size();++i)if(value[i]!=hex[(b[at+i/2]>>(i%2?0:4))&15])return false;
    return true;
}
}
std::int16_t BattleActionDescriptor::half(std::size_t offset) const {
    if(offset&1||offset>bytes.size()-2)throw std::runtime_error("Battle action descriptor halfword extent");
    return std::bit_cast<std::int16_t>(std::uint16_t(unsigned(bytes[offset])|(unsigned(bytes[offset+1])<<8)));
}
BattleActionData::BattleActionData(Bytes b) {
    constexpr std::array<std::uint8_t,8> magic{'O','K','B','A','C','T','1',0};
    constexpr std::size_t header=136,payload=256*4+(98*2+159+15)*0x88;
    if(b.size()!=header+payload||!std::equal(magic.begin(),magic.end(),b.begin())||
       word(b,8)!=1||word(b,12)!=256||word(b,16)!=256||word(b,20)!=98||
       word(b,24)!=159||word(b,28)!=15||word(b,32)!=0x88||word(b,36)!=payload)
        throw std::runtime_error("Unsupported battle action data format");
    if(!digest(b,40,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")||
       !digest(b,72,"973964019ddb24d033b0e10f4f3e2cf72fafdee9bdb25ee143ee87c832b0aff4"))
        throw std::runtime_error("Unsupported battle action source revision");
    std::size_t at=header;
    const auto map=[&](std::vector<std::int16_t>& out,unsigned limit) {
        for(unsigned i=0;i<256;++i,at+=2) {
            auto v=std::bit_cast<std::int16_t>(std::uint16_t(unsigned(b[at])|(unsigned(b[at+1])<<8)));
            if(v<0||unsigned(v)>=limit)throw std::runtime_error("Battle action descriptor mapping bounds");
            out.push_back(v);
        }
    };
    map(unit_map_,98);map(action_map_,159);
    std::uint16_t identity=0;
    const auto records=[&](std::vector<BattleActionDescriptor>& out,unsigned count) {
        out.resize(count);for(auto& r:out){r.identity=identity++;std::copy_n(b.begin()+at,r.bytes.size(),r.bytes.begin());at+=r.bytes.size();}
    };
    records(paired_,196);records(generic_,159);records(secondary_,15);
}
const BattleActionDescriptor& BattleActionData::motion(std::int32_t action,std::int32_t kind,
                                                       std::int32_t source) const {
    const auto id=narrow(action);
    if(id==200||id==201||id==208) {
        const auto identity=narrow(kind)==0?narrow(std::int32_t(std::uint16_t(source))+170):narrow(source);
        if(identity<0||std::size_t(identity)>=unit_map_.size())
            throw std::runtime_error("Battle action unit mapping outside imported region");
        return paired_[unsigned(unit_map_[unsigned(identity)])*2+(id==201?1:0)];
    }
    if(id<0||std::size_t(id)>=action_map_.size())
        throw std::runtime_error("Battle action outside owned waza extent");
    return generic_[unsigned(action_map_[unsigned(id)])];
}
BattleSecondaryActionSelection BattleActionData::secondary(std::int32_t action,std::int32_t mode) const {
    const auto id=narrow(action),selected_mode=narrow(mode);
    unsigned index;std::int16_t type=15;
    switch(id) {
    case 20:index=3;break;case 21:index=4;break;
    case 205:index=5;type=7;break;case 207:index=6;type=7;break;
    case 22:index=7;break;case 23:index=8;break;case 24:index=9;break;
    case 25:index=10;break;case 26:index=11;break;case 27:index=12;break;
    case 28:index=13;break;case 29:index=14;break;
    default:index=selected_mode==1?1:selected_mode==2?2:0;type=12;break;
    }
    return {secondary_[index],type};
}
}
