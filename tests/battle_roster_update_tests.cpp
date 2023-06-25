#include "okage/battle_roster_update.hpp"
#include "okage/battle_command_queue.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value,const char* message,unsigned at=__builtin_LINE()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at));
}
template<class F> void rejects(F action,const char* expected) {
    try {action();}catch(const std::runtime_error& e){require(std::string(e.what()).find(expected)!=std::string::npos,"Wrong guard");return;}
    throw std::runtime_error("Expected explicit branch rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned count=4) {
    for(unsigned i=0;i<count;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
unsigned word(okage::Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
unsigned half(okage::Bytes b,unsigned at){return b[at]|(unsigned(b[at+1])<<8);}
void number(std::span<std::uint8_t> b,unsigned at,float value){put(b,at,std::bit_cast<std::uint32_t>(value));}
struct Fixture {
    okage::BattleState root;
    okage::BattleEntryContext entry;
    std::array<std::uint8_t,0x1000> variables{};
    std::array<okage::BattleActor,5> actors;
    std::array<std::array<std::uint8_t,0x50>,5> records{};
    std::array<okage::BattleEntityBinding,5> entities;
    std::array<okage::BattleActor*,3> party;
    std::array<okage::BattleActor*,1> enemies;
    std::vector<okage::BattleRosterSound> sounds;
    unsigned draws=0;
    okage::BattleRosterUpdateContext context;
    Fixture():context{root,entry,variables,entities,0,0x40,
        [&](auto event){sounds.push_back(event);},[&]{++draws;return 123u;}} {
        root.initialize_root({1,1,17});put(root.bytes,0x50,5);
        for(unsigned i=0;i<5;++i) {
            auto& a=actors[i];a.entity_id=i;a.enemy=i==4;a.record=records[i];records[i][0]=1;
            a.stat_bindings={a.record.subspan(8,4),a.record.subspan(0,1),a.record.subspan(0x4a,2),
                a.record.subspan(1,1),a.record.subspan(0x4c,2),a.record.subspan(0x4e,2)};
            put(a.storage,0x130,i,2);put(a.storage,0x21c,1);number(a.storage,0x194,2);number(a.storage,0x198,4);
            put(root.entity(i),0x1fc,0);entities[i]={&a,a.record,i==4?1u:0u};
            // Six real script entries; selecting a motion must defer dispatch.
            std::vector<std::uint8_t> motion(80);put(motion,0,0x544d58);put(motion,4,48);
            put(motion,16,0xffffffff);put(motion,24,6);
            for(unsigned j=0;j<6;++j)put(motion,32+j*8,0x00a00002);
            okage::ModelFile model;model.models.emplace_back();
            a.model=std::make_unique<okage::FieldActor>(std::move(model),motion,okage::TextureAnimation{},true,true);
        }
        for(unsigned i=0;i<3;++i)party[i]=&actors[i];enemies[0]=&actors[4];
    }
    void update(){okage::battle_party_roster_update(party,actors[3],context);}
    void enemy(){okage::battle_enemy_roster_update(enemies,context);}
    void full(unsigned i){number(actors[i].storage,0x194,4);}
};
void waiting_and_ready() {
    Fixture f;const auto root=f.root.bytes;std::array<std::array<std::uint8_t,0x3e0>,5> before;
    for(unsigned i=0;i<5;++i)before[i]=f.actors[i].storage;
    f.update();f.enemy();require(f.root.bytes==root&&f.draws==0&&f.sounds.empty(),"Waiting callbacks preserve queue and RNG");
    for(unsigned i=0;i<5;++i)require(f.actors[i].storage==before[i],"Waiting predicates have no stores or clock steps");
    f.full(0);const auto script=f.actors[0].model->animation().script_state();f.update();
    require(word(f.actors[0].storage,0x21c)==2&&half(f.actors[0].storage,0x220)==2,"Full charge becomes command ready");
    require(word(f.root.bytes,0x81a4)==1&&word(f.root.bytes,0x80ac)==0&&word(f.root.bytes,0x80b0)==0,
            "First command uses source stack slot1 and actual entity ID");
    require(!okage::battle_command_available(f.root)&&f.sounds==std::vector{okage::BattleRosterSound::Ready},"Queue publication precedes ready sound");
    require(f.actors[0].model->animation().script_state()==script&&f.draws==0,"Roster does not tick scripts or consume ordinary RNG");
    f.update();require(word(f.root.bytes,0x81a4)==1&&f.sounds.size()==1,"No unconditional repeated enqueue");
}
void ordered_selection() {
    Fixture f;for(unsigned i=0;i<3;++i)f.full(i);put(f.root.bytes,0x81a8,0);
    f.update();require(half(f.actors[0].storage,0x220)==2&&half(f.actors[1].storage,0x220)==2&&half(f.actors[2].storage,0x220)==1,
                       "Second ready actor is marked in branch delay slot before break");
    require(f.sounds.empty(),"Unavailable queue produces no sound");
    // All already marked: unrelated pad bit does not select; authored selection bit does.
    put(f.actors[2].storage,0x220,2,2);put(f.root.bytes,0x81a8,1);f.context.pad_pressed=0x80;f.update();
    require(word(f.root.bytes,0x81a4)==0,"Only source mapping mask triggers manual selection");
    put(f.actors[0].record,0x26,101,2);f.context.pad_pressed=0x40;f.update();
    require(word(f.root.bytes,0x80ac)==1&&f.sounds==std::vector{okage::BattleRosterSound::Cycle},"Manual selection skips charge sentinel101 and uses cycle service");
    Fixture locked;locked.full(0);put(locked.root.bytes,0x81b4,1);locked.update();
    require(word(locked.actors[0].storage,0x21c)==2&&half(locked.actors[0].storage,0x220)==1&&locked.sounds.empty(),
            "Root81b4 gates only selection, after actor readiness mutation");
    Fixture first_sentinel;first_sentinel.full(0);first_sentinel.full(1);put(first_sentinel.actors[0].record,0x26,101,2);
    first_sentinel.update();require(first_sentinel.sounds.empty()&&half(first_sentinel.actors[1].storage,0x220)==2,
            "First marked charge sentinel prevents automatically selecting second candidate");
}
void status_motion_and_failure_order() {
    Fixture f;f.full(0);f.records[0][0]=4;put(f.records[0],0x4a,2,2);put(f.actors[0].storage,0x184,1);
    const auto script=f.actors[0].model->animation().script_state();f.update();
    require(word(f.actors[0].storage,0x194)==0&&word(f.actors[0].storage,0x21c)==1,"Status4 counter>=2 resets charge without readiness");
    require(word(f.actors[0].storage,0x184)==0&&f.actors[0].model->motion()==5&&word(f.actors[0].storage,0x12c)==0xbf800000,
            "Motion request runs even when charge-status returns false");
    require(f.actors[0].model->animation().script_state()!=script,"Motion selector changes deferred script selection");
    Fixture recovering;recovering.full(0);recovering.records[0][0]=4;put(recovering.records[0],0x4a,0xffff,2);recovering.update();
    require(recovering.sounds.size()==1,"Signed negative status4 counter is eligible");
    Fixture dead;dead.full(0);dead.records[0][0]=6;dead.update();require(word(dead.actors[0].storage,0x194)==0&&dead.sounds.empty(),"Status6 consumes no RNG and clears charge");
    for(unsigned status:{3u,5u}) {
        Fixture g;g.full(0);g.records[0][0]=status;const auto before=g.actors[0].storage;
        rejects([&]{g.update();},"status3/5");require(g.draws==0&&g.actors[0].storage==before,"Unrecovered status helper guards before RNG and writes");
    }
    Fixture audio;audio.full(0);audio.context.sound=[&](auto){require(word(audio.root.bytes,0x81a4)==1,"Source enqueue before audio");throw std::runtime_error("sound failure");};
    rejects([&]{audio.update();},"sound failure");require(word(audio.root.bytes,0x81a8)==0,"Audio failure retains preceding source queue stores");
    Fixture selected;selected.full(0);put(selected.actors[0].storage,0x220,3,2);put(selected.root.bytes,0x81b4,1);selected.update();
    require(half(selected.actors[0].storage,0x220)==3,"Existing command selection3 survives readiness");
    rejects([&]{selected.update();},"1580d0");
}
void stan_context_and_enemy_guards() {
    Fixture f;f.entry.reset(1,0xaabb,0xccdd,0xeeff);const auto before=f.entry.bytes();
    rejects([&]{f.update();},"158548");
    for(unsigned i=0;i<before.size();++i)require(f.entry.bytes()[i]==((i==0x30||i==0x31)?0:before[i]),"Stan consumes persistent+30 only");
    Fixture half_value;half_value.entry.reset(0xffff,7,8,9);half_value.update();require(half_value.entry.parameters().argument1==0xffff,"Request compares signed halfword to exactly1");
    Fixture counter;put(counter.actors[0].storage,0x3bc,1,2);put(counter.actors[0].stat_bindings[0],0,100);put(counter.actors[0].storage,0x164,399,2);
    counter.update();require(half(counter.actors[0].storage,0x3bc)==0&&counter.sounds.empty(),"HERO trigger consumed before ratio<4 return");
    put(counter.actors[0].storage,0x3bc,1,2);put(counter.actors[0].storage,0x164,400,2);
    rejects([&]{counter.update();},"1580d0");require(half(counter.actors[3].storage,0x142)==0xcd&&half(counter.actors[3].storage,0x144)==10,"Ratio boundary reaches real action dependency");
    Fixture alternate;put(alternate.variables,0x574,1);put(alternate.actors[0].storage,0x340,0x1234);
    rejects([&]{alternate.update();},"1580d0");require(word(alternate.actors[0].storage,0x340)==0&&half(alternate.actors[3].storage,0x142)==0x1234,"Shared variables574 branch consumes queued HERO command");
    Fixture enemy;enemy.full(4);put(enemy.actors[4].storage,0x184,1);
    rejects([&]{enemy.enemy();},"157b90");require(word(enemy.actors[4].storage,0x184)==0&&word(enemy.actors[4].storage,0x21c)==1&&enemy.draws==0,"Enemy full charge reaches command selection after source clear");
    Fixture end;put(end.root.bytes,0x81b8,1);end.update();
    for(unsigned i=0;i<4;++i)require(word(end.actors[i].storage,0x21c)==100,"End-state getter changes each party and Stan state before switch");
    require(end.sounds.empty(),"End state does not invent queue command");
}
void cleanup_and_source_defaults() {
    Fixture f;put(f.root.entity(0),0x1fc,0x80000000);put(f.root.entity(0),0x60,7,2);
    put(f.root.entity(0),0x62,9,2);put(f.root.entity(0),0x68,0x12345678);
    //158868 invalidates descriptors; it does not clear later compaction inputs.
    put(f.root.entity(0),0x6c,0x89abcdef);put(f.root.entity(0),0x16c,1,2);
    number(f.actors[0].storage,0x19c,8);f.update();
    require(word(f.actors[0].storage,0x194)==0&&word(f.actors[0].storage,0x19c)==0,"Inactive entity clears timers");
    require(half(f.root.entity(0),0x60)==0xffff&&half(f.root.entity(0),0x62)==0xffff&&word(f.root.entity(0),0x68)==0,"158868 exact descriptor invalidation");
    require(word(f.root.entity(0),0x6c)==0x89abcdef&&half(f.root.entity(0),0x16c)==1,"Cleanup preserves action list ownership and count");
    Fixture defaults;put(defaults.actors[0].storage,0x21c,7);number(defaults.actors[0].storage,0x19c,9);
    put(defaults.actors[4].storage,0x21c,2);number(defaults.actors[4].storage,0x19c,9);
    defaults.update();defaults.enemy();
    require(word(defaults.actors[0].storage,0x194)==0&&word(defaults.actors[0].storage,0x19c)==0&&
            word(defaults.actors[4].storage,0x194)==0&&word(defaults.actors[4].storage,0x19c)==0,"Source default states clear timers instead of unconditional waits");
    Fixture untouched;put(untouched.actors[0].storage,0x21c,5);put(untouched.actors[4].storage,0x21c,6);
    auto party=untouched.actors[0].storage,enemy=untouched.actors[4].storage;untouched.update();untouched.enemy();
    require(untouched.actors[0].storage==party&&untouched.actors[4].storage==enemy,"Source action-owned states return without altering timers");
}
void target_gates() {
    Fixture f;put(f.actors[0].storage,0x21c,3);put(f.actors[0].storage,0x140,0,2);
    number(f.actors[0].storage,0x19c,0);number(f.actors[0].storage,0x1a0,1);f.update();
    require(word(f.actors[0].storage,0x21c)==3,"Party action timer gates target scan");
    number(f.actors[0].storage,0x19c,1);f.update();
    require(word(f.actors[0].storage,0x21c)==2&&half(f.actors[0].storage,0x220)==4&&half(f.root.entity(0),0x60)==0xffff,
            "No target returns party to retarget state and invalidates descriptor");
    put(f.actors[0].storage,0x1d4,0xffffffff);f.update();
    require(word(f.actors[0].storage,0x21c)==2,"Unselected retarget class returns according to source");
    Fixture stan;put(stan.actors[3].storage,0x21c,3);number(stan.actors[3].storage,0x1a0,999);
    stan.update();require(word(stan.actors[3].storage,0x21c)==1&&word(stan.actors[3].storage,0x194)==0,"Stan target scan has no action-timer gate");
    Fixture enemy;put(enemy.actors[4].storage,0x21c,3);put(enemy.actors[4].storage,0x140,1,2);enemy.actors[4].storage[0x132]=0;
    enemy.records[0][0]=6;enemy.enemy();
    require(word(enemy.actors[4].storage,0x21c)==1&&word(enemy.actors[4].storage,0x194)==0,"Enemy targets use incapacity predicate rather than entity flags");
    Fixture sentinel;put(sentinel.actors[0].storage,0x21c,3);put(sentinel.actors[0].storage,0x140,1,2);sentinel.actors[0].storage[0x132]=3;
    sentinel.update();require(word(sentinel.actors[0].storage,0x21c)==3,"Target3 has its distinct source return before readiness helper");
    sentinel.actors[0].storage[0x132]=4;sentinel.update();
    require(word(sentinel.actors[0].storage,0x21c)==3,"Absent source graph head waits without inventing a registration");
}
void enemy_action_readiness() {
    Fixture f;auto& actor=f.actors[4];put(actor.storage,0x21c,3);
    put(actor.storage,0x140,1,2);actor.storage[0x132]=0;
    put(actor.storage,0x194,0x42f60000);put(actor.storage,0x19c,0x41a00000);
    put(actor.storage,0x1a0,0x41200000);put(actor.storage,0x142,200,2);
    //Earlier party action owns both heads, so enemy must defer without RNG.
    okage::battle_register_action(f.root,0,4,0);
    const auto before=actor.storage;f.enemy();
    require(actor.storage==before&&f.draws==0,"Enemy waits on actual conflicting graph head without resetting timers");
    //Null owner head yields2; state reset precedes invalidation; timers survive.
    f.root.action_links[4][0].reset();f.enemy();
    require(word(actor.storage,0x21c)==1&&word(actor.storage,0x194)==0x42f60000&&word(actor.storage,0x19c)==0x41a00000,
            "Readiness2 clears action but preserves both timers");
    put(actor.storage,0x21c,3);f.root.action_links[4][0]=4;f.root.action_links[0][0]=4;
    put(f.root.bytes,0xc0+4*0x200,4,2); //Embedded node signed owner.
    put(f.root.bytes,0xc6+4*0x200,7,2);
    f.enemy();
    require(word(actor.storage,0x21c)==5&&half(f.root.bytes,0xc6+4*0x200)==0,
            "Successful enemy readiness clears node flag and selects state before grouping");
    for(const auto command:{203u,202u}) {
        put(actor.storage,0x21c,3);put(actor.storage,0x142,command,2);f.enemy();
        require(word(actor.storage,0x21c)==(command==203?10u:8u),"Enemy action selects source state without extra timer writes");
    }
}
struct GroupFixture:Fixture {
    std::array<okage::BattleActor*,4> group;
    GroupFixture() {
        group={&actors[4],&actors[0],&actors[1],&actors[2]};
        for(unsigned id:{0u,1u,2u,4u}) {
            auto& a=actors[id];a.enemy=true;entities[id].kind=1;
            put(a.storage,0x21c,3);put(a.storage,0x142,200,2);a.storage[0x132]=3;
            put(a.storage,0x140,1,2);number(a.storage,0x19c,1);number(a.storage,0x1a0,1000);
            put(root.entity(id),0x60,id,2);put(root.entity(id),0x66,0xabcd,2);
            root.action_links[id][0]=id;
        }
        number(actors[4].storage,0x19c,1000);root.action_links[3][0]=4;
    }
    void run(){okage::battle_enemy_roster_update(group,context);}
};
void enemy_grouping() {
    for(unsigned action:{200u,201u}) {
        GroupFixture f;put(f.actors[4].storage,0x142,action,2);put(f.actors[0].storage,0x142,201,2);
        //Grouping does not run candidate timer/count/status/active-flag tests.
        number(f.actors[0].storage,0x19c,std::numeric_limits<float>::quiet_NaN());
        put(f.actors[0].storage,0x140,0,2);f.records[0][0]=6;put(f.root.entity(0),0x1fc,0x80000000);
        const auto before0=f.actors[0].storage,before1=f.actors[1].storage,before2=f.actors[2].storage;
        f.run();
        require(word(f.actors[4].storage,0x21c)==(action==200?5u:6u)&&half(f.root.entity(4),0x66)==0,"Initiator state and group ordinal0");
        require(word(f.actors[0].storage,0x21c)==5&&word(f.actors[1].storage,0x21c)==5&&
                half(f.root.entity(0),0x66)==1&&half(f.root.entity(1),0x66)==2,"Enroll two in source order; grouped201 also becomes5");
        for(unsigned i=0;i<before0.size();++i)if(i<0x21c||i>=0x220) {
            require(f.actors[0].storage[i]==before0[i]&&f.actors[1].storage[i]==before1[i],"Grouping preserves candidate timers, descriptors, and other fields");
        }
        require(f.actors[2].storage==before2&&half(f.root.entity(2),0x66)==0xabcd,"Third eligible candidate remains pending");
        require(f.draws==0&&f.sounds.empty(),"Grouping consumes no RNG or sound");
    }
    GroupFixture order;order.group={&order.actors[1],&order.actors[4],&order.actors[0],&order.actors[2]};order.run();
    require(half(order.root.entity(1),0x66)==1&&half(order.root.entity(0),0x66)==2,"Scan includes earlier parent slots, not only following initiator");
    for(unsigned action:{199u,202u,0xffffu}) {
        GroupFixture f;put(f.actors[0].storage,0x142,action,2);f.run();
        require(word(f.actors[0].storage,0x21c)==3&&half(f.root.entity(1),0x66)==1&&half(f.root.entity(2),0x66)==2,"Unsigned16 action filter accepts only200/201");
    }
    GroupFixture target;target.actors[0].storage[0x132]=0xff;target.run();
    require(word(target.actors[0].storage,0x21c)==3&&half(target.root.entity(1),0x66)==1,"Signed first-target mismatch skips candidate");
    GroupFixture state;put(state.actors[0].storage,0x21c,6);state.run();
    require(word(state.actors[0].storage,0x21c)==6&&half(state.root.entity(1),0x66)==1,"Only state3 enrolls");
}
void enemy_grouping_graph_and_failure() {
    GroupFixture owner;owner.root.action_links[0][0].reset();owner.run();
    require(word(owner.actors[0].storage,0x21c)==3&&half(owner.root.entity(1),0x66)==1,"Missing owner head prevents grouping");
    GroupFixture wrong;wrong.root.action_links[0][0]=2;wrong.run();
    require(word(wrong.actors[0].storage,0x21c)==3&&half(wrong.root.entity(1),0x66)==1,"Owner head descriptor must name candidate");
    GroupFixture alias;alias.root.action_links[0][0]=2;put(alias.root.entity(2),0x60,0,2);alias.run();
    require(word(alias.actors[0].storage,0x21c)==5&&half(alias.root.entity(0),0x66)==1&&
            word(alias.actors[2].storage,0x21c)==3,"Head descriptor contents, not typed row identity, decide owner match");
    GroupFixture null_after;null_after.group[3]=nullptr;null_after.run();
    require(half(null_after.root.entity(0),0x66)==1&&half(null_after.root.entity(1),0x66)==2,"After two enrollments source reads no further actor fields");
    GroupFixture null_before;null_before.group={&null_before.actors[4],&null_before.actors[0],nullptr,&null_before.actors[1]};
    rejects([&]{null_before.run();},"source slot");
    require(word(null_before.actors[4].storage,0x21c)==5&&half(null_before.root.entity(4),0x66)==0&&
            word(null_before.actors[0].storage,0x21c)==5&&half(null_before.root.entity(0),0x66)==1&&
            word(null_before.actors[1].storage,0x21c)==3,"Unsupported null slot preserves leader and earlier enrollment writes");
    GroupFixture corrupt;corrupt.root.action_links[1][0]=255;
    rejects([&]{corrupt.run();},"entity");
    require(word(corrupt.actors[0].storage,0x21c)==5&&half(corrupt.root.entity(0),0x66)==1&&word(corrupt.actors[1].storage,0x21c)==3,"Invalid later descriptor fails after earlier grouping prefix");
}
void post_action_timer_and_flags() {
    struct Timer {int speed;unsigned bits;};
    //154600: independently evaluated binary32 toward-zero at each operation.
    const Timer timers[]={{0,0x4449e4d8},{34,0x43be0000},{99,0x42484d92},
        {100,0x42480000},{-32768,0x4c9c199a},{32767,0x4c9a3256}};
    for(bool enemy:{false,true})for(unsigned state:{10u,99u})for(const auto t:timers) {
        Fixture f;const unsigned id=enemy?4:0;auto& a=f.actors[id];
        put(a.storage,0x21c,state);put(a.storage,0x16c,std::uint32_t(t.speed),2);
        put(a.storage,0x180,1);put(a.storage,0x17c,7);put(a.storage,0x184,9);
        number(a.storage,0x194,22);number(a.storage,0x19c,23);number(a.storage,0x1a4,.5f);
        put(f.root.entity(id),0x60,200,2);put(f.root.entity(id),0x62,4,2);put(f.root.entity(id),0x68,1);
        put(f.root.entity(id),0x16c,1,2);f.root.action_links[id][0]=id;
        const auto script=a.model->animation().script_state();
        const int round=std::fegetround();std::fesetround(FE_UPWARD);
        if(enemy)f.enemy();else f.update();
        require(std::fegetround()==FE_UPWARD,"Post-action timer restores caller rounding");std::fesetround(round);
        require(word(a.storage,0x21c)==1&&word(a.storage,0x198)==t.bits,"Source post-action timer/state reset");
        require(word(a.storage,0x194)==0&&word(a.storage,0x19c)==0,"Both elapsed timers reset");
        require(word(a.storage,0x180)==0&&word(a.storage,0x17c)==0&&word(a.storage,0x184)==9,"Only exact flag1 clears paired flags");
        require(word(a.storage,0x1a4)==std::bit_cast<unsigned>(.5f),"Reset does not overwrite constructor multiplier");
        require(half(f.root.entity(id),0x60)==0xffff&&half(f.root.entity(id),0x62)==0xffff&&word(f.root.entity(id),0x68)==0,"Post-action invalidates action descriptor");
        require(half(f.root.entity(id),0x16c)==1&&f.root.action_links[id][0]==id,"Reset retains graph list/count");
        require(f.draws==0&&f.sounds.empty()&&a.model->animation().script_state()==script,"No RNG, sound, or motion on reset");
    }
    for(unsigned flag:{0u,2u,0xffffffffu}) {
        Fixture f;put(f.actors[0].storage,0x21c,99);put(f.actors[0].storage,0x180,flag);put(f.actors[0].storage,0x17c,11);f.update();
        require(word(f.actors[0].storage,0x180)==flag&&word(f.actors[0].storage,0x17c)==11,"Non1 paired flags survive");
    }
    for(bool enemy:{false,true}) {
        Fixture f;const unsigned id=enemy?4:1;auto& a=f.actors[id];
        put(a.storage,0x21c,99);put(a.storage,0x16c,34,2);put(f.actors[0].storage,0x16c,100,2);put(f.root.bytes,0x807c,5);
        if(enemy)f.enemy();else f.update();require(word(a.storage,0x198)==0x42480000,"Mode5 borrows entity0 signed speed");
        put(a.storage,0x21c,99);f.entities[0].actor=nullptr;
        if(enemy)f.enemy();else okage::battle_party_roster_update(std::span(f.party).subspan(1),f.actors[3],f.context);
        require(word(a.storage,0x198)==0x43be0000,"Mode5 absent entity0 uses own speed");
    }
}
void post_action_secondary() {
    for(unsigned old:{998u,999u,32767u,65535u}) {
        Fixture f;auto& a=f.actors[0];put(a.storage,0x21c,99);f.records[0][1]=5;put(f.records[0],0x4c,old,2);f.update();
        const unsigned expected=old==998||old==999?999:old==32767?32768:0;
        require(half(f.records[0],0x4c)==expected,"Secondary5 narrows before signed1000 cap");
    }
    for(int selector:{-1,0,1,2,3})for(int hp:{160,-17,-1,std::numeric_limits<int>::max(),std::numeric_limits<int>::min()}) {
        Fixture f;auto& a=f.actors[0];put(a.storage,0x21c,99);f.records[0][1]=3;
        put(a.storage,0x1d4,std::uint32_t(selector));put(f.records[0],8,std::uint32_t(hp));f.update();
        const bool queued=selector>=0&&selector<3;
        require(half(f.root.entity(0),0x172)==unsigned(queued),"Secondary3 selector is signed range0..2");
        if(queued) {
            const unsigned amount=hp==160?10:hp==-17||hp==std::numeric_limits<int>::max()?65535:0;
            require(half(f.root.entity(0),0x17c)==3&&half(f.root.entity(0),0x17e)==amount&&half(f.root.entity(0),0x180)==0,"Secondary3 self-event command/sign/narrowing");
        }
    }
    for(unsigned old:{2u,1u,0u,32768u}) {
        Fixture f;auto& a=f.actors[0];put(a.storage,0x21c,99);f.records[0][1]=6;
        put(a.storage,0x178,old,2);put(f.records[0],0x4a,3,2);put(f.records[0],8,100);f.update();
        const bool exhausted=old==1||old==0;
        require(half(a.storage,0x178)==std::uint16_t(old-1),"Secondary6 counter wraps16 before signed test");
        require(f.records[0][0]==(exhausted?2:1)&&half(f.records[0],0x4a)==(exhausted?0:3),"Exhaustion installs primary2/counter0");
        require(half(f.root.entity(0),0x172)==unsigned(exhausted),"Primary2 producer runs after secondary6 transition");
        if(exhausted)require(half(f.root.entity(0),0x17c)==2&&half(f.root.entity(0),0x17e)==101,"New primary2 queues fullHP+1");
    }
    Fixture enemy;put(enemy.actors[4].storage,0x21c,99);enemy.actors[4].stat_bindings[3]={};enemy.actors[4].stat_bindings[4]={};enemy.enemy();
    require(word(enemy.actors[4].storage,0x21c)==1&&half(enemy.root.entity(4),0x172)==0,"Enemy omits party secondary-status block");
}
void post_action_primary_queue() {
    const unsigned expected[]={161,6,11,21};
    for(bool enemy:{false,true})for(unsigned counter=0;counter<4;++counter) {
        Fixture f;const unsigned id=enemy?4:0;auto& a=f.actors[id];put(a.storage,0x21c,10);f.records[id][0]=2;
        put(f.records[id],0x4a,counter,2);put(f.records[id],8,160);
        auto e=f.root.entity(id);put(e,0x182,0xabcd,2);
        if(enemy)f.enemy();else f.update();
        require(half(e,0x172)==1&&half(e,0x17c)==2&&half(e,0x17e)==expected[counter]&&half(e,0x180)==id,"Primary2 source divisor table and self owner");
        require(half(e,0x182)==0xabcd,"Event fourth halfword survives");
    }
    for(int hp:{-33,-1,std::numeric_limits<int>::max(),std::numeric_limits<int>::min()}) {
        Fixture f;auto& a=f.actors[0];put(a.storage,0x21c,99);f.records[0][0]=2;put(f.records[0],8,std::uint32_t(hp));
        put(f.records[0],0x4a,hp<0&&hp!=std::numeric_limits<int>::min()?1:0,2);f.update();
        const unsigned amount=hp==-33?0:hp==-1||hp==std::numeric_limits<int>::min()?1:0;
        require(half(f.root.entity(0),0x17e)==amount,"Primary2 signed division/add wrap and low16 event");
    }
    Fixture both;put(both.actors[0].storage,0x21c,99);both.records[0][0]=2;both.records[0][1]=3;put(both.records[0],8,160);both.update();
    require(half(both.root.entity(0),0x172)==2&&half(both.root.entity(0),0x17c)==3&&half(both.root.entity(0),0x184)==2,"Secondary event precedes primary event");
    for(unsigned old:{15u,16u,65535u}) {
        Fixture f;put(f.actors[0].storage,0x21c,99);f.records[0][0]=2;put(f.root.entity(0),0x172,old,2);f.update();
        const unsigned cursor=old==65535?0:15;
        require(half(f.root.entity(0),0x172)==cursor&&half(f.root.entity(0),0x174+cursor*8)==2,"Queue wraps16 then caps signed positive overflow");
    }
}
void post_action_failure_order() {
    Fixture missing;auto& a=missing.actors[0];put(a.storage,0x21c,99);a.stat_bindings[3]={};
    rejects([&]{missing.update();},"secondary status");
    require(word(a.storage,0x21c)==1&&half(missing.root.entity(0),0x60)==0xffff&&word(a.storage,0x194)==std::bit_cast<unsigned>(2.f),"Status guard follows state/invalidation before timer clear");
    Fixture exhausted;auto& b=exhausted.actors[0];put(b.storage,0x21c,99);exhausted.records[0][1]=6;put(b.storage,0x178,1,2);b.stat_bindings[2]={};
    rejects([&]{exhausted.update();},"write extent");
    require(half(b.storage,0x178)==0&&exhausted.records[0][0]==2,"Secondary6 decrement and primary store survive missing counter");
    Fixture counter;auto& c=counter.actors[0];put(c.storage,0x21c,99);counter.records[0][0]=2;put(counter.records[0],0x4a,4,2);
    rejects([&]{counter.update();},"divisor table");require(half(counter.root.entity(0),0x172)==0&&word(c.storage,0x198)==std::bit_cast<unsigned>(4.f),"Invalid table index fails before event/duration");
    Fixture queue;auto& d=queue.actors[0];put(d.storage,0x21c,99);queue.records[0][0]=2;put(queue.root.entity(0),0x172,32767,2);
    rejects([&]{queue.update();},"queue cursor");
    require(half(queue.root.entity(0),0x172)==32768&&word(d.storage,0x198)==std::bit_cast<unsigned>(4.f),"Unsupported negative cursor preserves count write before payload/timer");
}
void invalid_owners() {
    Fixture f;f.party[1]=nullptr;rejects([&]{f.update();},"owner missing");
    Fixture g;put(g.root.bytes,0x50,65);rejects([&]{g.update();},"count/binding");
    Fixture h;put(h.actors[0].storage,0x130,63,2);rejects([&]{h.update();},"identity");
    Fixture timer;number(timer.actors[0].storage,0x194,std::numeric_limits<float>::infinity());rejects([&]{timer.update();},"exceptional timer");
    Fixture missing_status;missing_status.full(0);missing_status.actors[0].stat_bindings[1]={};rejects([&]{missing_status.update();},"status binding");
}
}
int main() {
    try {waiting_and_ready();ordered_selection();status_motion_and_failure_order();stan_context_and_enemy_guards();cleanup_and_source_defaults();target_gates();enemy_action_readiness();enemy_grouping();enemy_grouping_graph_and_failure();post_action_timer_and_flags();post_action_secondary();post_action_primary_queue();post_action_failure_order();invalid_owners();
        std::cout<<"battle roster update tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
