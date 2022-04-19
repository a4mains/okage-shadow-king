#include "okage/battle_entry.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void require(bool condition) {
    if(!condition)throw std::runtime_error("Battle entry test assertion failed");
}
void expect(const okage::BattleEntryContext& context,
            const std::array<std::uint8_t,8>& tail) {
    const auto& bytes=context.bytes();
    require(bytes.size()==56);
    for(std::size_t i=0;i<48;++i)require(bytes[i]==0);
    for(std::size_t i=0;i<tail.size();++i)require(bytes[48+i]==tail[i]);
}
}

int main() {
    try {
        static_assert(sizeof(okage::BattleEntryContext)==56);
        static_assert(std::is_copy_constructible_v<okage::BattleEntryContext>);
        static_assert(std::is_same_v<decltype(okage::BattleEntryContext{}.bytes()),
                                    const okage::BattleEntryContext::Storage&>);
        okage::BattleEntryContext fresh;
        expect(fresh,{});
        const auto zero=fresh.parameters();
        require(zero.argument1==0 && zero.argument2==0 && zero.argument3==0 && zero.argument4==0);

        okage::BattleEntryContext context(0x12345678,0xffffffff,0x80000000,0xabcd0102);
        expect(context,{0x78,0x56,0xff,0xff,0,0,2,1});
        const auto parameters=context.parameters();
        require(parameters.argument1==0x5678 && parameters.argument2==0xffff &&
                parameters.argument3==0 && parameters.argument4==0x0102);
        const auto snapshot=context.bytes();
        const auto copy=context;
        context.reset(1,17,0x10000,0x80008000);
        expect(context,{1,0,17,0,0,0,0,0x80});
        require(copy.bytes()==snapshot);
        context.reset(0,0,0,0);
        expect(context,{});
        context.reset(0xffff0000,0xffff0000,0xffff0000,0xffff0000);
        expect(context,{});

        // Changing the center value must not cross either 56-byte boundary.
        std::array<okage::BattleEntryContext,3> adjacent={
            okage::BattleEntryContext(0x1234,0x5678,0x9abc,0xdef0),
            okage::BattleEntryContext(1,2,3,4),
            okage::BattleEntryContext(0xfedc,0xba98,0x7654,0x3210)};
        const auto left=adjacent[0].bytes(),right=adjacent[2].bytes();
        adjacent[1].reset(0xdeadbeef,0xcafebabe,0xffffffff,0x11223344);
        expect(adjacent[1],{0xef,0xbe,0xbe,0xba,0xff,0xff,0x44,0x33});
        require(adjacent[0].bytes()==left && adjacent[2].bytes()==right);
        struct Guarded {
            std::array<std::uint8_t,16> before;
            okage::BattleEntryContext value;
            std::array<std::uint8_t,16> after;
        } guarded;
        static_assert(offsetof(Guarded,value)==16 && offsetof(Guarded,after)==72);
        guarded.before.fill(0xa5);
        guarded.after.fill(0x5a);
        guarded.value.reset(0xffffffff,0xffffffff,0xffffffff,0xffffffff);
        guarded.value.reset(0,0,0,0);
        expect(guarded.value,{});
        for(auto byte:guarded.before)require(byte==0xa5);
        for(auto byte:guarded.after)require(byte==0x5a);
        std::array<std::uint8_t,88> dirty;
        for(unsigned i=0;i<dirty.size();++i)dirty[i]=std::uint8_t(i*73+19);
        const auto dirty_before=dirty;
        for(unsigned extent:{0u,47u,48u,55u}) {
            bool rejected=false;
            try {okage::clear_battle_entry_prefix(std::span(dirty).subspan(16,extent));}
            catch(const std::runtime_error&){rejected=true;}
            require(rejected&&dirty==dirty_before);
        }
        okage::clear_battle_entry_prefix(std::span(dirty).subspan(16));
        for(unsigned i=0;i<dirty.size();++i)
            require(dirty[i]==(i>=16&&i<64?0:dirty_before[i]));
        const auto cleared=dirty;
        okage::clear_battle_entry_prefix(std::span(dirty).subspan(16,56));
        require(dirty==cleared);
        adjacent[1].clear_prefix();
        expect(adjacent[1],{0xef,0xbe,0xbe,0xba,0xff,0xff,0x44,0x33});
        require(adjacent[0].bytes()==left&&adjacent[2].bytes()==right);
        std::cout<<"Battle entry tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
