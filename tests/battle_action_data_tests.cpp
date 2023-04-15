#include "okage/battle_action_data.hpp"
#include "okage/game_state.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected rejection");}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t v,unsigned size=4){for(unsigned i=0;i<size;++i)b.at(at+i)=v>>(i*8);}
void hash(std::vector<std::uint8_t>& b,unsigned at,std::string_view value){for(unsigned i=0;i<32;++i)b.at(at+i)=std::stoul(std::string(value.substr(i*2,2)),nullptr,16);}
std::vector<std::uint8_t> fixture() {
    constexpr unsigned payload=256*4+(196+159+15)*136;
    std::vector<std::uint8_t>b(136+payload);const std::string magic="OKBACT1";std::copy(magic.begin(),magic.end(),b.begin());
    unsigned at=8;for(unsigned x:{1u,256u,256u,98u,159u,15u,136u,payload}){put(b,at,x);at+=4;}
    hash(b,40,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    hash(b,72,"973964019ddb24d033b0e10f4f3e2cf72fafdee9bdb25ee143ee87c832b0aff4");
    for(unsigned i=0;i<256;++i){put(b,136+i*2,i%98,2);put(b,648+i*2,i%159,2);}
    at=1160;
    for(unsigned i=0;i<370;++i,at+=136) {
        put(b,at,i,2);put(b,at+8,0xffff,2);put(b,at+134,0x8000,2);
    }
    return b;
}
std::vector<std::uint8_t> xdb(bool battle,bool action=true) {
    std::vector<std::uint8_t> strings;
    const auto string=[&](std::string_view s){auto n=strings.size();strings.insert(strings.end(),s.begin(),s.end());strings.push_back(0);return unsigned(n);};
    const auto section=string(battle?"unit":"item"),waza=string("waza"),name=string("Name"),model=string("model"),description=string("Description");
    unsigned cols=battle?27:15,sections=battle&&action?2:1;
    const unsigned base=32+sections*16,count=cols+(sections==2?26:0);
    std::vector<std::uint8_t>b(base+count*4+strings.size());
    for(auto [at,v]:{std::pair{0u,0x424458u},{4u,count*4},{8u,unsigned(strings.size())},{12u,sections},{16u,0xffffffffu},{32u,section},{40u,cols}})put(b,at,v);
    put(b,base,name);put(b,base+(battle?1:14)*4,model);
    if(sections==2) {
        put(b,48,waza);put(b,52,cols*4);put(b,56,26);
        for(unsigned row=0;row<2;++row) {
            const auto at=base+cols*4+row*52;put(b,at,name);put(b,at+48,description);
            put(b,at+4,0x80000000u+row);put(b,at+40,row);put(b,at+44,8+row);
        }
    }
    std::copy(strings.begin(),strings.end(),b.begin()+base+count*4);return b;
}
std::vector<std::uint8_t> read(const char* p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("input missing");return {std::istreambuf_iterator<char>(f),{}};}
}
int main(int argc,char**argv) {
 try {
    auto b=fixture();okage::BattleActionData data(b);
    require(data.action_count()==256,"owned action extent");
    require(data.motion(200,0,0).half(0)==(170%98)*2,"party source170 mapping");
    require(data.motion(201,0,0).half(0)==(170%98)*2+1,"paired action201 second record");
    require(&data.motion(208,0,0)==&data.motion(200,0,0),"208 shares200 descriptor");
    require(data.motion(200,1,97).half(0)==194,"enemy identity without party adjustment");
    require(data.motion(200,65536,65536-170).half(0)==0,"signed16 kind and party identity wrap");
    require(data.motion(255,1,0).half(0)==196+255%159,"whole owned action mapping");
    require(&data.motion(65536+255,1,0)==&data.motion(255,1,0),"action signed16 narrowing");
    require(data.motion(0,0,0).half(8)==-1&&data.motion(0,0,0).half(134)==-32768,"signed descriptor fields and last halfword");
    rejects([&]{data.motion(0,0,0).half(1);});rejects([&]{data.motion(0,0,0).half(136);});
    rejects([&]{data.motion(-1,0,0);});rejects([&]{data.motion(256,0,0);});
    rejects([&]{data.motion(200,1,-1);});rejects([&]{data.motion(200,1,256);});
    for(auto [mode,index]:{std::pair{-1,0},{0,0},{1,1},{2,2},{3,0},{65537,1}}) {
        auto s=data.secondary(200,mode);require(s.type==12&&s.descriptor.half(0)==355+index,"default mode selection");
    }
    const int actions[]={20,21,205,207,22,23,24,25,26,27,28,29};
    for(unsigned i=0;i<12;++i) {
        auto s=data.secondary(actions[i],2);require(s.descriptor.half(0)==358+int(i),"special action record selection");
        require(s.type==(i==2||i==3?7:15),"special action type");
    }
    auto bad=b;put(bad,136,98,2);rejects([&]{okage::BattleActionData d(bad);});
    bad=b;put(bad,648,0xffff,2);rejects([&]{okage::BattleActionData d(bad);});
    bad=b;bad[72]^=1;rejects([&]{okage::BattleActionData d(bad);});
    bad=b;bad.push_back(0);rejects([&]{okage::BattleActionData d(bad);});
    bad=b;bad.pop_back();rejects([&]{okage::BattleActionData d(bad);});
    okage::GameDataTables tables(xdb(true),xdb(false));
    require(tables.action_count()==2&&tables.action(0).name=="Name"&&tables.action(1).description=="Description","zero based owned waza strings");
    require(tables.action(0).words[1]==0x80000000&&tables.action(1).words[11]==9,"raw signed bits and whole words retained");
    rejects([&]{tables.action(-1);});rejects([&]{tables.action(2);});
    okage::GameDataTables no_actions(xdb(true,false),xdb(false));
    require(no_actions.action_count()==0,"optional absent fixture section");rejects([&]{no_actions.action(0);});
    auto malformed=xdb(true);put(malformed,56,25);rejects([&]{okage::GameDataTables t(malformed,xdb(false));});
    malformed=xdb(true);put(malformed,64+27*4+48,0xffffffff);rejects([&]{okage::GameDataTables t(malformed,xdb(false));});
    if(argc==4) {
        okage::BattleActionData owned(read(argv[1]));okage::GameDataTables t(read(argv[2]),read(argv[3]));
        require(owned.action_count()==t.action_count(),"owned action map/XDB count");
        for(unsigned a=0;a<t.action_count();++a) {
            (void)t.action(int(a));(void)owned.motion(a,0,0);(void)owned.secondary(a,0);
        }
        for(int unit=0;unit<256;++unit)for(int action:{200,201,208})(void)owned.motion(action,1,unit);
        std::cout<<"owned waza/descriptor selection smoke passed\\n";
    } else require(argc==1,"expected three owned paths");
    std::cout<<"battle action data tests passed\\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
