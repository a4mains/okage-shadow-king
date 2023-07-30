#include "okage/battle_action_setup.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
std::int16_t half(Bytes b,unsigned at) {
    return std::bit_cast<std::int16_t>(std::uint16_t(b[at]|(unsigned(b[at+1])<<8)));
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned size=4) {
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
void number(std::span<std::uint8_t> b,unsigned at,float value){put(b,at,std::bit_cast<std::uint32_t>(value));}
unsigned choose(std::int16_t group,const BattleState& root,std::span<const BattleEntityBinding> bindings,
                const std::function<std::uint32_t()>& random) {
    const auto count=std::bit_cast<std::int32_t>(word(root.bytes,0x50));
    check(count<=64&&(count<0||bindings.size()==unsigned(count)),"Battle target entity count/binding mismatch");
    std::array<unsigned,16> candidates{};unsigned used=0;
    for(int i=0;i<count;++i) {
        const auto* actor=bindings[unsigned(i)].actor;
        if(!actor||half(actor->storage,0x46)!=group||(word(root.entity(unsigned(i)),0x1fc)&0x80000000u))continue;
        check(used<candidates.size(),"Battle157ce8 candidate list exceeds source capacity");
        candidates[used++]=unsigned(i);
    }
    if(!used)return 0;
    check(bool(random),"Battle157ce8 random owner missing");
    const auto draw=std::bit_cast<std::int32_t>(random());
    const auto index=(draw>>5)%int(used);
    check(index>=0,"Battle157ce8 RNG produced negative candidate index");
    return candidates[unsigned(index)];
}
void register_target_group(BattleActor& actor,BattleState& root,int owner,std::uint32_t flag) {
    check(owner>=0,"Battle group action signed owner outside entity storage");
    auto node=root.entity(unsigned(owner));
    put(node,0x60,unsigned(owner),2);put(node,0x62,0xffff,2);
    const auto previous=std::uint16_t(half(node,0x16c));
    const auto registrations=std::uint16_t(half(node,0x64));
    put(node,0x68,flag);put(node,0x16c,std::uint16_t(previous+1),2);
    put(node,0x64,std::uint16_t(registrations+1),2);
    const auto store_link=[&](unsigned destination,std::uint16_t previous) {
        const auto index=std::bit_cast<std::int16_t>(previous);
        check(index>=0&&index<64,"Battle group action append outside owned link storage");
        check(word(root.entity(destination),0x6c+unsigned(index)*4)==0,
              "Battle group action link contains an unnormalized guest pointer");
        root.action_links[destination][unsigned(index)]=std::uint8_t(owner);
    };
    store_link(unsigned(owner),previous);
    auto& b=actor.storage;put(b,0x140,0,2);
    const int group=half(b,0x144);
    check(group==10||group==11,"Battle158548 actor-filtered target group is unrecovered");
    //158688/158714 fixed ranges, independent of root+50 and actor pointers.
    for(unsigned target=group==10?4:0;target<(group==10?14u:3u);++target) {
        auto target_node=root.entity(target);
        if(word(target_node,0x1fc)&0x80000000u)continue;
        const int count=half(b,0x140);
        put(b,0x140,std::uint16_t(count+1),2);
        check(count>=0&&count<10,"Battle group action target storage extent");
        b[0x132+unsigned(count)]=std::uint8_t(target);
        const auto old=std::uint16_t(half(target_node,0x16c));
        put(target_node,0x16c,std::uint16_t(old+1),2);
        store_link(target,old);
    }
}
class Rounding {
    std::fenv_t saved_{};
public:
    Rounding() {
        check(std::fegetenv(&saved_)==0,"Battle action floating environment unavailable");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);throw std::runtime_error("Battle action rounding unavailable");
        }
    }
    ~Rounding(){std::fesetenv(&saved_);}
};
}
void battle_decode_command(BattleActor& actor,const BattleState& root,
    std::span<const BattleEntityBinding> bindings,const std::function<std::uint32_t()>& random) {
    auto& b=actor.storage;
    for(int i=9;i>=0;--i)b[0x132+unsigned(i)]=0xff;
    put(b,0x140,0,2);
    const auto group=half(b,0x224),action=half(b,0x226);
    const auto select=[&]{put(b,0x146,choose(group,root,bindings,random),2);};
    switch(half(b,0x222)) {
    case 0:case 1:
        put(b,0x142,half(b,0x222)==0?200:208,2);put(b,0x144,group,2);select();return;
    case 3:case 6:
        put(b,0x142,action,2);
        if(group==10){put(b,0x146,0xffff,2);put(b,0x144,11,2);}
        else {put(b,0x144,11,2);put(b,0x146,group,2);}return;
    case 4:case 7:
        put(b,0x142,action,2);put(b,0x144,group,2);
        if(group==10){put(b,0x146,0xffff,2);return;}select();return;
    case 9:put(b,0x146,0xffff,2);put(b,0x142,203,2);put(b,0x144,0xffff,2);return;
    case 10:put(b,0x142,3,2);put(b,0x146,0xffff,2);put(b,0x144,11,2);return;
    default:put(b,0x144,12,2);put(b,0x146,0xffff,2);put(b,0x142,action,2);return;
    }
}
void battle_setup_secondary_action(BattleActor& actor,const BattleActionData& data,std::int16_t action,std::int16_t mode) {
    auto& b=actor.storage;
    put(b,0x218,action,2);
    const auto secondary=data.secondary(action,mode);
    //Special selectors store the type before the descriptor; ordinary does the reverse.
    if(secondary.type!=12)put(b,0x21a,std::uint16_t(secondary.type),2);
    actor.action_secondary=secondary.descriptor;put(b,0x200,0);
    if(secondary.type==12)put(b,0x21a,std::uint16_t(secondary.type),2);
    put(b,0x208,0);
    const auto aux_first=actor.action_secondary->half(0x1a);put(b,0x210,0);number(b,0x204,float(aux_first));
    const auto aux_second=actor.action_secondary->half(0x3a);put(b,0x214,0);number(b,0x20c,float(aux_second));
}
float battle_setup_action(BattleActor& actor,BattleState& root,const GameDataTables& tables,const BattleActionData& data) {
    auto& b=actor.storage;
    const auto action=half(b,0x142);
    put(b,0x1fc,action,2);
    actor.action_record=tables.action(action);actor.action_record_id=std::uint16_t(action);
    for(unsigned i=0;i<13;++i)put(b,0x1ac+i*4,i==0||i==12?0:actor.action_record->words[i]);
    actor.action_motion=data.motion(action,half(b,0x40),half(b,0x44));put(b,0x1a8,0);
    //158328's actual dispatch argument is narrowed to signed16.
    const int type=action==3?8:half(b,0x1d4),owner=half(b,0x130);
    const auto register_target=[&](int target) {
        check(owner>=0&&target>=0,"Battle action signed target/owner outside entity storage");
        battle_register_action(root,unsigned(owner),unsigned(target),action==208?1u:0u);
    };
    switch(type) {
    case 0:case 3:
        put(b,0x140,1,2);b[0x132]=b[0x146];
        register_target(std::bit_cast<std::int8_t>(b[0x146]));break;
    case 2:
        register_target_group(actor,root,owner,action==208?1u:0u);break;
    case 5:case 6:
        b[0x132]=std::uint8_t(owner);put(b,0x140,1,2);
        register_target(std::bit_cast<std::int8_t>(b[0x132]));break;
    default:
        if(type!=1&&type!=2&&type!=4&&type!=8){put(b,0x144,12,2);put(b,0x146,0xffff,2);}
        throw std::runtime_error("Battle158548 multi-target action construction is unrecovered");
    }
    put(b,0x1e4,0);
    const auto first=actor.action_motion->half(8);put(b,0x1ec,0);number(b,0x1e0,float(first));
    const auto second=actor.action_motion->half(0x1a);put(b,0x1f4,0);number(b,0x1e8,float(second));
    const auto third=actor.action_motion->half(0x3a);put(b,0x1f8,0);number(b,0x1f0,float(third));
    int mode=0;
    if(half(b,0x40)==1) {
        const int identity=half(b,0x44);
        mode=identity>=203&&identity<=205?0:identity==207?2:1;
    }
    battle_setup_secondary_action(actor,data,half(b,0x1fc),std::int16_t(mode));
    Rounding rounding;
    volatile std::int32_t delay=std::bit_cast<std::int32_t>(word(b,0x1d8));
    volatile float converted=static_cast<float>(delay),factor=5.f;
    volatile float duration=converted*factor;
    return duration;
}
}
