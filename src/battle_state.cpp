#include "okage/battle_state.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void validate(std::span<std::uint8_t> root) {
    if(root.size()<battle_state_size)throw std::runtime_error("Battle state backing extent");
}
}
void initialize_battle_entities(std::span<std::uint8_t> root,
                                std::span<const std::uint8_t> default_bytes) {
    validate(root);
    if(default_bytes.size()!=16)throw std::runtime_error("Battle entity default extent");
    // Snapshot the explicit input before mutation, even if its span aliases root.
    std::array<std::uint8_t,16> defaults;
    std::copy(default_bytes.begin(),default_bytes.end(),defaults.begin());
    std::fill_n(root.begin()+0x50,4,0); // 0x1511d8 table header; +4..+f untouched.
    for(std::size_t i=0;i<battle_entity_count;++i) {
        auto record=root.subspan(0x60+i*battle_entity_size,battle_entity_size);
        // 0x1513f8: 0x40 zero bytes, copied vector, then 0x10 zero bytes.
        std::fill(record.begin(),record.begin()+0x40,0);
        std::copy(defaults.begin(),defaults.end(),record.begin()+0x40);
        std::fill(record.begin()+0x50,record.begin()+0x60,0);
        // 0x158b28: preserve the two bytes at +0x16e/+0x16f.
        std::fill(record.begin()+0x60,record.begin()+0x16e,0);
        for(auto offset:{0x60,0x61,0x62,0x63,0x66,0x67})record[offset]=0xff;
        // 0x158c58: two zero halfwords, then 16 four-halfword records.
        std::fill(record.begin()+0x170,record.begin()+0x1f4,0);
        for(unsigned j=0;j<16;++j)record[0x174+j*8]=record[0x175+j*8]=0xff;
        std::fill(record.begin()+0x1f4,record.end(),0);
        // 0x1511d8: clear final word, then OR 0x80000000 through the owner.
        record[0x1ff]=0x80;
    }
}
void initialize_battle_secondary(std::span<std::uint8_t> root) {
    validate(root);
    // 0x151798: 64 words -1, followed by words 0 and 1.
    std::fill(root.begin()+0x80a4,root.begin()+0x81a4,0xff);
    std::fill(root.begin()+0x81a4,root.begin()+0x81ac,0);
    root[0x81a8]=1;
}
void initialize_battle_root(std::span<std::uint8_t> root,const BattleRootParameters& parameters) {
    validate(root);
    const auto put=[&](std::size_t offset,std::uint32_t value) {
        for(unsigned i=0;i<4;++i)root[offset+i]=std::uint8_t(value>>(i*8));
    };
    // 0x149fd8 scalar writes. Typed runtime ownership handles the omitted
    // header callback/global-current-root/resource pointer operations.
    put(0x24,0);put(0x20,0);
    put(0x2c,__builtin_bit_cast(std::uint32_t, (parameters.scale)));
    put(0x80a0,0xffffffba); // Signed -70; not a readiness flag.
    put(0x28,0);
    put(0x24,0);put(0x30,0);
    put(0x3c,parameters.formation);put(0x44,0);
    put(0x40,__builtin_bit_cast(std::uint32_t, (parameters.stage)));
    // Pinned owned .rodata at 0x236f70 contains 16 zero bytes. Generic helper still accepts an
    // explicit default for independent callers and original helper probes.
    constexpr std::array<std::uint8_t,16> constructor_defaults{};
    initialize_battle_entities(root,constructor_defaults);
    initialize_battle_secondary(root);
    put(0x8080,0);put(0x81b0,0);put(0x81ac,0);put(0x807c,0);
    put(0x81bc,0);put(0x81b4,0);put(0x81b8,0); // 0x14b008.
    put(0x81c0,0);
}
std::span<std::uint8_t,battle_entity_size> BattleState::entity(std::size_t index) {
    if(index>=battle_entity_count)throw std::runtime_error("Battle entity index");
    return std::span<std::uint8_t,battle_entity_size>(bytes.data()+0x60+index*battle_entity_size,battle_entity_size);
}
std::span<const std::uint8_t,battle_entity_size> BattleState::entity(std::size_t index) const {
    if(index>=battle_entity_count)throw std::runtime_error("Battle entity index");
    return std::span<const std::uint8_t,battle_entity_size>(bytes.data()+0x60+index*battle_entity_size,battle_entity_size);
}
}
