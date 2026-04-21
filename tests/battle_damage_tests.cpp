#include "okage/battle_damage.hpp"
#include "okage/actor.hpp"
#include "okage/battle_command_queue.hpp"
#include "okage/battle_entry.hpp"
#include "okage/battle_hit.hpp"
#include "okage/game_state.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <tuple>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {
void require(bool value,const char* message,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at.line()));
}
template<class F> void rejects(F action,const char* fragment) {
    try {action();}catch(const std::runtime_error& e) {
        require(std::string(e.what()).find(fragment)!=std::string::npos,"Unexpected rejection");return;
    }
    throw std::runtime_error("Expected unsupported-scope rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned size=4) {
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(value>>(i*8));
}
std::uint32_t word32(okage::Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
int half(okage::Bytes b,unsigned at) {
    return std::bit_cast<std::int16_t>(std::uint16_t(unsigned(b[at])|(unsigned(b[at+1])<<8)));
}
struct Fixture {
    okage::BattleState root;
    std::array<okage::BattleActor,5> actors;
    std::array<okage::BattleEntityBinding,5> bindings{};
    unsigned draws=0;
    Fixture() {
        root.initialize_root({1,0,0});put(root.bytes,0x50,5);
        for(unsigned i=0;i<actors.size();++i) {
            bindings[i].actor=&actors[i];actors[i].entity_id=i;
            put(actors[i].storage,0x3c8,101+i); //Live HP owner, deliberately unchanged.
            actors[i].stat_bindings[0]=std::span(actors[i].storage).subspan(0x3c8,4);
        }
        auto& s=actors[0].storage;
        put(s,0x1fc,200,2);put(s,0x166,20,2);put(s,0x1c0,16);
        put(s,0x1c4,100);put(s,0x1cc,100);put(s,0x1d0,4);
        put(actors[4].storage,0x168,4,2); //Physical base:20-4=16.
    }
    void run(std::initializer_list<int> rolls={0,0,6},std::int16_t command=16,
             std::int16_t owner=0,std::int16_t target=4) {
        std::vector<int> values(rolls);
        okage::battle_queue_damage_event(root,bindings,command,owner,target,[&] {
            require(draws<values.size(),"Unexpected extra RNG draw");
            return std::uint32_t(values[draws++])*32u;
        });
        require(draws==values.size(),"Expected RNG draw was skipped");
    }
    int amount(unsigned target=4) const {
        auto e=root.entity(target);return half(e,0x176+unsigned(half(e,0x172))*8);
    }
};
void ordinary_queue_and_no_hp_changes() {
    Fixture f;
    const auto source=f.actors[0].storage,target=f.actors[4].storage;
    const auto before=f.root.bytes;
    f.run();auto e=f.root.entity(4);
    require(half(e,0x172)==1&&half(e,0x17c)==0&&half(e,0x17e)==16&&half(e,0x180)==0,
            "Ordinary event category/amount/owner occupy first pushed slot");
    require(f.actors[0].storage==source&&f.actors[4].storage==target,"Producer never applies HP/status or motion writes");
    for(unsigned i=0;i<before.size();++i) {
        const unsigned base=0x60+4*0x200;
        if(i==base+0x172||i==base+0x173||(i>=base+0x17c&&i<base+0x182))continue;
        require(f.root.bytes[i]==before[i],"Only target queue prefix/record changes");
    }
    Fixture live;
    //No owned action_record is needed: the authoritative copied task row is used.
    put(live.actors[0].storage,0x1c0,32);live.run();require(live.amount()==36,"Live copied waza multiplier is authoritative");
    Fixture flags;put(flags.root.entity(4),0x1fc,0x80000000);flags.actors[4].storage[0x3cd]=6;
    flags.run();require(flags.amount()==16,"Producer does not add caller-owned death/status gates");
    Fixture self;self.run({0,0,6},16,0,0);
    require(self.amount(0)==20&&half(self.root.entity(0),0x172)==1,"Self target queues on the same live actor without HP mutation");
}
void random_branches_and_modifiers() {
    {Fixture f;put(f.actors[0].storage,0x16e,100,2);f.run({0,6});require(f.amount()==34,"Critical bypasses accuracy draw");}
    {Fixture f;put(f.actors[0].storage,0x1cc,0);f.run({0,0});require(f.amount()==-1,"Miss has no variance draw");}
    {Fixture f;put(f.actors[0].storage,0x1cc,0);put(f.actors[0].storage,0x16e,1,2);
        f.run({0,6});require(f.amount()==34,"Critical succeeds independently of accuracy");}
    {Fixture f;put(f.root.bytes,0x807c,4);f.run({49,6});require(f.amount()==34,"Mode4 adds50 critical percentage points");}
    {Fixture f;put(f.root.bytes,0x807c,4);put(f.actors[0].storage,0x16e,99,2);
        f.run({99,0,6});require(f.amount()==16,"Mode4 cap99 retains one noncritical roll");}
    {Fixture f;put(f.root.bytes,0x807c,4);put(f.actors[0].storage,0x16e,32760,2);
        f.run();require(f.amount()==16,"Critical addition narrows before upper clamp");}
    {Fixture f;put(f.root.bytes,0x807c,2);f.run();require(f.amount()==8,"Physical mode2 halves before variance");}
    {Fixture f;put(f.root.bytes,0x807c,1);f.run();require(f.amount()==20,"Mode1 scales by5/4 before variance");}
    {Fixture f;put(f.root.bytes,0x807c,3);f.run();require(f.amount()==16,"Mode3 does not halve physical damage");}
    {Fixture f;put(f.actors[0].storage,0x17c,1);put(f.root.bytes,0x807c,1);put(f.actors[4].storage,0x184,1);
        f.run();require(f.amount()==20,"Double then mode1 then defense halving");}
    {Fixture f;put(f.actors[4].storage,0x168,100,2);put(f.actors[4].storage,0x184,1);
        f.run();require(f.amount()==0,"Defense halves after minimum clamp and may yieldzero");}
    {Fixture f;put(f.actors[0].storage,0x166,2000,2);put(f.actors[4].storage,0x184,1);
        f.run();require(f.amount()==499,"Defense halves after999 cap");}
    {Fixture f;f.run({-1,6});require(f.amount()==34&&f.draws==2,"Signed RNG shift preserves a negative critical roll");}
}
void integer_wrap_and_narrowing() {
    {Fixture f;put(f.actors[0].storage,0x1c0,0x7fffffff);put(f.actors[4].storage,0x168,0,2);
        f.run();require(f.amount()==1,"Attack multiplication wraps32 before signed division");}
    {Fixture f;put(f.actors[4].storage,0x168,2,2);put(f.actors[0].storage,0x1c4,0x7fffffff);
        f.run();require(f.amount()==20,"Defense multiplication wraps32 before divide100");}
    {Fixture f;put(f.actors[4].storage,0x168,0,2);put(f.actors[0].storage,0x1c0,52432);
        f.run();require(f.amount()==4,"Scaled attack narrows16 before subtraction");}
    {Fixture f;put(f.actors[4].storage,0x168,0,2);put(f.actors[0].storage,0x166,32767,2);put(f.actors[0].storage,0x1c0,8);
        f.run({0,0,6},32);require(f.amount()==1,"Command attack quotient narrows before power scaling");}
    {Fixture f;put(f.actors[0].storage,0x1cc,0x80000000);put(f.actors[4].storage,0x170,1,2);
        f.run();require(f.amount()==16,"Accuracy subtraction wraps32");}
    {Fixture f;put(f.actors[4].storage,0x168,0,2);put(f.actors[0].storage,0x166,30000,2);put(f.actors[0].storage,0x16e,100,2);
        f.run({0,6});require(f.amount()==1,"Critical amount narrows16 before later modifiers");}
    {Fixture f;put(f.actors[4].storage,0x168,0,2);put(f.actors[0].storage,0x166,20000,2);put(f.actors[0].storage,0x17c,1);
        f.run();require(f.amount()==1,"Attack boost narrows16 before variance");}
    {Fixture f;put(f.actors[4].storage,0x168,0,2);put(f.actors[0].storage,0x166,32000,2);
        f.run({0,0,13});require(f.amount()==1,"Variance quotient narrows16 before clamping");}
    {Fixture f;put(f.actors[0].storage,0x174,1,2);put(f.actors[4].storage,0x174,0,2);
        f.run();require(f.amount()==24,"Selector4 uses source attribute row");}
    {Fixture f;put(f.actors[0].storage,0x174,1,2);put(f.actors[0].storage,0x1d0,2);
        f.run();require(f.amount()==8,"Explicit attribute selector overrides source row");}
    {Fixture f;put(f.actors[0].storage,0x166,0xffff,2);put(f.actors[4].storage,0x168,0,2);
        f.run();require(f.amount()==1,"Live attack is signed16");}
}
void queue_bounds_and_prefixes() {
    {Fixture f;auto e=f.root.entity(4);put(e,0x172,14,2);put(e,0x1f2,0xa55a,2);
        f.run();require(half(e,0x172)==15&&f.amount()==16,"Index14 pushes slot15");
        f.draws=0;put(f.actors[0].storage,0x1c0,32);f.run();
        require(half(e,0x172)==15&&f.amount()==36&&half(e,0x1f2)==std::int16_t(0xa55a),"Full queue overwrites15 and preserves padding");}
    {Fixture f;put(f.root.entity(4),0x172,0xffff,2);f.run();require(half(f.root.entity(4),0x172)==0&&f.amount()==16,"Count minus1 wraps into slot0");}
    {Fixture f;auto e=f.root.entity(4);put(e,0x172,32767,2);const auto before=f.root.bytes;
        rejects([&]{f.run();},"queue storage");require(half(e,0x172)==-32768&&f.draws==0,"Wrapped negative index preserves count prefix");
        for(unsigned i=0;i<before.size();++i)if(i!=0x60+4*0x200+0x172&&i!=0x60+4*0x200+0x173)
            require(before[i]==f.root.bytes[i],"Unsafe slot leaves all bytes except count untouched");}
    {Fixture f;f.bindings[0].actor=nullptr;const auto before=f.root.bytes;f.run({},16,0,-1);
        require(before==f.root.bytes,"Null source returns before invalid target access");}
    {Fixture f;const auto before=f.root.bytes;f.run({},16,-1,-1);require(before==f.root.bytes,"Both negative IDs return immediately");}
    {Fixture f;const auto before=f.root.bytes;rejects([&]{f.run({},16,0,-1);},"target outside");
        require(before==f.root.bytes,"Negative target guard precedes queue access");}
    {Fixture f;auto e=f.root.entity(4);f.bindings[4].actor=nullptr;put(e,0x17e,1234,2);
        rejects([&]{f.run();},"target actor");require(half(e,0x172)==1&&half(e,0x17c)==0&&half(e,0x180)==0&&half(e,0x17e)==1234&&f.draws==0,
            "Missing target retains count,owner,category but not an invented amount");}
    {Fixture f;rejects([&]{f.run({},16,0,63);},"binding index");auto e=f.root.entity(63);
        require(half(e,0x172)==1&&half(e,0x17c)==0,"Unrepresented target actor fails after queue prefix");}
    {Fixture f;put(f.actors[0].storage,0x1fc,201,2);f.run();
        require(half(f.root.entity(4),0x17c)==0&&f.amount()==16,"158e38 uses the waza ID only in its debug print");}
    {Fixture f;put(f.actors[0].storage,0x1bc,2);put(f.actors[0].storage,0x1c8,0x12340007);f.run({});auto e=f.root.entity(4);
        require(half(e,0x172)==1&&half(e,0x180)==0&&half(e,0x17c)==999&&half(e,0x17e)==7&&f.draws==0,
                "Class2 waza200 is1597a8's default999 and stores TASK1c8 low16 without RNG");}
    {Fixture f;put(f.actors[0].storage,0x1bc,0x10000);f.run({});
        require(half(f.root.entity(4),0x17c)==0&&f.amount()==-1&&f.draws==0,
                "Classifier low16 category0 but158e38 full-word class0x10000 returns-1 without RNG");}
    {Fixture f;put(f.actors[0].storage,0x1bc,0xffff0001u);f.run({});
        require(half(f.root.entity(4),0x17c)==1&&f.amount()==-1&&f.draws==0,"Negative full-word class with low16 1 misses without RNG");}
    {Fixture f;put(f.actors[0].storage,0x1bc,5);put(f.actors[0].storage,0x1c8,0xfffe);f.run({});
        require(half(f.root.entity(4),0x17c)==-1&&f.amount()==-2&&f.draws==0,"Class5 classifies-1 and stores signed TASK1c8 low16");}
    {Fixture f;put(f.actors[4].storage,0x174,4,2);rejects([&]{f.run();},"attribute index");
        require(half(f.root.entity(4),0x17c)==0&&f.draws==0,"Attribute guard preserves queue prefix before RNG");}
}
void service_order_and_partial_failure() {
    for(unsigned fail=0;fail<3;++fail) {
        Fixture f;auto e=f.root.entity(4);put(e,0x17e,2222,2);const auto target=f.actors[4].storage;
        rejects([&]{okage::battle_queue_damage_event(f.root,f.bindings,16,0,4,[&]() ->std::uint32_t {
            if(f.draws++==fail)throw std::runtime_error("Injected RNG failure");return 0;
        });},"Injected");
        require(f.draws==fail+1&&half(e,0x172)==1&&half(e,0x17c)==0&&half(e,0x17e)==2222,
                "Failed draw retains queue prefix and old result without rollback");
        require(target==f.actors[4].storage,"Failed resolution never mutates target HP");
    }
    {Fixture f;okage::battle_queue_damage_event(f.root,f.bindings,16,0,4,[&] {
        ++f.draws;if(f.draws==2)put(f.root.bytes,0x807c,2);return f.draws==3?192u:0u;
    });require(f.amount()==8,"Mode is reread after critical and accuracy draws");}
    {Fixture f;okage::battle_queue_damage_event(f.root,f.bindings,16,0,4,[&] {
        ++f.draws;if(f.draws==3){put(f.root.entity(4),0x172,2,2);return 192u;}return 0u;
    });auto e=f.root.entity(4);require(half(e,0x17c)==0&&half(e,0x17e)==0&&half(e,0x186)==16,
            "Resolver return stores through reloaded queue cursor");}
    {Fixture f;rejects([&]{okage::battle_queue_damage_event(f.root,f.bindings,16,0,4,{});},"RNG owner");
        require(half(f.root.entity(4),0x172)==1&&half(f.root.entity(4),0x17c)==0,"Missing RNG fails at first source draw");}
}
struct Magic : Fixture {
    Magic() {
        auto& s=actors[0].storage;put(s,0x1bc,1);put(s,0x1fc,205,2);put(s,0x1c8,44);put(s,0x1d0,0);
        put(s,0x1c0,0);put(s,0x1c4,0);put(s,0x166,0,2);
        put(actors[4].storage,0x168,0,2);
    }
};
void magic_category1() {
    {Magic f;f.run({0,6});auto e=f.root.entity(4);
        require(half(e,0x172)==1&&half(e,0x17c)==1&&half(e,0x17e)==44&&half(e,0x180)==0&&f.draws==2,
                "Class1 category1: power44*16/16, factor100, accuracy then variance");}
    {Magic f;f.run({99,0});require(f.amount()==39,"Accuracy draw precedes variance: 44*58/64 toward zero");}
    {Magic f;f.run({0,13});require(f.amount()==48,"Variance71: 44*71/64");}
    {Magic f;put(f.actors[4].storage,0x174,1,2);f.run({0,6});require(f.amount()==22,"Target attribute1 row0 factor50");}
    {Magic f;put(f.actors[4].storage,0x174,2,2);f.run({0,6});require(f.amount()==66,"Target attribute2 row0 factor150");}
    {Magic f;put(f.actors[0].storage,0x1d0,4);put(f.actors[0].storage,0x174,1,2);f.run({0,6});
        require(f.amount()==66,"Selector4 uses the source attribute row (row1 col0 = 150)");}
    {Magic f;put(f.actors[0].storage,0x1d0,3);put(f.actors[4].storage,0x174,1,2);f.run({0,6});
        require(f.amount()==44,"Explicit selector3 row");}
    {Magic f;put(f.actors[0].storage,0x1cc,50);put(f.actors[4].storage,0x172,10,2);f.run({40});
        require(f.amount()==-1&&f.draws==1,"Roll>=accuracy-magic evasion misses with no variance draw");}
    {Magic f;put(f.actors[0].storage,0x1cc,50);put(f.actors[4].storage,0x172,10,2);put(f.actors[4].storage,0x170,90,2);
        f.run({39,6});require(f.amount()==44,"Magic accuracy ignores physical evasion TASK170");}
    {Magic f;put(f.actors[0].storage,0x16e,100,2);f.run({0,6});require(f.amount()==44&&f.draws==2,"No critical draw for magic");}
    {Magic f;put(f.root.bytes,0x807c,4);put(f.actors[0].storage,0x16e,99,2);f.run({0,6});require(f.amount()==44,"Mode4 critical bonus is physical-only");}
    {Magic f;put(f.actors[0].storage,0x17c,1);f.run({0,6});require(f.amount()==44,"TASK17c doubling is physical-only");}
    {Magic f;put(f.actors[0].storage,0x166,999,2);put(f.actors[4].storage,0x168,999,2);put(f.actors[0].storage,0x1c0,100);
        put(f.actors[0].storage,0x1c4,100);f.run({0,6});require(f.amount()==44,"Attack/defense stats do not enter magic");}
    {Magic f;put(f.root.bytes,0x807c,2);f.run({0,6});require(f.amount()==44,"Mode2 (P_REDUCE) is physical-only");}
    {Magic f;put(f.root.bytes,0x807c,3);f.run({0,6});require(f.amount()==22,"Mode3 M_REDUCE halves magic");}
    {Magic f;put(f.actors[0].storage,0x1c8,45);put(f.root.bytes,0x807c,3);f.run({0,6});require(f.amount()==22,"M_REDUCE truncates 45/2");}
    {Magic f;put(f.root.bytes,0x807c,1);f.run({0,6});require(f.amount()==55,"Mode1 5/4 in the shared tail");}
    {Magic f;put(f.actors[4].storage,0x184,1);f.run({0,6});require(f.amount()==22,"Defending target halves after clamps");}
    {Magic f;put(f.actors[0].storage,0x1c8,0xffffffff);f.run({0,6});require(f.amount()==1,"Negative power narrows to -1 then clamps1");}
    {Magic f;put(f.actors[0].storage,0x1c8,0x10000000);f.run({0,6});require(f.amount()==1,"Power*command wraps32 to0, clamps1");}
    {Magic f;put(f.actors[0].storage,0x1c8,0x0fff);f.run({0,6},16);require(f.amount()==999,"4095*16/16 =4095 >999 caps");}
    {Magic f;put(f.actors[0].storage,0x1c8,0x10005);put(f.actors[4].storage,0x174,1,2);f.run({0,6});
        require(f.amount()==2,"Power quotient narrows16 (65541->5) before attribute50");}
    {Magic f;f.run({0,6},-16);require(f.amount()==1,"Signed16 command");}
    {Magic f;put(f.actors[0].storage,0x1cc,0x80000000);put(f.actors[4].storage,0x172,1,2);f.run({0,6});
        require(f.amount()==44,"Accuracy subtraction wraps32");}
    {Magic f;put(f.actors[4].storage,0x174,4,2);rejects([&]{f.run({});},"attribute index");
        require(half(f.root.entity(4),0x17c)==1&&f.draws==0,"Attribute guard after category store, before RNG");}
}
void classifier_and_source_effects() {
    const auto classify=[](std::uint32_t type,std::int16_t waza) {
        Fixture f;put(f.actors[0].storage,0x1bc,type);put(f.actors[0].storage,0x1fc,std::uint16_t(waza),2);
        put(f.actors[0].storage,0x1c8,0x00050009);f.run({});
        require(f.amount()==9&&f.draws==0,"Categories outside0..3 store TASK1c8 low16 without RNG");
        return half(f.root.entity(4),0x17c);
    };
    //1597a8 (pinned jump table 0x244440 verified for waza3/153/203/204).
    require(classify(2,3)==10&&classify(3,3)==10,"Retreat waza3 -> category10 (classes2 and3)");
    require(classify(2,204)==11&&classify(2,6)==11&&classify(2,9)==11&&classify(2,27)==11,"Escape/Snatch/Wild Cry -> 11");
    require(classify(2,153)==0x38&&classify(2,152)==0x38,"Rage/Fury -> 0x38");
    require(classify(2,203)==999&&classify(2,205)==999&&classify(2,1)==999&&classify(2,-1)==999,"Default999 incl. negative");
    require(classify(2,4)==0xd&&classify(2,21)==0xd&&classify(2,20)==0xe&&classify(2,11)==0x11&&classify(2,19)==0x19,
            "Status mapping rows");
    require(classify(2,115)==0x24&&classify(2,243)==0x24&&classify(2,147)==0x2e&&classify(2,137)==0x2f&&classify(2,149)==0x30&&classify(2,248)==0x34&&classify(2,238)==0x3a,
            "Ailment/buff mapping rows");
    //159a28 class4 (waza-0x91).
    require(classify(4,145)==0x2c&&classify(4,237)==0x2c&&classify(4,160)==4&&classify(4,215)==4&&classify(4,218)==4&&classify(4,217)==5&&
            classify(4,165)==0x23&&classify(4,166)==6&&classify(4,167)==7&&classify(4,223)==0x1a&&classify(4,236)==0x2b,
            "Recovery mapping rows");
    require(classify(4,200)==-1&&classify(4,144)==-1,"Class4 default-1");
    for(auto [type,waza,helper]:std::array<std::tuple<std::uint32_t,std::int16_t,const char*>,6>{{
        {2,2,"164070"},{2,5,"14adc8"},{3,26,"root8078"},{2,156,"14ac20"},{2,239,"14acb0"},{4,219,"14ad38"}}}) {
        Fixture f;put(f.actors[0].storage,0x1bc,type);put(f.actors[0].storage,0x1fc,std::uint16_t(waza),2);
        rejects([&]{f.run({});},helper);auto e=f.root.entity(4);
        require(half(e,0x172)==1&&half(e,0x180)==0&&half(e,0x17c)==-1&&f.draws==0,"Guarded classifier side effect after owner prefix");
    }
    {Fixture f;put(f.actors[0].storage,0x1bc,4);put(f.actors[0].storage,0x1fc,220,2);rejects([&]{f.run({});},"244820");}
    //158da0: waza1 sets source status2/counter0; waza0xca sets TASK180 (also on a miss).
    for(bool miss:{false,true}) {
        Fixture f;std::array<std::uint8_t,1> primary{1};std::array<std::uint8_t,2> counter{7,7};
        f.actors[0].stat_bindings[1]=primary;f.actors[0].stat_bindings[2]=counter;put(f.actors[0].storage,0x1fc,1,2);
        if(miss)put(f.actors[0].storage,0x1cc,0);
        f.run(miss?std::initializer_list<int>{0,0}:std::initializer_list<int>{0,0,6});
        require(primary[0]==2&&counter==std::array<std::uint8_t,2>{0,0}&&f.amount()==(miss?-1:16),"Waza1 source status2/counter0");
    }
    {Fixture f;put(f.actors[0].storage,0x1fc,0xca,2);f.run();
        require(word32(f.actors[0].storage,0x180)==1&&f.amount()==16,"Waza0xca source TASK180=1");}
    {Fixture f;put(f.actors[0].storage,0x1fc,0xca,2);put(f.actors[0].storage,0x1bc,2);f.run({});
        require(word32(f.actors[0].storage,0x180)==0,"Source side effects only follow categories0..3");}
    {Fixture f;put(f.actors[0].storage,0x1fc,1,2);rejects([&]{f.run();},"waza1 source status");
        require(f.amount()==16,"Waza1 binding guard follows the amount store");}
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Missing owned input "+path.string());
    return {std::istreambuf_iterator<char>(in),{}};
}
void copy_waza(okage::BattleActor& actor,const okage::GameDataTables& tables,std::int16_t id) {
    const auto& row=tables.action(id);
    for(unsigned i=0;i<13;++i)put(actor.storage,0x1ac+i*4,i==0||i==12?0:row.words[i]);
    put(actor.storage,0x1fc,std::uint16_t(id),2);
}
struct OwnedEnemy {
    okage::BattleActor actor;
    explicit OwnedEnemy(const okage::GameDataTables& tables,int unit_id) {
        const auto& unit=tables.unit(unit_id);
        auto& c=actor.storage;
        for(unsigned i=0;i<27;++i)put(c,0x350+i*4,unit[i]);
        constexpr std::array<unsigned,10> offsets={8,0x10,0x18,0x20,0x28,0x2c,0x30,0x34,0x3c,0x40};
        for(unsigned j=0;j<offsets.size();++j)put(c,0x164+j*2,std::uint16_t(half(okage::Bytes(c).subspan(0x350),offsets[j])),2);
        put(c,0x178,9,2);put(c,0x184,0);
        float radius=5;switch(unit[25]){case 1:case 3:radius=10;break;case 2:radius=15;break;case 4:radius=60;break;}
        put(c,0x4c,std::bit_cast<std::uint32_t>(radius));
        put(c,0x3c8,unit[2]);c[0x3cd]=1;
        actor.stat_bindings={std::span(c).subspan(0x3c8,4),std::span(c).subspan(0x3cd,1),std::span(c).subspan(0x3ce,2),
                             std::span(c).subspan(0x3d0,1),std::span(c).subspan(0x3d2,2),{}};
        actor.enemy=true;
        std::vector<std::uint8_t> motion(104);
        put(motion,0,0x544d58);put(motion,4,72);put(motion,16,0xffffffff);put(motion,24,1);
        put(motion,32,48);put(motion,48,0x00a00002);put(motion,52,0);
        okage::ModelFile model;model.models.emplace_back().bones.resize(1);
        actor.model=std::make_unique<okage::FieldActor>(std::move(model),motion,okage::TextureAnimation{},true,true);
        actor.model->animation().set_channel(1,{30,0,-10,1});
    }
};
struct OwnedBattle {
    okage::BattleState root;
    std::array<okage::BattleActor,4> party;
    std::array<std::array<std::uint8_t,1>,4> primaries{};std::array<std::array<std::uint8_t,2>,4> counters{};
    std::vector<std::unique_ptr<OwnedEnemy>> enemies;
    std::vector<okage::BattleEntityBinding> bindings;
    std::vector<int> rolls;unsigned draws=0;
    OwnedBattle(const okage::GameDataTables& tables,std::initializer_list<int> units) {
        root.initialize_root({1,0,0});root.initialize_secondary();
        for(unsigned i=0;i<4;++i) {
            party[i].entity_id=i;primaries[i][0]=1;
            party[i].stat_bindings[1]=primaries[i];party[i].stat_bindings[2]=counters[i];
            bindings.push_back({&party[i],{},0});
        }
        for(int unit:units) {
            enemies.push_back(std::make_unique<OwnedEnemy>(tables,unit));
            enemies.back()->actor.entity_id=unsigned(bindings.size());bindings.push_back({&enemies.back()->actor,{},1});
        }
        put(root.bytes,0x50,unsigned(bindings.size()));
    }
    void queue(std::int16_t command,std::int16_t owner,std::int16_t target,std::vector<int> values) {
        rolls=std::move(values);draws=0;
        okage::battle_queue_damage_event(root,bindings,command,owner,target,[&] {
            require(draws<rolls.size(),"Unexpected extra RNG draw");return std::uint32_t(rolls[draws++])*32u;
        });
        require(draws==rolls.size(),"Expected RNG draw was skipped");
    }
    std::array<int,4> top(unsigned target) {
        auto e=root.entity(target);const auto at=0x174+unsigned(half(e,0x172))*8;
        return {half(e,0x172),half(e,at),half(e,at+2),half(e,at+4)};
    }
};
void owned_actions(const std::filesystem::path& stage) {
    okage::GameDataTables tables(read_file(stage/"CMNDATA/XDB/BKDATA.XDB"),read_file(stage/"CMNDATA/XDB/ITEM.XDB"));
    require(tables.action(205).words[4]==1&&tables.action(205).words[7]==44&&tables.action(153).words[4]==2&&
            tables.action(204).words[4]==2&&tables.action(3).words[4]==2&&tables.action(207).words[4]==0,
            "Owned BKDATA waza classes");
    {
        OwnedBattle b(tables,{13,13});copy_waza(b.party[3],tables,205);
        b.queue(16,3,4,{0,6});
        require(b.top(4)==std::array<int,4>{1,1,22,3}&&b.draws==2,"205 vs oni01: category1, 44*50/100=22, owner3");
        b.queue(16,3,5,{99,0});
        require(b.top(5)==std::array<int,4>{1,1,19,3},"Second target: accuracy99<100 hits; variance58 -> 22*58/64=19");
        //159bd8 category1 on the owned target: lethal, death history, impact.
        auto& oni=b.enemies[0]->actor;std::vector<std::string> calls;int number=0,delta=0,history_category=-1,impact=0;
        okage::BattleHitServices hits;
        hits.attached_effect=[&](int id,okage::BattleActor&){calls.push_back("attached"+std::to_string(id));return 4u;};
        hits.effect_value=[&](unsigned,int value){calls.push_back("number");number=value;};
        hits.history=[&](okage::BattleActor&,std::int16_t d,std::int16_t c){calls.push_back("history");delta=d;history_category=c;};
        hits.death_history=[&](okage::BattleActor&){calls.push_back("death");};
        hits.positioned_effect=[&](int id,const okage::Float4&,bool){calls.push_back("impact");impact=id;return 9u;};
        const auto event=b.top(4);
        require(okage::battle_apply_hit(oni,std::int16_t(event[1]),std::int16_t(event[2]),hits)==6,"Owned oni01 HP1 is lethal");
        require(word32(oni.storage,0x3c8)==0&&number==22&&delta==-22&&history_category==1&&impact==39&&
                calls==std::vector<std::string>{"attached35","number","history","death","impact"},
                "Category1 hit: number/history(category1)/HP0/death/impact39 (owned unit25=3, radius10) in source order");
    }
    {
        OwnedBattle b(tables,{44,8});copy_waza(b.party[3],tables,205);
        b.queue(16,3,4,{0,6});b.queue(16,3,5,{0,6});
        require(b.top(4)[2]==44&&b.top(5)[2]==66,"Owned attributes0/2 -> factors100/150");
        put(b.root.bytes,0x807c,3);b.queue(16,3,4,{0,6});require(b.top(4)[2]==22&&b.top(4)[0]==2,"Mode3 M_REDUCE");
    }
    {   //Formation5/7: frog01 Fury (153, target group10) on rab01: category0x38, amount1, no RNG.
        OwnedBattle b(tables,{64,44});copy_waza(b.enemies[0]->actor,tables,153);
        auto& rab=b.enemies[1]->actor;put(rab.storage,0x18c,5,2);
        b.queue(0,4,5,{});require(b.top(5)==std::array<int,4>{1,0x38,1,4},"Fury queues category0x38 amount1 from word7");
        okage::BattleHitServices none;const auto hp=word32(rab.storage,0x3c8);
        require(okage::battle_apply_hit(rab,0x38,1,none)==0&&half(rab.storage,0x18c)==6&&word32(rab.storage,0x3c8)==hp,
                "0x38 raises rab01's command column5->6 without HP/services");
        require(okage::battle_apply_hit(rab,0x38,1,none)==0&&half(rab.storage,0x18c)==6,"Column clamps at6");
    }
    {
        OwnedBattle b(tables,{8,8});copy_waza(b.enemies[0]->actor,tables,204);copy_waza(b.party[0],tables,3);
        b.queue(0,4,4,{});require(b.top(4)==std::array<int,4>{1,11,0,4},"Escape queues category11 amount0");
        b.queue(0,0,0,{});b.queue(0,0,3,{});
        require(b.top(0)==std::array<int,4>{1,10,0,0}&&b.top(3)==std::array<int,4>{1,10,0,0},"Retreat queues category10 on Ari and Stan");
        okage::BattleEntryContext entry(0,0,0,0);std::vector<std::int32_t> spent;
        okage::BattleHitServices hits;
        hits.entry_escape_flag=[&]{const auto& e=entry.bytes();return std::int16_t(e[0x36]|(e[0x37]<<8));};
        hits.command_available=[&]{return okage::battle_command_available(b.root);};
        hits.command_push=[&](std::int32_t o,std::int32_t c){okage::battle_command_push(b.root,o,c);};
        hits.spend_money=[&](std::int32_t v){spent.push_back(v);};
        auto& cock=b.enemies[0]->actor;
        require(okage::battle_apply_hit(cock,11,0,hits)==0&&okage::battle_command_available(b.root)==0&&spent.empty(),
                "Category11 pushes and closes the command gate; amount0 spends nothing");
        require(okage::battle_apply_hit(b.party[0],10,0,hits)==0,"Closed gate: Retreat category10 pushes nothing");
        std::int32_t owner=-1;
        require(okage::battle_command_pop(b.root,b.bindings,owner)==4&&owner==3,"Queued (3,4)");
        require(okage::battle_command_pop(b.root,b.bindings,owner)==-1,"Only one push");
        put(b.root.bytes,0x81a8,1);
        require(okage::battle_apply_hit(b.party[0],10,0,hits)==0&&okage::battle_command_pop(b.root,b.bindings,owner)==3&&owner==0,
                "Open gate: Retreat pushes (0,3)");
        okage::BattleEntryContext blocked(0,0,0,1);put(b.root.bytes,0x81a8,1);
        hits.entry_escape_flag=[&]{const auto& e=blocked.bytes();return std::int16_t(e[0x36]|(e[0x37]<<8));};
        require(okage::battle_apply_hit(cock,11,0,hits)==0&&okage::battle_command_available(b.root)==1,
                "Entry argument d (bank7+36) nonzero blocks escape");
    }
    std::cout<<"Owned Chapter2 damage categories passed\n";
}
}
int main(int argc,char** argv) {
    try {
        magic_category1();
        ordinary_queue_and_no_hp_changes();random_branches_and_modifiers();integer_wrap_and_narrowing();
        queue_bounds_and_prefixes();service_order_and_partial_failure();classifier_and_source_effects();
        if(argc>1)owned_actions(argv[1]);
        else std::cout<<"Owned Chapter2 damage categories skipped: no stage argument\n";
        std::cout<<"Battle damage producer tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
