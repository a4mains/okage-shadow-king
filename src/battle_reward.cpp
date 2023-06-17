#include "okage/battle_reward.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
std::uint32_t read(Bytes bytes,std::size_t at,unsigned size) {
    check(at<=bytes.size()&&bytes.size()-at>=size,"Battle reward read extent");
    std::uint32_t result=0;for(unsigned i=0;i<size;++i)result|=std::uint32_t(bytes[at+i])<<(i*8);
    return result;
}
std::int32_t word(Bytes bytes,std::size_t at) {return std::bit_cast<std::int32_t>(read(bytes,at,4));}
std::int16_t half(Bytes bytes,std::size_t at) {return std::bit_cast<std::int16_t>(std::uint16_t(read(bytes,at,2)));}
void put(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t value) {
    check(at<=bytes.size()&&bytes.size()-at>=2,"Battle reward write extent");
    bytes[at]=std::uint8_t(value);bytes[at+1]=std::uint8_t(value>>8);
}
const BattleEntityBinding& entity(std::span<const BattleEntityBinding> entities,int index) {
    check(index>=0&&unsigned(index)<entities.size(),"Battle reward entity owner missing");
    return entities[index];
}
void reveal(std::unique_ptr<FieldActor>& model) {
    check(bool(model),"Battle reward model owner missing");model->visible=true;
}
int currency_divisor(std::span<const BattleEntityBinding> entities) {
    unsigned count=0;
    for(unsigned i=0;i<3;++i)if(const auto* actor=entity(entities,i).actor) {
        check(!actor->stat_bindings[3].empty(),"Battle reward secondary status owner missing");
        if(actor->stat_bindings[3][0]==9)++count;
    }
    //Owned ELF244bd0: 01000000 02000000 04000000 08000000.
    constexpr int divisors[]={1,2,4,8};
    return divisors[count];
}
void experience(std::span<const BattleEntityBinding> entities,std::span<std::uint8_t> reward,std::int32_t value) {
    const auto adjusted=value<0?std::bit_cast<std::int32_t>(std::uint32_t(value)+15):value;
    const std::int32_t level=adjusted>>4;
    const std::uint32_t fraction=(std::uint32_t(value)&15)+16;
    for(unsigned i=0;i<3;++i) {
        const auto& owner=entity(entities,i);
        if(owner.record.empty())continue;
        const auto difference=std::bit_cast<std::int32_t>(std::uint32_t(level)-std::uint32_t(std::int32_t(half(owner.record,4)))+5);
        std::int32_t amount=0x54d;
        if(difference<10) {
            const auto scaled=difference<0?fraction:fraction<<unsigned(difference);
            amount=std::bit_cast<std::int32_t>((scaled*9)/80+std::uint32_t(level));
        }
        check(owner.actor,"Battle reward party BODY owner missing");
        //BODY+2 is the ordinal (TASK42), not the source character (TASK44).
        const int offset=0x26+4*int(half(owner.actor->storage,0x42));
        check(offset>=0,"Battle reward experience offset outside owner");
        const auto total=read(reward,unsigned(offset),2)+std::uint32_t(amount);
        put(reward,unsigned(offset),total);
        if(std::bit_cast<std::int16_t>(std::uint16_t(total))>=0x54d) {
            const int cap_offset=0x26+4*int(half(entity(entities,i).actor->storage,0x42));
            check(cap_offset>=0,"Battle reward experience cap offset outside owner");
            put(reward,unsigned(cap_offset),0x54d);
        }
    }
}
}
void battle_enemy_reward(BattleActor& actor,std::span<const BattleEntityBinding> entities,
                         std::span<std::uint8_t> reward,const std::function<std::uint32_t()>& random) {
    const auto enemy_record=[&]()->Bytes {
        const auto& owner=entity(entities,half(actor.storage,0x130));
        check(!owner.record.empty(),"Battle reward enemy record owner missing");return owner.record;
    };
    unsigned item_offset=0;
    if(half(actor.storage,0x178)<0)item_offset=0x60;
    else {
        check(bool(random),"Battle reward random service missing");
        const auto sample=std::bit_cast<std::int32_t>(random());
        if((sample>>5)%100<word(enemy_record(),0x58))item_offset=0x5c;
    }
    if(item_offset) {
        bool found=false;
        for(unsigned i=0;i<8;++i) {
            if(std::int32_t(half(reward,4+4*i))==word(enemy_record(),item_offset)) {
                found=true;put(reward,6+4*i,read(reward,6+4*i,2)+1);
            }
        }
        if(!found)for(unsigned i=0;i<8;++i)if(half(reward,4+4*i)==0) {
            put(reward,4+4*i,read(enemy_record(),item_offset,2));put(reward,6+4*i,1);break;
        }
        reveal(actor.box);
    } else {
        const auto previous=read(reward,2,2);
        const auto source=word(enemy_record(),0x54);
        const int divisor=currency_divisor(entities);
        check(divisor!=0,"Battle reward zero currency divisor");
        const auto quotient=std::int32_t(std::int64_t(source)/divisor);
        auto total=std::bit_cast<std::int32_t>(previous+std::uint32_t(quotient<0?1:quotient));
        if(total>59999)total=60000;
        put(reward,2,std::uint32_t(total));reveal(actor.bag);
    }
    experience(entities,reward,word(enemy_record(),0x50));
}
}
