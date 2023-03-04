#include "okage/battle_ui_roster.hpp"
#include "okage/battle_actor.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
std::uint32_t word(std::span<const std::uint8_t> b,std::size_t at) {
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
std::int16_t signed_half(std::span<const std::uint8_t> b,std::size_t at) {
    return __builtin_bit_cast(std::int16_t, (std::uint16_t(std::uint16_t(b[at])|
                                                      (std::uint16_t(b[at+1])<<8))));
}
}
std::array<std::int32_t,4> battle_ui_ready_party(std::span<const BattleEntityBinding> entities) {
    if(entities.size()<3)throw std::runtime_error("Battle UI requires three represented party bindings");
    std::array<std::int32_t,4> ready{-1,-1,-1,-1};
    unsigned count=0;
    for(unsigned i=0;i<3;++i) {
        const auto* actor=entities[i].actor;
        //1512f0 registers the body pointer (task+40).154418 dereferences
        // body+1dc/+1e0; BattleActor::storage starts at the task header.
        if(actor&&word(actor->storage,0x40+0x1dc)==2&&signed_half(actor->storage,0x40+0x1e0)==2)ready[count++]=std::int32_t(i);
    }
    return ready;
}
unsigned battle_ui_available_party(std::span<const BattleEntityBinding> entities) {
    const auto ready=battle_ui_ready_party(entities);
    return unsigned(std::count_if(ready.begin(),ready.end(),[](auto id){return id>=0;}));
}
unsigned battle_ui_enemy_group_count(const BattleState& root,
        std::span<const BattleActor* const> enemies,std::int32_t group) {
    if(enemies.size()>battle_entity_count)throw std::runtime_error("Battle UI enemy list exceeds native entity capacity");
    const auto selected=__builtin_bit_cast(std::int16_t, (std::uint16_t(group)));
    unsigned count=0;
    for(const auto* actor:enemies) {
        if(!actor)throw std::runtime_error("Battle UI enemy child is missing");
        if(signed_half(actor->storage,0x46)!=selected)continue;
        const auto index=signed_half(actor->storage,0x130);
        if(index<0||unsigned(index)>=battle_entity_count)
            throw std::runtime_error("Battle UI enemy child entity index is outside native root");
        if(!(word(root.entity(unsigned(index)),0x1fc)&0x80000000u))++count;
    }
    return count;
}
void BattleUiFormationSummary::refresh(const BattleState& root,const BattleData& data,
        std::span<const BattleActor* const> enemies,unsigned mode) {
    const auto selected=mode==4?1:std::clamp(__builtin_bit_cast(std::int32_t, (word(root.bytes,0x3c))),0,10000);
    count=0;
    if(!data.has_formation(selected))return;
    refresh_entries(data.formation(selected),root,enemies,mode);
}
void BattleUiFormationSummary::refresh_entries(std::span<const BattleFormationEntry> entries,
        const BattleState& root,std::span<const BattleActor* const> enemies,unsigned mode) {
    count=0;
    std::int8_t previous=-1;
    for(const auto& entry:entries) {
        if(entry.field_2==previous)continue;
        const auto live=mode==4?8u:battle_ui_enemy_group_count(root,enemies,entry.field_2);
        if(live) {
            if(count>=unit_ids.size())throw std::runtime_error("Battle UI formation summary exceeds eight source slots");
            unit_ids[count]=entry.unit_id;counts[count]=std::int32_t(live);groups[count]=entry.field_2;
            ++count;
        }
        previous=entry.field_2;
    }
}
}
