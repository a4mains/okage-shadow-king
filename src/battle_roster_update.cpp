#include "okage/battle_roster_update.hpp"
#include "okage/battle_command_queue.hpp"
#include "okage/battle_action_setup.hpp"
#include "okage/battle_enemy_action.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

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
            const auto duration=battle_setup_action(a,c.root,*c.tables,*c.action_data);
            put(a.storage,0x1a0,std::bit_cast<std::uint32_t>(duration));
            put(a.storage,0x21c,3);put(a.storage,0x220,0,2);put(a.storage,0x19c,0);return;
        }
        if(half(a.storage,0x220)==4) {
            const auto selector=signed_word(a.storage,0x1d4);
            if(selector==0||selector==1||selector==3)
                missing("Battle party retargeting158548/158490 is unrecovered");
        }
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
        put(a.storage,0x21c,1);clear_action(id,c);
        missing("Battle party post-action status/timer reset is unrecovered");
    default:put(a.storage,0x19c,0);put(a.storage,0x194,0);return;
    }
}
void action_duration(BattleActor&) {missing("Battle roster1580d0 action duration is unrecovered");}
void stan_actor(BattleActor& stan,BattleActor* hero,BattleRosterUpdateContext& c) {
    const unsigned id=entity_id(stan,c);
    //152dc0 gets both persistent owners before checking the shared mode.
    check(c.variables.size()>=0x578,"Battle roster shared variables extent");
    if(word(c.root.bytes,0x81b8)==1){put(stan.storage,0x21c,100);return;}
    switch(word(stan.storage,0x21c)) {
    case 1:case 2:
        if(std::bit_cast<std::int16_t>(c.entry.parameters().argument1)==1) {
            c.entry.clear_argument1();
            missing("Battle Stan158548 request target construction is unrecovered");
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
        action_duration(stan);return;
    case 3:
        if(!any_target(stan,c,false)) {
            put(stan.storage,0x194,0);put(stan.storage,0x19c,0);put(stan.storage,0x21c,1);clear_action(id,c);return;
        }
        missing("Battle Stan target/action readiness1588b0 is unrecovered");
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
void enemy_actor(BattleActor& a,BattleRosterUpdateContext& c) {
    const unsigned id=entity_id(a,c);
    switch(word(a.storage,0x21c)) {
    case 1:
        if(!elapsed(a,0x194,0x198)||!charge_status(a))return;
        if(word(a.storage,0x184)!=0)put(a.storage,0x184,0);
        check(c.tables&&c.enemy_commands,"Battle enemy157b90 owned command data missing");
        battle_decode_enemy_command(a,c.root,c.entities,*c.tables,*c.enemy_commands,c.random);
        check(c.action_data,"Battle enemy1580d0 owned action data missing");
        {
            const auto duration=battle_setup_action(a,c.root,*c.tables,*c.action_data);
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
                missing("Battle enemy action grouping15b388 is unrecovered");
            case 203:put(a.storage,0x21c,10);return;
            default:put(a.storage,0x21c,8);return;
            }
        }
        default:return;
        }
    case 5:case 6:case 8:return;
    case 10:case 99:
        put(a.storage,0x21c,1);clear_action(id,c);
        missing("Battle enemy post-action status/timer reset is unrecovered");
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
    for(auto* a:enemies)if(a)enemy_actor(*a,c);
}
}
