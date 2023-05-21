#include "okage/battle_damage.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value,const char* message,unsigned at=__builtin_LINE()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at));
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
    {Fixture f;put(f.actors[0].storage,0x1fc,201,2);rejects([&]{f.run();},"ordinary200");
        require(half(f.root.entity(4),0x17c)==0&&f.draws==0,"Unsupported action preserves classifier store");}
    {Fixture f;put(f.actors[0].storage,0x1bc,1);rejects([&]{f.run();},"category0");
        require(half(f.root.entity(4),0x17c)==1&&f.draws==0,"Class1 classification precedes unsupported resolver");}
    {Fixture f;put(f.actors[0].storage,0x1bc,2);rejects([&]{f.run();},"1597a8");auto e=f.root.entity(4);
        require(half(e,0x172)==1&&half(e,0x180)==0&&half(e,0x17c)==-1&&f.draws==0,"Unknown classifier stops after owner prefix");}
    {Fixture f;put(f.actors[0].storage,0x1bc,0x10000);rejects([&]{f.run();},"class0");
        require(half(f.root.entity(4),0x17c)==0&&f.draws==0,"Classifier low16 differs from full32 resolver class");}
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
}
int main() {
    try {
        ordinary_queue_and_no_hp_changes();random_branches_and_modifiers();integer_wrap_and_narrowing();
        queue_bounds_and_prefixes();service_order_and_partial_failure();
        std::cout<<"Battle physical damage producer tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
