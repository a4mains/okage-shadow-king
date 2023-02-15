#include "okage/battle_command_queue.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::size_t queue_begin=0x80a4;
constexpr std::size_t queue_index=0x81a4;
constexpr std::size_t available_flag=0x81a8;
constexpr std::size_t queue_slots=32;
constexpr std::size_t entity_count_offset=0x50;

void require(bool ok,const char* message) {
    if(!ok) throw std::runtime_error(message);
}
std::uint32_t word(std::span<const std::uint8_t> bytes,std::size_t at) {
    require(at<=bytes.size()&&bytes.size()-at>=4,"Battle command queue state bounds");
    return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8)|
           (std::uint32_t(bytes[at+2])<<16)|(std::uint32_t(bytes[at+3])<<24);
}
void put_word(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t value) {
    require(at<=bytes.size()&&bytes.size()-at>=4,"Battle command queue state bounds");
    for(unsigned i=0;i<4;++i)bytes[at+i]=std::uint8_t(value>>(8*i));
}
std::int32_t signed_word(std::uint32_t value) { return __builtin_bit_cast(std::int32_t, (value)); }
void validate_bindings(const BattleState& state,std::span<const BattleEntityBinding> bindings) {
    const auto count=word(state.bytes,entity_count_offset);
    require(count<=battle_entity_count,"Battle command queue entity count exceeds source capacity");
    require(bindings.size()>=count,"Battle command queue bindings do not cover root entity count");
    for(std::uint32_t i=0;i<count;++i) {
        const auto* actor=bindings[i].actor;
        if(actor) require(actor->entity_id==i,"Battle command queue binding identity mismatch");
    }
}
}

bool battle_actor_incapacitated(const BattleActor* actor) {
    if(!actor)return true;
    const auto status=actor->stat_bindings[1];
    const auto counter=actor->stat_bindings[2];
    require(status.size()>=1,"Battle command actor status binding is unavailable");
    require(counter.size()>=2,"Battle command actor counter binding is unavailable");
    const auto signed_status=__builtin_bit_cast(std::int8_t, (status[0]));
    if(signed_status==4) {
        const auto raw=std::uint16_t(std::uint16_t(counter[0])|
                                     (std::uint16_t(counter[1])<<8));
        const auto signed_counter=__builtin_bit_cast(std::int16_t, (raw));
        return signed_counter>=2;
    }
    return signed_status==6;
}

void battle_command_push(BattleState& state,std::int32_t owner_index,std::int32_t command) {
    const auto count=word(state.bytes,entity_count_offset);
    require(count<=battle_entity_count,"Battle command queue entity count exceeds source capacity");
    require(owner_index>=0&&std::uint32_t(owner_index)<count,
            "Battle command queue owner index is outside registered entities");
    const auto old_index=signed_word(word(state.bytes,queue_index));
    require(old_index>=0&&old_index<static_cast<std::int32_t>(queue_slots),
            "Battle command queue stack index is invalid");
    auto next=old_index+1;
    if(next>=static_cast<std::int32_t>(queue_slots))next=static_cast<std::int32_t>(queue_slots-1);
    const auto slot=queue_begin+std::size_t(next)*8;
    put_word(state.bytes,queue_index,std::uint32_t(next));
    put_word(state.bytes,slot,std::uint32_t(owner_index));
    put_word(state.bytes,slot+4,std::uint32_t(command));
    put_word(state.bytes,available_flag,0);
}

std::uint32_t battle_command_available(const BattleState& state) {
    return word(state.bytes,available_flag);
}

std::int32_t battle_command_pop(BattleState& state,
        std::span<const BattleEntityBinding> bindings,std::int32_t& owner_out) {
    validate_bindings(state,bindings);
    auto index=signed_word(word(state.bytes,queue_index));
    require(index>=0&&index<static_cast<std::int32_t>(queue_slots),
            "Battle command queue stack index is invalid");
    for(;;) {
        const auto slot=queue_begin+std::size_t(index)*8;
        const auto owner=signed_word(word(state.bytes,slot));
        if(owner<0) {
            if(index>0)throw std::runtime_error("Battle command queue has a negative owner above its base slot");
            return -1;
        }
        const auto count=word(state.bytes,entity_count_offset);
        require(std::uint32_t(owner)<count,"Battle command queue owner index is outside registered entities");
        const auto* actor=bindings[std::size_t(owner)].actor;
        if(!actor) {
            put_word(state.bytes,available_flag,1);
            --index;put_word(state.bytes,queue_index,std::uint32_t(index));
            if(index<=0)return -1;
            continue;
        }
        const auto command=signed_word(word(state.bytes,slot+4));
        if(battle_actor_incapacitated(actor)) {
            put_word(state.bytes,available_flag,1);
            --index;put_word(state.bytes,queue_index,std::uint32_t(index));
            if(index<=0)return -1;
            continue;
        }
        owner_out=owner;
        put_word(state.bytes,slot,0xffffffffu);
        put_word(state.bytes,slot+4,0xffffffffu);
        --index;put_word(state.bytes,queue_index,std::uint32_t(index));
        return command;
    }
}
}
