#include "okage/battle_roster_update.hpp"
#include "okage/battle_command_queue.hpp"
#include "okage/battle_action_setup.hpp"
#include "okage/battle_enemy_action.hpp"
#include <bit>
#include <cmath>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void check(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=4,"Battle roster word extent");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
std::int32_t signed_word(Bytes b,unsigned at){return std::bit_cast<std::int32_t>(word(b,at));}
std::int16_t half(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=2,"Battle roster halfword extent");
    return std::bit_cast<std::int16_t>(std::uint16_t(b[at]|(unsigned(b[at+1])<<8)));
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t v,unsigned count=4) {
    check(at<=b.size()&&b.size()-at>=count,"Battle roster write extent");
    for(unsigned i=0;i<count;++i)b[at+i]=std::uint8_t(v>>(i*8));
}
int status(const BattleActor& a) {
    check(!a.stat_bindings[1].empty(),"Battle roster primary status binding missing");
    return std::bit_cast<std::int8_t>(a.stat_bindings[1][0]);
}
unsigned entity_id(const BattleActor& a,const BattleRosterUpdateContext& c) {
    const int id=half(a.storage,0x130);
    check(id>=0&&unsigned(id)<c.entities.size()&&c.entities[id].actor==&a,
          "Battle roster actor/entity identity mismatch");
    return unsigned(id);
}
void validate(const BattleRosterUpdateContext& c) {
    const auto count=word(c.root.bytes,0x50);
    check(count<=64&&c.entities.size()==count,"Battle roster entity count/binding mismatch");
}
bool elapsed(const BattleActor& a,unsigned value,unsigned limit) {
    const auto x=std::bit_cast<float>(word(a.storage,value));
    const auto y=std::bit_cast<float>(word(a.storage,limit));
    check(std::isfinite(x)&&std::isfinite(y)&&std::fpclassify(x)!=FP_SUBNORMAL&&std::fpclassify(y)!=FP_SUBNORMAL,
          "Battle roster exceptional timer comparison is unrecovered");
    return y<=x;
}
[[noreturn]] void missing(const char* message){throw std::runtime_error(message);}
void motion(BattleActor& a,unsigned selected) {
    //1546a0 stores before calling1bb908; duration follows a successful select.
    put(a.storage,0x128,selected);
    check(bool(a.model),"Battle roster model owner missing");
    a.model->select_motion(selected);
    put(a.storage,0x12c,0xbf800000);
}
bool charge_status(BattleActor& a) {
    switch(status(a)) {
    case 3:case 5:missing("Battle roster status3/5 recovery and conditional RNG helpers are unrecovered");
    case 4:if(half(a.stat_bindings[2],0)<2)return true;[[fallthrough]];
    case 6:put(a.storage,0x194,0);return false;
    default:return true;
    }
}
void clear_action(unsigned id,const BattleRosterUpdateContext& c) {
    battle_clear_action(c.root,id);
}
void self_event(BattleActor& a,BattleRosterUpdateContext& c,std::uint16_t amount,std::int16_t category) {
    const int id=half(a.storage,0x130);
    if(id<0)return;
    check(id<64,"Battle post-action event target outside root");
    auto entity=c.root.entity(unsigned(id));
    const auto next=std::uint16_t(std::uint16_t(half(entity,0x172))+1u);
    put(entity,0x172,next,2);
    if(std::bit_cast<std::int16_t>(next)>=16)put(entity,0x172,15,2);
    const auto slot=[&]() {
        const int cursor=half(entity,0x172);
        check(cursor>=0&&cursor<16,"Battle post-action event queue cursor outside storage");
        return 0x174+unsigned(cursor)*8;
    };
    put(entity,slot()+4,std::uint16_t(id),2);
    put(entity,slot(),std::uint16_t(category),2);
    put(entity,slot()+2,amount,2);
}
void primary_post_action(BattleActor& a,BattleRosterUpdateContext& c) {
    if(status(a)!=2)return; //15ab58.
    const int index=half(a.stat_bindings[2],0);
    const auto hp=signed_word(a.stat_bindings[0],0);
    //Pinned244bc0, signed32 entries. Source indexes this four-word stack copy.
    constexpr std::int32_t divisors[]={1,32,16,8};
    check(index>=0&&index<4,"Battle post-action primary counter outside divisor table");
    const auto amount=std::uint32_t(hp/divisors[index])+1u;
    self_event(a,c,std::uint16_t(amount),2);
}
void secondary_post_action(BattleActor& a,BattleRosterUpdateContext& c) {
    check(!a.stat_bindings[3].empty(),"Battle post-action secondary status binding missing");
    const int secondary=std::bit_cast<std::int8_t>(a.stat_bindings[3][0]);
    if(secondary==5) {
        const auto next=std::uint16_t(std::uint16_t(half(a.stat_bindings[4],0))+1u);
        put(a.stat_bindings[4],0,next,2);
        if(std::bit_cast<std::int16_t>(next)>=1000)put(a.stat_bindings[4],0,999,2);
    } else if(secondary==3) {
        const auto selector=signed_word(a.storage,0x1d4);
        if(selector>=0&&selector<3) {
            const auto hp=signed_word(a.stat_bindings[0],0);
            const auto adjusted=std::uint32_t(hp)+(hp<0?15u:0u);
            //SLL12/SRA16: wrap32 before extracting signed low16 of HP/16.
            const auto amount=std::bit_cast<std::int32_t>(adjusted<<12)>>16;
            self_event(a,c,std::uint16_t(amount),3);
        }
    } else if(secondary==6) {
        const auto next=std::uint16_t(std::uint16_t(half(a.storage,0x178))-1u);
        put(a.storage,0x178,next,2);
        if(std::bit_cast<std::int16_t>(next)<=0) {
            check(!a.stat_bindings[1].empty(),"Battle post-action primary status binding missing");
            a.stat_bindings[1][0]=2;
            put(a.stat_bindings[2],0,0,2);
        }
    }
}
void reset_charge_duration(BattleActor& a,int parameter) {
    const int saved=std::fegetround();
    check(saved>=0&&std::fesetround(FE_TOWARDZERO)==0,"Battle post-action rounding unavailable");
    volatile float input=float(parameter);
    volatile float difference=100.f-input;
    volatile float square=difference*difference;
    volatile float quotient=square/66.f;
    volatile float added=quotient+10.f;
    volatile float duration=added*5.f;
    std::fesetround(saved);
    put(a.storage,0x198,std::bit_cast<std::uint32_t>(float(duration)));
}
void post_action(BattleActor& a,BattleRosterUpdateContext& c,bool party) {
    put(a.storage,0x21c,1);clear_action(entity_id(a,c),c);
    if(party)secondary_post_action(a,c);
    primary_post_action(a,c);
    int parameter=half(a.storage,0x16c);
    if(signed_word(c.root.bytes,0x807c)==5&&!c.entities.empty()&&c.entities[0].actor)
        parameter=half(c.entities[0].actor->storage,0x16c);
    reset_charge_duration(a,parameter);
    if(word(a.storage,0x180)==1) {
        put(a.storage,0x180,0);put(a.storage,0x17c,0);
    }
    put(a.storage,0x19c,0);put(a.storage,0x194,0);
}
bool any_target(BattleActor& a,BattleRosterUpdateContext& c,bool status_gate) {
    const int count=half(a.storage,0x140);
    check(count<=14,"Battle roster target count exceeds authored target storage");
    bool any=false;
    for(int j=0;j<count;++j) {
        const int id=std::bit_cast<std::int8_t>(a.storage[0x132+j]);
        check(id>=0&&id<64,"Battle roster target index outside entity records");
        if(status_gate) {
            check(unsigned(id)<c.entities.size(),"Battle roster target has no registered binding");
            if(!battle_actor_incapacitated(c.entities[id].actor))any=true;
        } else if(signed_word(c.root.entity(unsigned(id)),0x1fc)>=0)any=true;
    }
    return any;
}
void retarget(BattleActor& a,BattleRosterUpdateContext& c) {
    const auto selector=signed_word(a.storage,0x1d4);
    if(selector!=0&&selector!=1&&selector!=3)return;
    const auto live=[&](unsigned i){return (word(c.root.entity(i),0x1fc)&0x80000000u)==0;};
    unsigned chosen=3;
    if(selector==0||selector==1) {
        for(unsigned i=4;i<14;++i)if(live(i))chosen=i; //movz: last live enemy.
    } else {
        for(unsigned i=0;i<3;++i) {
            const auto* owner=i<c.entities.size()?c.entities[i].actor:nullptr;
            if(owner&&status(*owner)!=6)chosen=i;
        }
    }
    if(selector==1) {
        if(chosen==3)return;
        //BODY is never null here, so the 1c7910(237170) log branch is unreachable.
        check(chosen<c.entities.size()&&c.entities[chosen].actor,
              "Battle party group retarget entity has no typed actor owner");
        put(a.storage,0x144,std::uint16_t(half(c.entities[chosen].actor->storage,0x46)),2);
        put(a.storage,0x146,0xffff,2);
        battle_register_target_group(a,c.root,c.entities,0);
    } else {
        a.storage[0x132]=std::uint8_t(chosen);put(a.storage,0x140,1,2);
        battle_register_action(c.root,entity_id(a,c),chosen,0);
    }
    put(a.storage,0x21c,3);put(a.storage,0x220,0,2);
}
void party_actor(BattleActor& a,BattleRosterUpdateContext& c) {
    const unsigned id=entity_id(a,c);
    if(signed_word(c.root.entity(id),0x1fc)<0) {
        put(a.storage,0x19c,0);put(a.storage,0x194,0);clear_action(id,c);return;
    }
    if(word(c.root.bytes,0x81b8)==1){put(a.storage,0x21c,100);return;}
    const auto state=word(a.storage,0x21c);
    switch(state) {
    case 1:
        if(!elapsed(a,0x194,0x198))return;
        if(charge_status(a)) {
            put(a.storage,0x21c,2);
            if(half(a.storage,0x220)!=3)put(a.storage,0x220,1,2);
        }
        if(word(a.storage,0x184)!=0) {
            put(a.storage,0x184,0);
            const int s=status(a);motion(a,s>1&&s<6?5:1);
        }
        return;
    case 2:
        if(half(a.storage,0x220)==3) {
            if(half(a.storage,0x44)==4&&signed_word(a.storage,0x344)!=-1)
                missing("Battle party101648 selected actor effect stop is unrecovered");
            battle_decode_command(a,c.root,c.entities,c.random);
            const auto action=half(a.storage,0x142);
            if(action>=22&&action<=24) {
                put(a.storage,0x21c,99);put(a.storage,0x340,std::uint32_t(action));
                put(a.storage,0x304,word(a.storage,0x304)+1);put(a.storage,0x220,0,2);
                put(a.storage,0x194,0);return;
            }
            if(action==208)missing("Battle party153388 combo setup is unrecovered");
            check(c.tables&&c.action_data,"Battle1580d0 owned action data missing");
            const auto duration=battle_setup_action(a,c.root,*c.tables,*c.action_data,c.entities);
            put(a.storage,0x1a0,std::bit_cast<std::uint32_t>(duration));
            put(a.storage,0x21c,3);put(a.storage,0x220,0,2);put(a.storage,0x19c,0);return;
        }
        if(half(a.storage,0x220)==4)retarget(a,c);
        return;
    case 3:case 4:
        if(!elapsed(a,0x19c,0x1a0))return;
        if(!any_target(a,c,false)) {
            clear_action(id,c);
            if(state==4)put(a.storage,0x142,200,2);
            put(a.storage,0x21c,2);put(a.storage,0x220,4,2);return;
        }
        if(a.storage[0x132]==3)return;
        if(state==4)missing("Battle party153460 combo readiness is unrecovered");
        switch(battle_action_readiness(c.root,id,Bytes(a.storage).subspan(0x132,16))) {
        case 1: {
            const auto action=half(a.storage,0x142);
            put(a.storage,0x194,0);put(a.storage,0x304,word(a.storage,0x304)+1);
            if(action==200||action==201||action==208)put(a.storage,0x21c,5);
            else if(action==203)put(a.storage,0x21c,10);
            else if(action>=35&&action<=47) {
                put(a.storage,0x21c,9);
                check(c.action_data,"Battle157ed0 owned secondary action data missing");
                battle_setup_secondary_action(a,*c.action_data,action,0);
            } else put(a.storage,0x21c,8);
            return;
        }
        case 3:clear_action(id,c);put(a.storage,0x21c,2);put(a.storage,0x220,4,2);return;
        default:return;
        }
    case 5:case 6:case 8:case 9:return;
    case 10:case 99:
        post_action(a,c,true);return;
    default:put(a.storage,0x19c,0);put(a.storage,0x194,0);return;
    }
}
void action_duration(BattleActor& actor,BattleRosterUpdateContext& c) {
    check(c.tables&&c.action_data,"Battle roster1580d0 owned action data missing");
    const auto duration=battle_setup_action(actor,c.root,*c.tables,*c.action_data,c.entities);
    //152ff8..153004: completion writes happen only after real1580d0 returns.
    put(actor.storage,0x1a0,std::bit_cast<std::uint32_t>(duration));
    put(actor.storage,0x21c,3);put(actor.storage,0x19c,0);
}
void stan_actor(BattleActor& stan,BattleActor* hero,BattleRosterUpdateContext& c) {
    const unsigned id=entity_id(stan,c);
    //152dc0 gets both persistent owners before checking the shared mode.
    check(c.variables.size()>=0x578,"Battle roster shared variables extent");
    if(word(c.root.bytes,0x81b8)==1){put(stan.storage,0x21c,100);return;}
    switch(word(stan.storage,0x21c)) {
    case 1:case 2:
        if(std::bit_cast<std::int16_t>(c.entry.parameters().argument1)==1) {
            c.entry.clear_argument1();
            battle_register_target_group(stan,c.root,c.entities,0);
            put(stan.storage,0x146,0,2);put(stan.storage,0x142,0xcf,2);put(stan.storage,0x144,10,2);
            action_duration(stan,c);return;
        }
        check(hero!=nullptr,"Battle Stan HERO owner missing");
        if(word(c.variables,0x574)==0) {
            if(half(hero->storage,0x3bc)<1)return;
            put(hero->storage,0x3bc,0,2);
            check(!c.entities.empty()&&c.entities[0].actor,"Battle Stan entity0 owner missing");
            const auto& first=*c.entities[0].actor;
            const int denominator=signed_word(first.stat_bindings[0],0);
            check(denominator!=0,"Battle Stan source division by zero");
            if(int(half(first.storage,0x164))/denominator<4)return;
            put(stan.storage,0x146,0,2);put(stan.storage,0x142,0xcd,2);put(stan.storage,0x144,10,2);
        } else if(word(hero->storage,0x340)!=0) {
            put(stan.storage,0x142,std::uint16_t(half(hero->storage,0x340)),2);
            put(hero->storage,0x340,0);put(stan.storage,0x146,0,2);put(stan.storage,0x144,10,2);
        } else {
            if(half(hero->storage,0x3bc)<7)return;
            put(hero->storage,0x3bc,0,2);put(stan.storage,0x142,0xce,2);put(stan.storage,0x144,10,2);
            int selected=half(hero->storage,0x148);put(stan.storage,0x146,selected,2);
            if(selected<4) {
                put(stan.storage,0x146,3,2);
                for(unsigned i=4;i<14;++i)
                    if(signed_word(c.root.entity(i),0x1fc)>=0)put(stan.storage,0x146,i,2);
            }
        }
        action_duration(stan,c);return;
    case 3:
        if(!any_target(stan,c,false)) {
            put(stan.storage,0x194,0);put(stan.storage,0x19c,0);put(stan.storage,0x21c,1);clear_action(id,c);return;
        }
        {
            const int ready=battle_action_readiness(c.root,id,Bytes(stan.storage).subspan(0x132,16));
            if(ready==1) {
                put(stan.storage,0x194,0);
                put(stan.storage,0x21c,half(stan.storage,0x142)==206?9u:8u);
                put(stan.storage,0x304,word(stan.storage,0x304)+1);
            } else if(ready==2||ready==3) {
                put(stan.storage,0x194,0);put(stan.storage,0x19c,0);
                put(stan.storage,0x21c,1);clear_action(id,c);
            }
            return;
        }
    case 5:case 6:case 8:case 9:return;
    case 99:put(stan.storage,0x21c,1);clear_action(id,c);[[fallthrough]];
    default:put(stan.storage,0x19c,0);put(stan.storage,0x194,0);return;
    }
}
void ready_queue(std::span<BattleActor* const> ordinary,BattleRosterUpdateContext& c) {
    bool first=false;
    const auto enqueue=[&](BattleActor& a,BattleRosterSound sound) {
        battle_command_push(c.root,half(a.storage,0x130),0);
        check(bool(c.sound),"Battle roster audio owner missing after command enqueue");c.sound(sound);
    };
    for(auto* a:ordinary) {
        if(battle_actor_incapacitated(a)||word(a->storage,0x21c)!=2||half(a->storage,0x220)!=1)continue;
        put(a->storage,0x220,2,2);
        if(first)break;
        first=true;
        if(half(a->record,0x26)!=101&&battle_command_available(c.root)!=0) {
            enqueue(*a,BattleRosterSound::Ready);return;
        }
    }
    if(!(std::uint32_t(c.pad_pressed)&c.selection_buttons))return;
    for(auto* a:ordinary) {
        if(!battle_actor_incapacitated(a)&&word(a->storage,0x21c)==2&&half(a->storage,0x220)==2&&
           half(a->record,0x26)!=101&&battle_command_available(c.root)!=0) {
            enqueue(*a,BattleRosterSound::Cycle);return;
        }
    }
}
bool grouping_ready(const BattleState& root,int owner,int target) {
    check(owner>=0&&owner<64,"Battle enemy grouping owner outside root");
    const auto head=root.action_links[unsigned(owner)][0];
    if(!head)return false;
    check(target>=0&&target<64,"Battle enemy grouping target outside root");
    if(!root.action_links[unsigned(target)][0])return false;
    return half(root.entity(*head),0x60)==owner;
}
void group_enemies(std::span<BattleActor* const> enemies,BattleRosterUpdateContext& c,int target) {
    unsigned grouped=0;
    for(auto* candidate:enemies) {
        //15b388 skips every candidate field once two others have enrolled.
        if(grouped>=2)continue;
        check(candidate!=nullptr,"Battle enemy grouping source slot has no actor");
        auto& a=*candidate;
        if(word(a.storage,0x21c)!=3)continue;
        const auto action=std::uint16_t(half(a.storage,0x142));
        if(action!=200&&action!=201)continue;
        const int first=std::bit_cast<std::int8_t>(a.storage[0x132]);
        if(first!=target)continue;
        if(!grouping_ready(c.root,half(a.storage,0x130),first))continue;
        ++grouped;
        const int id=half(a.storage,0x130);
        put(a.storage,0x21c,5);
        put(c.root.entity(unsigned(id)),0x66,grouped,2); //158aa8 follows state.
    }
}
void enemy_actor(BattleActor& a,BattleRosterUpdateContext& c,std::span<BattleActor* const> enemies) {
    const unsigned id=entity_id(a,c);
    switch(word(a.storage,0x21c)) {
    case 1:
        if(!elapsed(a,0x194,0x198)||!charge_status(a))return;
        if(word(a.storage,0x184)!=0)put(a.storage,0x184,0);
        check(c.tables&&c.enemy_commands,"Battle enemy157b90 owned command data missing");
        battle_decode_enemy_command(a,c.root,c.entities,*c.tables,*c.enemy_commands,c.random);
        check(c.action_data,"Battle enemy1580d0 owned action data missing");
        {
            const auto duration=battle_setup_action(a,c.root,*c.tables,*c.action_data,c.entities);
            put(a.storage,0x1a0,std::bit_cast<std::uint32_t>(duration));
            put(a.storage,0x21c,3);put(a.storage,0x19c,0);return;
        }
    case 3:
        if(!elapsed(a,0x19c,0x1a0))return;
        if(!any_target(a,c,true)) {
            put(a.storage,0x194,0);put(a.storage,0x19c,0);put(a.storage,0x21c,1);clear_action(id,c);return;
        }
        switch(battle_action_readiness(c.root,id,Bytes(a.storage).subspan(0x132,16))) {
        case 0:return;
        case 2:case 3:
            put(a.storage,0x21c,1);clear_action(id,c);return; //Timers survive.
        case 1: {
            put(c.root.bytes,0xc6+id*0x200,0,2); //158aa8, before state selection.
            switch(half(a.storage,0x142)) {
            case 200:case 201:
                put(a.storage,0x21c,half(a.storage,0x142)==200?5:6);
                group_enemies(enemies,c,std::bit_cast<std::int8_t>(a.storage[0x132]));return;
            case 203:put(a.storage,0x21c,10);return;
            default:put(a.storage,0x21c,8);return;
            }
        }
        default:return;
        }
    case 5:case 6:case 8:return;
    case 10:case 99:
        post_action(a,c,false);return;
    default:put(a.storage,0x19c,0);put(a.storage,0x194,0);return;
    }
}
}
void battle_party_roster_update(std::span<BattleActor* const> ordinary,BattleActor& stan,BattleRosterUpdateContext& c) {
    validate(c);check(ordinary.size()<=3,"Battle ordinary party count outside source slots");
    for(auto* a:ordinary){check(a!=nullptr&&!a->enemy,"Battle ordinary party owner missing/type mismatch");entity_id(*a,c);}
    check(!stan.enemy,"Battle Stan owner type mismatch");entity_id(stan,c);
    for(auto* a:ordinary)party_actor(*a,c);
    stan_actor(stan,ordinary.empty()?nullptr:ordinary[0],c);
    if(word(c.root.bytes,0x81b4)==0)ready_queue(ordinary,c);
}
void battle_enemy_roster_update(std::span<BattleActor* const> enemies,BattleRosterUpdateContext& c) {
    validate(c);
    for(auto* a:enemies)if(a){check(a->enemy,"Battle enemy owner type mismatch");entity_id(*a,c);}
    for(auto* a:enemies)if(a)enemy_actor(*a,c,enemies);
}
}
