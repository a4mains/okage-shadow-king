#include "okage/battle_action_setup.hpp"
#include "okage/battle_roster_update.hpp"
#include "okage/battle_enemy_action.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool v,const char* m,unsigned at=__builtin_LINE()) {
    if(!v)throw std::runtime_error(std::string(m)+" line "+std::to_string(at));
}
template<class F>void rejects(F f,const char* text) {
    try{f();}catch(const std::runtime_error& e){require(std::string(e.what()).find(text)!=std::string::npos,"Wrong guard");return;}
    throw std::runtime_error("Expected rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t v,unsigned size=4){for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(v>>(i*8));}
unsigned word(okage::Bytes b,unsigned at){return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);}
unsigned half(okage::Bytes b,unsigned at){return b[at]|(unsigned(b[at+1])<<8);}
float number(okage::Bytes b,unsigned at){return std::bit_cast<float>(word(b,at));}
std::vector<std::uint8_t> descriptors() {
    std::vector<std::uint8_t> b(51480);const std::string magic="OKBACT1";std::copy(magic.begin(),magic.end(),b.begin());
    unsigned at=8;for(unsigned v:{1u,256u,256u,98u,159u,15u,136u,51344u}){put(b,at,v);at+=4;}
    const auto hash=[&](unsigned at,std::string_view hex){for(unsigned i=0;i<32;++i)b[at+i]=std::stoul(std::string(hex.substr(i*2,2)),nullptr,16);};
    hash(40,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    hash(72,"973964019ddb24d033b0e10f4f3e2cf72fafdee9bdb25ee143ee87c832b0aff4");
    for(unsigned i=0;i<370;++i) {
        at=1160+i*136;put(b,at+8,120,2);put(b,at+0x1a,i>=355?0xffff:20,2);put(b,at+0x3a,i>=355?0:21,2);
    }
    return b;
}
std::vector<std::uint8_t> xdb(bool battle,unsigned target=0,std::int32_t delay=8) {
    const std::string strings("unit\0item\0waza\0Name\0Model\0Description\0",38);
    const unsigned columns=battle?27:15,sections=battle?2:1,base=32+16*sections,words=columns+(battle?256*13:0);
    std::vector<std::uint8_t>b(base+words*4+strings.size());
    for(auto [at,v]:{std::pair{0u,0x424458u},{4u,words*4},{8u,unsigned(strings.size())},{12u,sections},{16u,0xffffffffu},{32u,battle?0u:5u},{40u,columns}})put(b,at,v);
    put(b,base,15);put(b,base+(battle?1:14)*4,20);
    if(battle) {
        put(b,48,10);put(b,52,columns*4);put(b,56,256*13);
        for(unsigned row=0;row<256;++row) {
            const auto at=base+columns*4+row*52;put(b,at,15);put(b,at+48,26);
            for(unsigned j=1;j<10;++j)put(b,at+j*4,0xf0000000+row+j);
            put(b,at+40,target);put(b,at+44,std::uint32_t(delay));
        }
    }
    std::copy(strings.begin(),strings.end(),b.begin()+base+words*4);return b;
}
struct Fixture {
    okage::BattleState root;
    std::array<okage::BattleActor,18> actors;
    std::array<std::array<std::uint8_t,8>,18> stats{};
    std::vector<okage::BattleEntityBinding> bindings;
    unsigned draws=0,draw=32;
    Fixture(unsigned count=4):bindings(count) {
        root.initialize_root({1,1,17});put(root.bytes,0x50,count);
        for(unsigned i=0;i<count;++i){actors[i].entity_id=i;put(actors[i].storage,0x130,i,2);put(actors[i].storage,0x46,i<2?1:0,2);put(root.entity(i),0x1fc,0);bindings[i].actor=&actors[i];
            actors[i].stat_bindings[0]=std::span(stats[i]).first(4);
            actors[i].stat_bindings[1]=std::span(stats[i]).subspan(4,1);
            actors[i].stat_bindings[2]=std::span(stats[i]).subspan(6,2);put(stats[i],0,100);stats[i][4]=1;
        }
    }
    void decode(){okage::battle_decode_command(actors[0],root,bindings,[&]{++draws;return draw;});}
};
void decode_and_rng() {
    Fixture f;auto& b=f.actors[0].storage;std::fill(b.begin()+0x132,b.begin()+0x140,0xa5);
    put(b,0x222,0,2);put(b,0x224,0,2);f.decode();
    require(f.draws==1&&half(b,0x142)==200&&half(b,0x144)==0&&half(b,0x146)==3,"Ordered group consumes one shifted RNG draw");
    for(unsigned i=0;i<10;++i)require(b[0x132+i]==255,"Targets cleared");
    for(unsigned i=10;i<14;++i)require(b[0x132+i]==0xa5,"Untouched four-byte target tail");
    require(half(b,0x140)==0,"Decode resets target count");
    put(f.root.entity(3),0x1fc,0x80000000);f.decode();require(f.draws==2&&half(b,0x146)==2,"One eligible target still draws");
    f.bindings[2].actor=nullptr;f.decode();require(f.draws==2&&half(b,0x146)==0,"No targets returns0 without a draw");
    put(b,0x224,0xffff,2);put(f.actors[1].storage,0x46,0xffff,2);f.decode();
    require(f.draws==3&&half(b,0x146)==1&&half(b,0x144)==0xffff,"Group matching uses signed16 body+6");
    for(unsigned category:{1u,3u,4u,6u,7u,9u,10u,2u,0xffffu}) {
        Fixture g;auto& a=g.actors[0].storage;put(a,0x222,category,2);put(a,0x224,10,2);put(a,0x226,29,2);g.decode();
        const unsigned action=category==1?208:category==9?203:category==10?3:29;
        const unsigned group=category==1||category==4||category==7?10:category==3||category==6||category==10?11:category==9?0xffff:12;
        require(half(a,0x142)==action&&half(a,0x144)==group&&half(a,0x146)==(category==1?0:0xffff),"Category decoder semantics");
        require(g.draws==0,"Group10 empty/direct category consumes no draw");
    }
    Fixture many(18);for(auto& a:many.actors)put(a.storage,0x46,0,2);
    rejects([&]{many.decode();},"capacity");require(many.draws==0&&half(many.actors[0].storage,0x142)==200,"Candidate capacity guards after decode stores before RNG");
}
void setup_and_failures() {
    okage::BattleActionData data(descriptors());okage::GameDataTables tables(xdb(true),xdb(false));
    Fixture f;auto& a=f.actors[0];auto& b=a.storage;put(b,0x142,200,2);put(b,0x146,0x0102,2);put(b,0x1fe,0xabcd,2);
    const float duration=okage::battle_setup_action(a,f.root,tables,data);
    require(duration==40&&a.action_record_id==200&&a.action_record->name=="Name"&&a.action_record->description=="Description","Authored action data and delay");
    require(a.action_motion->identity==0&&a.action_secondary->identity==355,"Stable main/secondary identities");
    require(b[0x132]==2&&half(b,0x140)==1&&half(f.root.entity(0),0x62)==2,"Direct target uses only low signed byte");
    require(f.root.action_links[0][0]==0&&f.root.action_links[2][0]==0&&word(f.root.entity(0),0x68)==0,"Real ordered action registration");
    for(unsigned at:{0x1a8u,0x1acu,0x1dcu,0x200u})require(word(b,at)==0,"Typed pointer lanes normalized");
    for(unsigned i=1;i<12;++i)require(word(b,0x1ac+i*4)==tables.action(200).words[i],"All numeric waza words retained");
    require(half(b,0x1fe)==0xabcd&&half(b,0x218)==200&&half(b,0x21a)==12,"Preserve adjacent lane and initialize auxiliary selector");
    require(number(b,0x1e0)==120&&number(b,0x1e8)==20&&number(b,0x1f0)==21&&number(b,0x204)==-1&&number(b,0x20c)==0,"Descriptor signed durations");
    for(unsigned at:{0x1e4u,0x1ecu,0x1f4u,0x1f8u,0x208u,0x210u,0x214u})require(word(b,at)==0,"Action timers initialized");
    for(unsigned type:{3u,5u,6u}) {
        Fixture g;auto& actor=g.actors[0];put(actor.storage,0x142,208,2);put(actor.storage,0x146,2,2);
        okage::GameDataTables self(xdb(true,type),xdb(false));okage::battle_setup_action(actor,g.root,self,data);
        require(word(g.root.entity(0),0x68)==1&&actor.storage[0x132]==(type==3?2:0),"Action208 flag and self/direct target");
        require(half(g.root.entity(0),0x16c)==(type==3?1:2),"Self registration retains both aliases");
    }
    Fixture bad;put(bad.actors[0].storage,0x142,200,2);put(bad.actors[0].storage,0x146,255,2);
    rejects([&]{okage::battle_setup_action(bad.actors[0],bad.root,tables,data);},"signed target");
    require(bad.actors[0].action_motion&&half(bad.actors[0].storage,0x140)==1&&bad.actors[0].storage[0x132]==255&&!bad.actors[0].action_secondary,"Invalid target retains preceding setup writes only");
    Fixture multi;auto& m=multi.actors[0];put(m.storage,0x142,200,2);put(m.storage,0x144,5,2);put(m.storage,0x146,2,2);
    okage::GameDataTables unknown(xdb(true,7),xdb(false));
    rejects([&]{okage::battle_setup_action(m,multi.root,unknown,data);},"158548");
    require(half(m.storage,0x144)==12&&half(m.storage,0x146)==0xffff&&m.action_motion&&!m.action_secondary,"Fallback stores before guarded group construction");
    const int old=std::fegetround();std::fesetround(FE_UPWARD);
    for(int delay:{16777219,-16777219}) {
        Fixture g;put(g.actors[0].storage,0x142,200,2);put(g.actors[0].storage,0x146,2,2);
        okage::GameDataTables rounding(xdb(true,0,delay),xdb(false));
        require(okage::battle_setup_action(g.actors[0],g.root,rounding,data)==(delay>0?83886088.f:-83886088.f),"Separate toward-zero conversion and multiplication");
        require(std::fegetround()==FE_UPWARD,"Restore caller rounding");
    }
    std::fesetround(old);
}
void roster_integration() {
    okage::BattleActionData data(descriptors());okage::GameDataTables tables(xdb(true),xdb(false));
    for(unsigned category:{0u,1u,3u}) {
        Fixture f;okage::BattleEntryContext entry;std::array<std::uint8_t,0x1000> variables{};
        auto& a=f.actors[0];auto& b=a.storage;put(b,0x21c,2);put(b,0x220,3,2);put(b,0x222,category,2);put(b,0x224,0,2);put(b,0x226,22,2);
        put(b,0x194,0x42c80000);put(b,0x19c,0x3f800000);put(b,0x304,0xffffffff);put(f.actors[3].storage,0x21c,5);put(f.root.bytes,0x81b4,1);
        std::array<okage::BattleActor*,1> party{&a};
        okage::BattleRosterUpdateContext c{f.root,entry,variables,f.bindings,0,0,{},[&]{++f.draws;return f.draw;},&tables,&data};
        const auto run=[&]{okage::battle_party_roster_update(party,f.actors[3],c);};
        if(category==1) {
            rejects(run,"153388");require(word(b,0x21c)==2&&half(b,0x220)==3&&!a.action_record&&f.draws==1,"Combo stops before ordinary setup/poststores");
        } else if(category==3) {
            run();require(word(b,0x21c)==99&&word(b,0x340)==22&&word(b,0x304)==0&&word(b,0x194)==0&&word(b,0x19c)==0x3f800000&&!a.action_record,"Special22 state path and charge reset");
        } else {
            run();require(word(b,0x21c)==3&&half(b,0x220)==0&&number(b,0x1a0)==40&&word(b,0x19c)==0&&word(b,0x194)==0x42c80000&&f.draws==1,"Ordinary setup commits caller state only after success");
        }
    }
}
void ready_transition() {
    okage::BattleActionData data(descriptors());
    for(unsigned action:{200u,201u,208u,203u,35u,47u,34u,48u,3u}) {
        Fixture f;okage::BattleEntryContext entry;std::array<std::uint8_t,0x1000> variables{};
        auto& a=f.actors[0];auto& b=a.storage;put(b,0x21c,3);put(b,0x142,action,2);
        put(b,0x140,1,2);b[0x132]=2;put(b,0x19c,0x42200000);put(b,0x1a0,0x42200000);
        put(b,0x194,0x42c80000);put(b,0x304,0xffffffff);put(f.actors[3].storage,0x21c,5);put(f.root.bytes,0x81b4,1);
        okage::battle_register_action(f.root,0,2,0);
        std::array<okage::BattleActor*,1> party{&a};
        okage::BattleRosterUpdateContext c{f.root,entry,variables,f.bindings,0,0,{},[&]{++f.draws;return 0u;},nullptr,&data};
        okage::battle_party_roster_update(party,f.actors[3],c);
        const unsigned expected=action==200||action==201||action==208?5:action==203?10:action>=35&&action<=47?9:8;
        require(word(b,0x21c)==expected&&word(b,0x194)==0&&word(b,0x304)==0&&word(b,0x19c)==0x42200000,"Ready state transition, charge reset and wrapping action counter");
        require(f.draws==0&&half(f.root.entity(0),0x64)==1,"Readiness consumes neither RNG nor duplicate registration");
        if(expected==9)require(a.action_secondary&&half(b,0x218)==action&&half(b,0x21a)==12,"Special ready action initializes auxiliary descriptor");
        else require(!a.action_secondary,"Other ready actions do not reinitialize auxiliary state");
    }
}
std::vector<std::uint8_t> enemy_commands(unsigned action=200) {
    std::vector<std::uint8_t> b(56+84*7*4);const std::string magic="OKBECMD1";
    std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1);put(b,12,84);put(b,16,7);put(b,20,4);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)b[24+i]=std::stoul(hash.substr(i*2,2),nullptr,16);
    for(unsigned i=0;i<84*7;++i)put(b,56+i*4,0xdead0000u|action);
    return b;
}
void enemy_selection() {
    okage::BattleEnemyCommands commands(enemy_commands());okage::GameDataTables tables(xdb(true),xdb(false));
    for(unsigned mode=0;mode<=10;++mode) {
        Fixture f(6);auto& a=f.actors[4];auto& b=a.storage;
        put(b,0x188,mode,2);put(b,0x18c,3,2);put(b,0x164,100,2);
        for(unsigned i=0;i<2;++i)put(f.actors[i].storage,0x46,11,2);
        std::fill(b.begin()+0x132,b.begin()+0x140,0xa5);
        const std::array<unsigned,4> draws{32,64,96,0};unsigned used=0;
        okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,[&]{return draws.at(used++);});
        const unsigned base=mode==3?2:mode==4?4:mode==5?2:mode==7?1:mode==8?6:mode==9?0:3;
        require(half(b,0x18c)==base&&half(b,0x18e)==(mode==3?4:base),"Authored enemy mode, base and jitter");
        require(used==(mode==3?4:3)&&half(b,0x142)==200&&half(b,0x144)==11&&half(b,0x146)==(mode==3?0:1),"Chooser then target RNG order");
        for(unsigned i=0;i<10;++i)require(b[0x132+i]==255,"Enemy request clears exactly ten target bytes");
        for(unsigned i=10;i<14;++i)require(b[0x132+i]==0xa5,"Enemy request preserves target tail");
    }
    Fixture f(6);auto& a=f.actors[4];auto& b=a.storage;
    put(f.actors[0].storage,0x46,11,2);put(f.actors[1].storage,0x46,11,2);
    put(f.root.entity(1),0x1fc,0x80000000);f.stats[0][4]=6;
    unsigned used=0;const auto random=[&]{++used;return 0u;};
    okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,random);
    require(used==3&&half(b,0x146)==1,"Enemy picker excludes status6 but ignores root dead bit");
    f.stats[1][4]=6;used=0;okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,random);
    require(used==2&&half(b,0x146)==3,"Empty enemy target list returns3 without target RNG");
    for(unsigned override:{1u,5u,0xffffu,0x8000u}) {
        put(b,0x190,override,2);used=0;
        okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,random);
        const unsigned target=override==1?0:override==0xffff?0xfffe:2;
        require(used==2&&half(b,0x146)==target,"Override wraps signed16 then clamps only upper bound");
    }
    put(b,0x190,0,2);put(b,0x188,6,2);put(b,0x18c,2,2);put(f.root.entity(5),0x1fc,0x80000000);
    f.bindings[4].actor=nullptr;okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,random);
    require(half(b,0x18c)==6,"Mode6 counts alive entity flags even without actor binding");
    put(b,0x188,4,2);put(b,0x18c,0x7fff,2);okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,random);
    require(half(b,0x18c)==0,"Mode increment wraps signed16 before clamp");
    put(b,0x188,0,2);put(b,0x18a,0xffff,2);used=0;
    rejects([&]{okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,random);},"authored region");
    require(used==2&&half(b,0x140)==0,"Command table failure retains preceding RNG and request writes");
}
void enemy_costs_and_setup() {
    constexpr unsigned action=35;
    okage::BattleEnemyCommands commands(enemy_commands(action));okage::BattleActionData data(descriptors());
    for(bool hp:{true,false})for(unsigned resource:{7u,8u,9u}) {
        Fixture f(6);auto& a=f.actors[4];auto& b=a.storage;put(b,0x190,1,2);
        auto bk=xdb(true);const unsigned row=64+27*4+action*52;
        put(bk,row+4,hp?8:0);put(bk,row+8,hp?0:8);
        okage::GameDataTables tables(bk,xdb(false));put(f.stats[4],0,resource);put(f.root.bytes,0x8078,resource);
        okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,[&]{++f.draws;return 0u;});
        const bool spent=hp?resource>8:resource>=8;
        require(half(b,0x142)==(spent?action:200)&&word(hp?okage::Bytes(f.stats[4]):okage::Bytes(f.root.bytes),hp?0:0x8078)==(spent?resource-8:resource),"HP cannot spend last point; shared resource can spend exactly zero");
        require(f.draws==2,"Direct target override avoids RNG after cost handling");
    }
    Fixture f(6);auto& a=f.actors[4];auto& b=a.storage;auto bk=xdb(true);const unsigned row=64+27*4+action*52;
    put(bk,row+4,110);put(bk,row+8,0);put(b,0x164,100,2);put(b,0x190,1,2);
    okage::GameDataTables tables(bk,xdb(false));
    okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,[]{return 0u;});
    require(word(f.stats[4],0)==90&&half(b,0x142)==action,"Percentage HP cost uses authored maximum");
    f.stats[4][4]=5;put(f.stats[4],6,2,2);
    okage::battle_decode_enemy_command(a,f.root,f.bindings,tables,commands,[]{return 0u;});
    require(word(f.stats[4],0)==90&&half(b,0x142)==200,"Status5 counter2 prevents HP spending");
    //Real roster state1 uses decode before setup and preserves charge and selector.
    Fixture live(6);auto& e=live.actors[4];e.enemy=true;put(e.storage,0x40,1,2);
    put(e.storage,0x21c,1);put(e.storage,0x194,0x40800000);put(e.storage,0x198,0x40800000);
    put(e.storage,0x220,0xbeef,2);put(e.storage,0x190,1,2);put(e.storage,0x19c,0x42c80000);
    okage::BattleEntryContext entry;std::array<std::uint8_t,0x1000> vars{};
    okage::GameDataTables ordinary(xdb(true),xdb(false));okage::BattleEnemyCommands attack(enemy_commands());
    okage::BattleRosterUpdateContext c{live.root,entry,vars,live.bindings,0,0,{},[&]{++live.draws;return 0u;},&ordinary,&data,&attack};
    std::array<okage::BattleActor*,1> enemies{&e};okage::battle_enemy_roster_update(enemies,c);
    require(word(e.storage,0x21c)==3&&number(e.storage,0x1a0)==40&&word(e.storage,0x19c)==0&&word(e.storage,0x194)==0x40800000&&half(e.storage,0x220)==0xbeef,"Enemy caller commits delay/state/timer without party selector reset");
    require(live.draws==2&&live.root.action_links[0][0]==4&&live.root.action_links[4][0]==4,"Enemy authored action registers real target after chooser");
}
std::vector<std::uint8_t> read(const char* p){std::ifstream f(p,std::ios::binary);require(bool(f),"Owned input missing");return {std::istreambuf_iterator<char>(f),{}};}
}
int main(int argc,char** argv) {
    try {
        decode_and_rng();setup_and_failures();roster_integration();ready_transition();enemy_selection();enemy_costs_and_setup();
        if(argc==4) {
            okage::BattleActionData data(read(argv[1]));okage::GameDataTables tables(read(argv[2]),read(argv[3]));
            Fixture f;auto& a=f.actors[0];put(a.storage,0x142,200,2);put(a.storage,0x146,2,2);
            require(okage::battle_setup_action(a,f.root,tables,data)==40&&a.action_motion->identity==166&&a.action_secondary->identity==355,"Owned HERO Attack setup");
        }else require(argc==1,"Expected three optional owned paths");
        std::cout<<"battle action setup tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
