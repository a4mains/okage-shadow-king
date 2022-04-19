#include "okage/battle_state.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

void require(bool value) {if(!value)throw std::runtime_error("Battle state assertion failed");}
template<class F> void rejects(F f) {
    bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}require(rejected);
}
int main() {
    try {
        for(unsigned pattern=0;pattern<4;++pattern) {
            std::vector<std::uint8_t> guarded(okage::battle_state_size+64);
            for(std::size_t i=0;i<guarded.size();++i)
                guarded[i]=pattern==0?0:pattern==1?255:pattern==2?0xa5:(i*73+19)&255;
            const auto before=guarded;
            std::array<std::uint8_t,16> defaults;
            for(unsigned i=0;i<16;++i)defaults[i]=i*11+7;
            auto root=std::span(guarded).subspan(32,okage::battle_state_size);
            okage::initialize_battle_entities(root,defaults);
            unsigned preserved=0;
            for(std::size_t i=0;i<guarded.size();++i) {
                auto expected=before[i];
                if(i>=32+0x50&&i<32+0x54)expected=0;
                if(i>=32+0x60&&i<32+0x8060) {
                    const auto offset=(i-32-0x60)%512;
                    if(offset==0x16e||offset==0x16f)++preserved;
                    else if(offset>=0x40&&offset<0x50)expected=defaults[offset-0x40];
                    else if((offset>=0x60&&offset<0x64)||(offset>=0x66&&offset<0x68)||
                            (offset>=0x174&&offset<0x1f4&&(offset-0x174)%8<2))expected=255;
                    else expected=offset==0x1ff?128:0;
                }
                require(guarded[i]==expected);
            }
            require(preserved+12==140);
            const auto entities=guarded;
            okage::initialize_battle_secondary(root);
            for(std::size_t i=0;i<guarded.size();++i) {
                const auto at=i>=32?i-32:okage::battle_state_size;
                auto expected=entities[i];
                if(at>=0x80a4&&at<0x81a4)expected=255;
                else if(at>=0x81a4&&at<0x81ac)expected=at==0x81a8?1:0;
                require(guarded[i]==expected);
            }
            const auto both=guarded;
            okage::initialize_battle_entities(root,defaults);okage::initialize_battle_secondary(root);
            require(guarded==both);
        }
        for(const auto parameters:{okage::BattleRootParameters{1.f,1,17},
                okage::BattleRootParameters{-0.f,0xfedcba98u,-1}}) {
            std::vector<std::uint8_t> guarded(okage::battle_state_size+64);
            for(std::size_t i=0;i<guarded.size();++i)guarded[i]=std::uint8_t(i*73+19);
            auto expected=guarded;
            auto expected_root=std::span(expected).subspan(32,okage::battle_state_size);
            const std::array<std::uint8_t,16> source_defaults{};
            okage::initialize_battle_entities(expected_root,source_defaults);
            okage::initialize_battle_secondary(expected_root);
            for(auto offset:{0x20,0x24,0x28,0x30,0x44,0x807c,0x8080,
                             0x81ac,0x81b0,0x81b4,0x81b8,0x81bc,0x81c0})
                std::fill_n(expected_root.begin()+offset,4,0);
            const std::array<std::pair<unsigned,std::uint32_t>,4> scalars={{{0x2c,__builtin_bit_cast(std::uint32_t, (parameters.scale))},
                {0x3c,parameters.formation},{0x40,__builtin_bit_cast(std::uint32_t, (parameters.stage))},{0x80a0,0xffffffba}}};
            for(auto [offset,value]:scalars)
                for(unsigned i=0;i<4;++i)expected_root[offset+i]=value>>(i*8);
            okage::initialize_battle_root(std::span(guarded).subspan(32,okage::battle_state_size+32),parameters);
            require(guarded==expected);
            const auto initialized=guarded;
            okage::initialize_battle_root(std::span(guarded).subspan(32),parameters);
            require(guarded==initialized);
        }
        okage::BattleState constructed;
        std::array<std::uint8_t,16> nonzero_default;nonzero_default.fill(0xa5);
        constructed.initialize_entities(nonzero_default);
        constructed.initialize_root({1.f,1,17});
        for(unsigned i=0;i<64;++i)for(unsigned j=0x40;j<0x50;++j)
            require(constructed.entity(i)[j]==0);
        // Malformed inputs must fail before changing any byte.
        std::vector<std::uint8_t> root(okage::battle_state_size,0xa5),defaults(16,0x42);
        const auto before=root;
        for(std::size_t size:{0u,0x50u,0x805fu,0x81cfu}) {
            rejects([&]{okage::initialize_battle_entities(std::span(root).first(size),defaults);});
            require(root==before);
            rejects([&]{okage::initialize_battle_secondary(std::span(root).first(size));});require(root==before);
            rejects([&]{okage::initialize_battle_root(std::span(root).first(size),{1.f,1,17});});require(root==before);
        }
        for(unsigned size:{0u,15u,17u}) {
            defaults.resize(size);
            rejects([&]{okage::initialize_battle_entities(root,defaults);});require(root==before);
        }
        // Bytes beyond the required root extent are retained as well.
        root.resize(okage::battle_state_size+17,0x31);defaults.assign(16,0);
        okage::initialize_battle_entities(root,defaults);okage::initialize_battle_secondary(root);
        require(std::all_of(root.begin()+okage::battle_state_size,root.end(),[](auto x){return x==0x31;}));
        okage::BattleState owner;owner.bytes.fill(0xa5);owner.initialize_entities(defaults);owner.initialize_secondary();
        const auto& const_owner=owner;
        require(owner.entity(0).data()==owner.bytes.data()+0x60);
        require(const_owner.entity(63).data()==owner.bytes.data()+0x7e60);
        require(const_owner.entity(63)[0x1ff]==0x80&&owner.entity(0)[0x16e]==0xa5);
        for(auto index:{std::size_t(64),std::numeric_limits<std::size_t>::max()}) {
            rejects([&]{owner.entity(index);});rejects([&]{const_owner.entity(index);});
        }
        // The explicit default input can alias backing without being erased.
        for(unsigned i=0;i<16;++i)owner.bytes[0x60+i]=i+1;
        owner.initialize_entities(std::span(owner.bytes).subspan(0x60,16));
        for(unsigned i=0;i<64;++i)for(unsigned j=0;j<16;++j)require(owner.entity(i)[0x40+j]==j+1);
        std::cout<<"Battle state exact ranges, defaults, preservation and bounds passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
