#include "okage/battle_enemy_commands.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
        (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
}
BattleEnemyCommands::BattleEnemyCommands(Bytes b) {
    constexpr std::array<std::uint8_t,8> magic{'O','K','B','E','C','M','D','1'};
    constexpr std::size_t header=56;
    if(b.size()!=header+rows*slots*4||!std::equal(magic.begin(),magic.end(),b.begin())||
       word(b,8)!=1||word(b,12)!=rows||word(b,16)!=slots||word(b,20)!=4)
        throw std::runtime_error("Unsupported battle enemy command format");
    constexpr std::string_view hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    constexpr std::string_view hex="0123456789abcdef";
    for(unsigned i=0;i<hash.size();++i)
        if(hash[i]!=hex[(b[24+i/2]>>(i%2?0:4))&15])
            throw std::runtime_error("Unsupported battle enemy command source revision");
    for(unsigned i=0;i<cells_.size();++i)cells_[i]=word(b,header+i*4);
}
std::uint32_t BattleEnemyCommands::cell(std::int16_t row,std::int16_t slot) const {
    if(row<0||row>=int(rows)||slot<0||slot>=int(slots))
        throw std::runtime_error("Battle enemy command index outside authored region");
    return cells_[unsigned(row)*slots+unsigned(slot)];
}
std::int16_t BattleEnemyCommands::command(std::int16_t row,std::int16_t slot) const {
    //15c614..648: signed row*28 + signed slot*4, then LH (not LW).
    return std::bit_cast<std::int16_t>(std::uint16_t(cell(row,slot)));
}
}
