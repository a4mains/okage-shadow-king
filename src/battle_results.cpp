#include "okage/battle_results.hpp"
#include "okage/event.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
std::uint16_t half(Bytes bytes,unsigned at) {
    return std::uint16_t(unsigned(bytes[at])|(unsigned(bytes[at+1])<<8));
}
std::int32_t signed_half(Bytes bytes,unsigned at) {
    return std::bit_cast<std::int16_t>(half(bytes,at));
}
std::int32_t add_capped(std::int32_t current,std::int32_t quantity,std::int32_t cap) {
    const auto sum=std::bit_cast<std::int32_t>(std::uint32_t(current)+std::uint32_t(quantity));
    return sum>cap?cap:sum;
}
}
void commit_battle_result_rewards(std::span<std::uint8_t> variables,Bytes reward) {
    if(reward.size()<0x24)throw std::runtime_error("Battle results reward extent");
    (void)event_variable(variables,0x40003);
    for(unsigned i=0;i<8;++i) {
        const auto id=signed_half(reward,4+i*4);
        if(id>0)(void)event_variable(variables,0x30000u|std::uint32_t(id));
    }
    // 16bf58..16bf98: each pair observes prior writes, including duplicate IDs.
    for(unsigned i=0;i<8;++i) {
        const auto id=signed_half(reward,4+i*4);
        if(id<=0)continue;
        const auto current=event_variable(variables,0x30000u|std::uint32_t(id));
        set_event_variable(variables,unsigned(id),3,add_capped(current,signed_half(reward,6+i*4),99));
    }
    // Currency is after all item stores. Reward+2 is LHU, unlike quantities.
    const auto currency=event_variable(variables,0x40003);
    set_event_variable(variables,3,4,add_capped(currency,half(reward,2),999999999));
}
}
