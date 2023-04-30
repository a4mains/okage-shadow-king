#include "okage/battle_action_setup.hpp"
#include "okage/battle_roster_update.hpp"
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
    std::vector<okage::BattleEntityBinding> bindings;
    unsigned draws=0,draw=32;
    Fixture(unsigned count=4):bindings(count) {
        root.initialize_root({1,1,17});put(root.bytes,0x50,count);
        for(unsigned i=0;i<count;++i){actors[i].entity_id=i;put(actors[i].storage,0x130,i,2);put(actors[i].storage,0x46,i<2?1:0,2);put(root.entity(i),0x1fc,0);bindings[i].actor=&actors[i];}
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
std::vector<std::uint8_t> read(const char* p){std::ifstream f(p,std::ios::binary);require(bool(f),"Owned input missing");return {std::istreambuf_iterator<char>(f),{}};}
}
int main(int argc,char** argv) {
    try {
        decode_and_rng();setup_and_failures();roster_integration();
        if(argc==4) {
            okage::BattleActionData data(read(argv[1]));okage::GameDataTables tables(read(argv[2]),read(argv[3]));
            Fixture f;auto& a=f.actors[0];put(a.storage,0x142,200,2);put(a.storage,0x146,2,2);
            require(okage::battle_setup_action(a,f.root,tables,data)==40&&a.action_motion->identity==166&&a.action_secondary->identity==355,"Owned HERO Attack setup");
        }else require(argc==1,"Expected three optional owned paths");
        std::cout<<"battle action setup tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
