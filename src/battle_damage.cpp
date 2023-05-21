#include "okage/battle_damage.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
std::int32_t signed_word(Bytes b,unsigned at){return std::bit_cast<std::int32_t>(word(b,at));}
std::int16_t narrow(std::uint32_t value){return std::bit_cast<std::int16_t>(std::uint16_t(value));}
std::int16_t half(Bytes b,unsigned at){return narrow(unsigned(b[at])|(unsigned(b[at+1])<<8));}
void put_half(std::span<std::uint8_t> b,unsigned at,std::int16_t value) {
    const auto bits=std::uint16_t(value);b[at]=std::uint8_t(bits);b[at+1]=std::uint8_t(bits>>8);
}
std::uint32_t product(std::int32_t a,std::int32_t b){return std::uint32_t(a)*std::uint32_t(b);}
std::int16_t divide_narrow(std::uint32_t bits,int divisor) {
    //Positive fixed divisors: signed division cannot trigger INT_MIN/-1 UB.
    return narrow(std::uint32_t(std::bit_cast<std::int32_t>(bits)/divisor));
}
unsigned event_slot(Bytes entity) {
    const int index=half(entity,0x172);
    //The source stores the increment before this first unchecked slot access.
    check(index>=0&&index<16,"Battle damage event index outside source queue storage");
    return 0x174+unsigned(index)*8;
}
const BattleActor* actor_binding(std::span<const BattleEntityBinding> bindings,int index) {
    check(index>=0&&index<64&&unsigned(index)<bindings.size(),
          "Battle damage actor binding index outside native storage");
    return bindings[unsigned(index)].actor;
}
std::int16_t category(std::int16_t type) {
    //159728 dispatches on the signed LOW halfword, unlike158e38's full word.
    switch(type) {
    case 0:return 0;
    case 1:return 1;
    case 2:case 3:case 4:
        throw std::runtime_error("Battle damage status category1597a8/159a28 is unrecovered");
    default:return -1;
    }
}
std::int16_t physical_base(std::int16_t command,Bytes source,Bytes target) {
    //159238: source TASK1ac is the live copied waza; do not reread BKDATA.
    const auto defense_product=product(half(target,0x168),signed_word(source,0x1c4));
    const auto attack_product=product(half(source,0x166),command);
    const auto attack_first=divide_narrow(attack_product,16);
    const auto attack=divide_narrow(product(attack_first,signed_word(source,0x1c0)),16);
    const auto defense=divide_narrow(defense_product,100);
    const auto selector=signed_word(source,0x1d0);
    const int row=selector<4?selector:half(source,0x174),column=half(target,0x174);
    check(row>=0&&row<4&&column>=0&&column<4,"Battle damage attribute index outside recovered table");
    //Pinned ELF2a90...aaa5,2441d8..2441f7: signed16 attribute percentages.
    constexpr std::array<std::int16_t,16> factors{
        100,50,150,100,150,100,50,100,50,150,100,100,100,100,100,100};
    return divide_narrow(product(int(attack)-int(defense),factors[unsigned(row*4+column)]),100);
}
std::int16_t physical_result(BattleState& root,Bytes source,Bytes target,std::int16_t command,
                             const std::function<std::uint32_t()>& random) {
    check(half(source,0x1fc)==200,"Battle damage action outside ordinary200 scope");
    check(signed_word(source,0x1bc)==0,"Battle damage resolver outside physical class0 scope");
    auto amount=physical_base(command,source,target);
    int critical=half(source,0x16e);
    if(signed_word(root.bytes,0x807c)==4) {
        critical=narrow(std::uint32_t(critical)+50u);
        if(critical>=100)critical=99;
    }
    const auto draw=[&](int modulus) {
        check(bool(random),"Battle damage shared RNG owner missing");
        return (std::bit_cast<std::int32_t>(random())>>5)%modulus;
    };
    if(draw(100)<critical) {
        const auto tripled=std::bit_cast<std::int32_t>(product(amount,3));
        amount=narrow(std::uint32_t(tripled/2)+10u);
    } else {
        const auto roll=draw(100);
        const auto accuracy=std::bit_cast<std::int32_t>(word(source,0x1cc)-std::uint32_t(half(target,0x170)));
        if(roll>=accuracy)return -1;
    }
    if(word(source,0x17c)==1)amount=narrow(product(amount,2));
    if(signed_word(root.bytes,0x807c)==2)amount=divide_narrow(std::uint32_t(amount),2);
    if(signed_word(root.bytes,0x807c)==1)amount=divide_narrow(product(amount,5),4);
    const int factor=draw(14)+58;
    amount=divide_narrow(product(amount,factor),64);
    if(amount<1)amount=1;
    if(amount>999)amount=999;
    if(word(target,0x184)==1)amount=divide_narrow(std::uint32_t(amount),2);
    return amount;
}
}
void battle_queue_damage_event(BattleState& root,std::span<const BattleEntityBinding> bindings,
    std::int16_t command,std::int16_t owner,std::int16_t target,
    const std::function<std::uint32_t()>& random) {
    if(owner<0&&target<0)return;
    const auto* source=actor_binding(bindings,owner);
    if(!source)return;
    check(target>=0&&target<64,"Battle damage target outside entity storage");
    auto entity=root.entity(unsigned(target));
    auto index=narrow(std::uint32_t(half(entity,0x172))+1u);
    put_half(entity,0x172,index);
    if(index>=16)put_half(entity,0x172,15);
    put_half(entity,event_slot(entity)+4,owner);
    const auto type=category(half(source->storage,0x1bc));
    put_half(entity,event_slot(entity),type);
    check(type==0,"Battle damage event outside ordinary physical category0 scope");
    //158e38 first resolves target BODY after the wrapper's owner/category writes.
    const auto* recipient=actor_binding(bindings,target);
    check(recipient!=nullptr,"Battle damage target actor is missing");
    const auto amount=physical_result(root,source->storage,recipient->storage,command,random);
    //The source reloads the queue cursor after the resolver, including its RNG.
    put_half(entity,event_slot(entity)+2,amount);
}
}
