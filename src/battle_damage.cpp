#include "okage/battle_damage.hpp"
#include <bit>
#include <stdexcept>
#include <string>

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
void put_word(std::span<std::uint8_t> b,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(value>>(8*i));
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
BattleActor* actor_binding(std::span<const BattleEntityBinding> bindings,int index) {
    check(index>=0&&index<64&&unsigned(index)<bindings.size(),
          "Battle damage actor binding index outside native storage");
    return bindings[unsigned(index)].actor;
}
[[noreturn]] void unrecovered_classifier(const char* function,std::int16_t waza,const char* helper) {
    throw std::runtime_error(std::string("Battle damage classifier ")+function+" waza "+std::to_string(waza)+
                             " side effect "+helper+" is unrecovered");
}
std::int16_t status_category(std::int16_t waza) {
    switch(std::int16_t(std::uint16_t(waza)-2u)) {
    case 0:unrecovered_classifier("1597a8",waza,"164070(0) (1fd81c)");
    case 1:return 10;
    case 2:case 0x13:return 0xd;
    case 3:unrecovered_classifier("1597a8",waza,"two-draw 14adc8/15ad58");
    case 4:case 5:case 6:case 7:case 0x19:case 0xca:return 0xb;
    case 9:return 0x11;
    case 10:return 0x12;
    case 0xb:return 0x13;
    case 0xc:return 0x14;
    case 0xd:return 0x15;
    case 0xe:return 0x16;
    case 0xf:return 0x17;
    case 0x10:return 0x18;
    case 0x11:return 0x19;
    case 0x12:return 0xe;
    case 0x17:return 0x36;
    case 0x18:unrecovered_classifier("1597a8",waza,"root8078 14aed0/14ad38");
    case 0x1a:return 0xf;
    case 0x1b:return 0x10;
    case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0xf1:return 0x24;
    case 0x77:case 0x78:case 0x79:case 0x7a:case 0x7b:case 0xf2:return 0x25;
    case 0x7c:case 0x7d:case 0x7e:case 0x7f:case 0x80:case 0xf3:return 0x26;
    case 0x81:case 0x82:case 0x83:case 0x84:case 0x85:case 0xf4:return 0x27;
    case 0x86:return 0x2d;
    case 0x87:case 0x88:return 0x2f;
    case 0x89:case 0x8a:return 0x31;
    case 0x8b:case 0x8c:return 0x33;
    case 0x8d:case 0x8e:return 0x39;
    case 0x90:case 0x91:return 0x2e;
    case 0x92:case 0x93:return 0x30;
    case 0x94:case 0x95:case 0xf5:return 0x32;
    case 0x96:case 0x97:return 0x38;
    case 0x98:case 0x99:case 0x9a:case 0x9b:case 0x9c:case 0x9d:
        unrecovered_classifier("1597a8",waza,"14ac20 root807c mode");
    case 0xec:return 0x3a;
    case 0xed:unrecovered_classifier("1597a8",waza,"14acb0 root8080");
    case 0xf6:return 0x34;
    default:return 999;
    }
}
//159a28 (class4): switch on the signed16 (waza-0x91), jump table 0x244840
//(0x5d entries, default-1).
std::int16_t recovery_category(std::int16_t waza) {
    switch(std::int16_t(std::uint16_t(waza)-0x91u)) {
    case 0:case 0x5c:return 0x2c;
    case 0xf:case 0x10:case 0x12:case 0x46:case 0x47:case 0x49:return 4;
    case 0x11:case 0x13:case 0x48:return 5;
    case 0x14:case 0x57:return 0x23;
    case 0x15:case 0x4c:case 0x4d:return 6;
    case 0x16:return 7;
    case 0x4a:unrecovered_classifier("159a28",waza,"14ad38/15ad58(25)");
    case 0x4b:unrecovered_classifier("159a28",waza,"244820 random table 14adc8/14ad38/15ad58");
    case 0x4e:return 0x1a;
    case 0x4f:return 0x1b;
    case 0x50:return 0x1c;
    case 0x51:return 0x1d;
    case 0x52:return 0x1e;
    case 0x53:return 0x1f;
    case 0x54:return 0x20;
    case 0x55:return 0x21;
    case 0x56:return 0x22;
    case 0x58:return 0x28;
    case 0x59:return 0x29;
    case 0x5a:return 0x2a;
    case 0x5b:return 0x2b;
    default:return -1;
    }
}
std::int16_t category(std::int16_t waza,std::int16_t type) {
    //159728 dispatches on the signed LOW halfword, unlike158e38's full word.
    switch(type) {
    case 0:return 0;
    case 1:return 1;
    case 2:case 3:return status_category(waza);
    case 4:return recovery_category(waza);
    default:return -1;
    }
}
std::int16_t attribute_factor(Bytes source,Bytes target) {
    const auto selector=signed_word(source,0x1d0);
    const int row=selector<4?selector:half(source,0x174),column=half(target,0x174);
    check(row>=0&&row<4&&column>=0&&column<4,"Battle damage attribute index outside recovered table");
    //Pinned ELF2a90...aaa5,2441d8..2441f7: signed16 attribute percentages.
    constexpr std::array<std::int16_t,16> factors{
        100,50,150,100,150,100,50,100,50,150,100,100,100,100,100,100};
    return factors[unsigned(row*4+column)];
}
std::int16_t physical_base(std::int16_t command,Bytes source,Bytes target) {
    //159238: source TASK1ac is the live copied waza; do not reread BKDATA.
    const auto defense_product=product(half(target,0x168),signed_word(source,0x1c4));
    const auto attack_product=product(half(source,0x166),command);
    const auto attack_first=divide_narrow(attack_product,16);
    const auto attack=divide_narrow(product(attack_first,signed_word(source,0x1c0)),16);
    const auto defense=divide_narrow(defense_product,100);
    const auto factor=attribute_factor(source,target);
    return divide_narrow(product(int(attack)-int(defense),factor),100);
}
std::int16_t magic_base(std::int16_t command,Bytes source,Bytes target) {
    const auto power=divide_narrow(product(signed_word(source,0x1c8),command),16);
    const auto factor=attribute_factor(source,target);
    return divide_narrow(product(power,factor),100);
}
std::int16_t resolve(BattleState& root,Bytes source,Bytes target,std::int16_t command,
                     const std::function<std::uint32_t()>& random) {
    const auto draw=[&](int modulus) {
        check(bool(random),"Battle damage shared RNG owner missing");
        return (std::bit_cast<std::int32_t>(random())>>5)%modulus;
    };
    const auto type=signed_word(source,0x1bc);
    std::int16_t amount;
    if(type==1) {
        amount=magic_base(command,source,target);
        const auto roll=draw(100);
        const auto accuracy=std::bit_cast<std::int32_t>(word(source,0x1cc)-std::uint32_t(half(target,0x172)));
        if(roll>=accuracy)return -1;
        //M_REDUCE: root807c mode3 halves magic only (toward zero, narrowed).
        if(signed_word(root.bytes,0x807c)==3)amount=divide_narrow(std::uint32_t(amount),2);
    } else if(type==0) {
        amount=physical_base(command,source,target);
        int critical=half(source,0x16e);
        if(signed_word(root.bytes,0x807c)==4) {
            critical=narrow(std::uint32_t(critical)+50u);
            if(critical>=100)critical=99;
        }
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
    } else return -1;
    //159118: shared tail, root807c reread at each test.
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
    auto* source=actor_binding(bindings,owner);
    if(!source)return;
    check(target>=0&&target<64,"Battle damage target outside entity storage");
    auto entity=root.entity(unsigned(target));
    auto index=narrow(std::uint32_t(half(entity,0x172))+1u);
    put_half(entity,0x172,index);
    if(index>=16)put_half(entity,0x172,15);
    put_half(entity,event_slot(entity)+4,owner);
    const auto waza=half(source->storage,0x1fc);
    const auto type=category(waza,half(source->storage,0x1bc));
    put_half(entity,event_slot(entity),type);
    if(type<0||type>=4) {
        put_half(entity,event_slot(entity)+2,half(source->storage,0x1c8));
        return;
    }
    //158e38 first resolves target BODY after the wrapper's owner/category writes.
    const auto* recipient=actor_binding(bindings,target);
    check(recipient!=nullptr,"Battle damage target actor is missing");
    const auto amount=resolve(root,source->storage,recipient->storage,command,random);
    //The source reloads the queue cursor after the resolver, including its RNG.
    put_half(entity,event_slot(entity)+2,amount);
    if(waza==1) {
        check(!source->stat_bindings[1].empty()&&source->stat_bindings[2].size()>=2,
              "Battle damage waza1 source status bindings missing");
        source->stat_bindings[1][0]=2;
        put_half(source->stat_bindings[2],0,0);
    } else if(waza==0xca) {
        put_word(source->storage,0x180,1);
    }
}
}
