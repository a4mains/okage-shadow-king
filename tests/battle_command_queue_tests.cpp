#include "okage/battle_command_queue.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F&& f) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected explicit battle command queue rejection");
}
void put32(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes[at+i]=std::uint8_t(value>>(8*i));
}
void put16(std::span<std::uint8_t> bytes,std::uint16_t value) {
    bytes[0]=std::uint8_t(value);bytes[1]=std::uint8_t(value>>8);
}
struct Fixture {
    okage::BattleState root;
    std::array<okage::BattleActor,3> actors{};
    std::array<std::array<std::uint8_t,1>,3> statuses{};
    std::array<std::array<std::uint8_t,2>,3> counters{};
    std::array<okage::BattleEntityBinding,3> bindings{};
    explicit Fixture(unsigned count=3) {
        root.initialize_secondary();
        put32(root.bytes,0x50,count);
        for(unsigned i=0;i<count;++i) {
            actors[i].entity_id=i;
            actors[i].stat_bindings[1]=statuses[i];
            actors[i].stat_bindings[2]=counters[i];
            bindings[i].actor=&actors[i];
        }
    }
};
void push_and_availability() {
    Fixture f(1);
    require(okage::battle_command_available(f.root)==1,"Secondary initializer sets source availability word to1");
    for(std::int32_t value=1;value<=40;++value) {
        okage::battle_command_push(f.root,0,value);
        require(okage::battle_command_available(f.root)==0,"Push clears source availability word");
    }
    require(__builtin_bit_cast(std::int32_t, (std::uint32_t(f.root.bytes[0x81a4])|
        (std::uint32_t(f.root.bytes[0x81a5])<<8)|(std::uint32_t(f.root.bytes[0x81a6])<<16)|
        (std::uint32_t(f.root.bytes[0x81a7])<<24)))==31,"Push saturates stack index at31");
    std::int32_t owner=-99;
    require(okage::battle_command_pop(f.root,f.bindings,owner)==40&&owner==0,
            "Saturated push overwrites slot31 and returns latest command");
    for(std::int32_t value=30;value>=1;--value)
        require(okage::battle_command_pop(f.root,f.bindings,owner)==value&&owner==0,
                "Remaining queue entries pop in LIFO order");
    owner=77;
    require(okage::battle_command_pop(f.root,f.bindings,owner)==-1&&owner==77,
            "Empty base slot returns source sentinel without changing output owner");
    require(okage::battle_command_available(f.root)==0,"Valid pops preserve availability");
}
void empty_and_negative_sentinel() {
    Fixture f(1);std::int32_t owner=12;
    require(okage::battle_command_pop(f.root,f.bindings,owner)==-1&&owner==12,
            "Empty index-zero slot returns -1 and preserves output");
    require(okage::battle_command_available(f.root)==1,"Empty pop does not change initialized availability");
    okage::battle_command_push(f.root,0,5);
    put32(f.root.bytes,0x80a4+8,0xffffffffu);
    put32(f.root.bytes,0x81a8,0);
    rejects([&]{okage::battle_command_pop(f.root,f.bindings,owner);});
    require(f.root.bytes[0x81a4]==1&&okage::battle_command_available(f.root)==0,
            "Nonprogressing negative slot guard does not repair queue state");
}
void null_and_invalid_owners() {
    Fixture f(2);
    f.bindings[0].actor=nullptr;
    okage::battle_command_push(f.root,1,22);
    okage::battle_command_push(f.root,0,11);
    std::int32_t owner=-7;
    require(okage::battle_command_pop(f.root,f.bindings,owner)==22&&owner==1,
            "Null typed actor is discarded before the next valid LIFO entry");
    require(okage::battle_command_available(f.root)==1,"Null owner discard sets available flag");

    Fixture invalid(1);
    okage::battle_command_push(invalid.root,0,4);
    // Simulate corrupted queue data. It must not silently scan or discard.
    put32(invalid.root.bytes,0x80a4+8,1);
    const auto before=invalid.root.bytes;
    rejects([&]{okage::battle_command_pop(invalid.root,invalid.bindings,owner);});
    require(invalid.root.bytes==before,"Out-of-range owner throws before queue mutation");
    rejects([&]{okage::battle_command_push(invalid.root,1,1);});
}
void discarded_top_stops_before_base_slot() {
    for(bool incapacitated:{false,true}) {
        Fixture f(2);
        if(incapacitated) {
            f.statuses[0][0]=6;
            f.bindings[0].actor=&f.actors[0];
        } else {
            f.bindings[0].actor=nullptr;
        }
        put32(f.root.bytes,0x80a4,1);put32(f.root.bytes,0x80a8,99);
        put32(f.root.bytes,0x80ac,incapacitated?0:0);
        put32(f.root.bytes,0x80b0,incapacitated?55:56);
        put32(f.root.bytes,0x81a4,1);put32(f.root.bytes,0x81a8,0);
        std::int32_t owner=73;
        require(okage::battle_command_pop(f.root,f.bindings,owner)==-1&&owner==73,
                "Discarding slot1 returns sentinel instead of scanning the populated base slot");
        require(f.root.bytes[0x81a4]==0&&okage::battle_command_available(f.root)==1,
                "Discard stops at base index and records unavailable state");
        require(f.root.bytes[0x80a4]==1&&f.root.bytes[0x80a8]==99,
                "Base slot remains byte-for-byte unconsumed after discard");
    }
}
void null_base_slot_underflow() {
    Fixture f(1);f.bindings[0].actor=nullptr;
    put32(f.root.bytes,0x80a4,0);put32(f.root.bytes,0x80a8,88);
    put32(f.root.bytes,0x81a4,0);put32(f.root.bytes,0x81a8,0);
    std::int32_t owner=14;
    require(okage::battle_command_pop(f.root,f.bindings,owner)==-1&&owner==14,
            "Null source entry at index0 returns sentinel without output mutation");
    require(f.root.bytes[0x81a4]==0xff&&f.root.bytes[0x81a5]==0xff&&
            f.root.bytes[0x81a6]==0xff&&f.root.bytes[0x81a7]==0xff,
            "Index0 null discard stores signed index-1");
    require(okage::battle_command_available(f.root)==1&&f.root.bytes[0x80a4]==0&&
            f.root.bytes[0x80a8]==88,
            "Index0 null discard does not read a wrapped unsigned stack offset");
}
void incapacitated_status_rules() {
    Fixture f(1);std::int32_t owner=-1;
    f.statuses[0][0]=4;put16(f.counters[0],1);
    require(!okage::battle_actor_incapacitated(&f.actors[0]),"Status4 signed counter1 is available");
    okage::battle_command_push(f.root,0,41);
    require(okage::battle_command_pop(f.root,f.bindings,owner)==41&&owner==0,
            "Status4 counter below2 returns queued command");

    f.statuses[0][0]=4;put16(f.counters[0],2);
    require(okage::battle_actor_incapacitated(&f.actors[0]),"Status4 counter2 is incapacitated");
    okage::battle_command_push(f.root,0,42);
    require(okage::battle_command_pop(f.root,f.bindings,owner)==-1,
            "Status4 counter at least2 is discarded");
    require(okage::battle_command_available(f.root)==1,"Status4 discard sets availability");

    f.statuses[0][0]=4;put16(f.counters[0],0xffff);
    require(!okage::battle_actor_incapacitated(&f.actors[0]),"Status4 counter uses signed int16 comparison");
    f.statuses[0][0]=6;put16(f.counters[0],0);
    require(okage::battle_actor_incapacitated(&f.actors[0]),"Status6 is always incapacitated");
    okage::battle_command_push(f.root,0,43);
    require(okage::battle_command_pop(f.root,f.bindings,owner)==-1&&
            okage::battle_command_available(f.root)==1,"Status6 command is discarded and marks unavailable");
    require(okage::battle_actor_incapacitated(nullptr),"Source helper treats null actor as incapacitated");

    f.statuses[0][0]=std::uint8_t(0xff);put16(f.counters[0],2);
    require(!okage::battle_actor_incapacitated(&f.actors[0]),"Other signed status values remain available");
}
void binding_validation() {
    Fixture f(2);std::int32_t owner=9;
    okage::battle_command_push(f.root,0,7);
    rejects([&]{okage::battle_command_pop(f.root,std::span(f.bindings).first(1),owner);});
    f.actors[1].entity_id=0;
    rejects([&]{okage::battle_command_pop(f.root,f.bindings,owner);});
    require(f.root.bytes[0x81a4]==1,"Binding validation precedes queue mutation");

    Fixture short_stats(1);
    short_stats.actors[0].stat_bindings[1]={};
    okage::battle_command_push(short_stats.root,0,2);
    rejects([&]{okage::battle_command_pop(short_stats.root,short_stats.bindings,owner);});
    require(short_stats.root.bytes[0x81a4]==1,"Malformed status binding rejects before queue mutation");
}
}
int main() {
    try {
        push_and_availability();empty_and_negative_sentinel();null_and_invalid_owners();
        discarded_top_stops_before_base_slot();null_base_slot_underflow();
        incapacitated_status_rules();binding_validation();
        std::cout<<"Battle command queue source-contract tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
