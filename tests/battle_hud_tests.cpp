#include "okage/battle_hud.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool v,const char* what){if(!v)throw std::runtime_error(what);}
template<class F> void rejects(F f) {try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected HUD rejection");}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t value) {for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(8*i);}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(352);
    const std::string magic="OKBHUD1";std::copy(magic.begin(),magic.end(),b.begin());put(b,8,1);put(b,12,272);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)b[16+i]=std::stoul(hash.substr(i*2,2),nullptr,16);
    for(unsigned i=0;i<18;++i)put(b,80+i*4,i+1);
    const auto ratio=__builtin_bit_cast(std::uint64_t, (0.1));put(b,152,std::uint32_t(ratio));put(b,156,std::uint32_t(ratio>>32));
    unsigned at=160;
    for(const std::string s:{"\\f[1]\\c[%d]00\\c[0]0","%03d","\\f[1]\\c[%d]%s\\c[0]%s"}) {
        std::copy(s.begin(),s.end(),b.begin()+at);at+=64;
    }
    return b;
}
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path,std::ios::binary);require(bool(in),"missing HUD diagnostic input");
    return {(std::istreambuf_iterator<char>(in)),{}};
}
void report(const char* path,const okage::BattleHud& h,const okage::RuwResource& r,okage::Bytes vars) {
    namespace fs=std::filesystem;
    require(!fs::exists(path)&&!fs::is_symlink(path),"refusing existing HUD report");
    std::ofstream out(path);require(bool(out),"cannot create HUD report");
    out<<h.diagnostic(r,vars,"isolated");require(bool(out),"cannot write HUD report");
}
std::string text(const okage::RuwResource& r,unsigned group,int id) {return r.widget(group,id).runtime->text->data();}
void owned_checks(const okage::BattleHudData& data,const okage::GameDataTables& tables,okage::Bytes raw,std::vector<std::uint8_t> vars) {
    okage::RuwResource r(raw);okage::BattleHud h;okage::UiPartySelection party;
    // This independent branch exercise uses native variables, not reference RAM.
    party.refresh(vars,3);h.refresh(r,data,tables,vars,party,3,0);
    require(text(r,2,35)=="\\s[0.76,1.0]\\f[2]Ari         ","name must retain twelve-character padding");
    require(r.widget(2,42).words[6]==r.widget(2,42).words[13],"full health bar width");
    put(vars,0x5cc,0);vars[0x5c5]=0xff;h.mode_counter=59;h.selection_state=0;
    h.refresh(r,data,tables,vars,party,3,0);
    require(h.mode_counter==60&&h.selection_state==1,"warning counter/state");
    require((r.widget(2,38).runtime->words[4]&0xffffff)==0x2d28be,"zeroHP color");
    require(r.widget(2,23).runtime->words[0]==60,"signed second status icon");
    put(vars,0x570,0);party.refresh(vars,3);h.refresh(r,data,tables,vars,party,3,0);
    require((r.widget(2,42).runtime->words[4]>>24)==0&&(r.widget(2,42).runtime->words[5]>>24)==0,"absent bars both hidden");
    require(text(r,2,35)=="\\s[0.76,1.0]\\f[2]            ","absent padded name");
    h.menu_state=1;h.mode_counter=120;const auto old=r.widget(2,35).runtime->words;
    h.refresh(r,data,tables,{},party,3,0);
    require(h.mode_counter==120&&r.widget(2,35).runtime->words==old,"menu gate must precede refresh");
    h.menu_state=0;h.refresh(r,data,tables,vars,party,3,0);require(!h.mode_counter,"counter wraps after120");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==8&&std::string_view(argv[1])=="--diagnostic") {
            const okage::BattleHudData data(read(argv[2]));const okage::GameDataTables tables(read(argv[3]),read(argv[4]));
            const auto raw=read(argv[5]);const auto vars=read(argv[6]);okage::RuwResource r(raw);
            okage::BattleHud hud;okage::UiPartySelection party;
            hud.initialize_type2(r,data,tables,vars,party,3,0);hud.initialize_type3(r,tables);
            report(argv[7],hud,r,vars);owned_checks(data,tables,raw,vars);
            std::cout<<"owned HUD constructor diagnostic and branch checks passed\n";return 0;
        }
        require(argc==1,"usage: battle-hud-tests [--diagnostic HUD BKDATA ITEM RUW NATIVE_VARIABLES FRESH_JSON]");
        auto b=fixture();const okage::BattleHudData data(b);b.assign(b.size(),0);
        require(data.palette()[5][2]==18&&data.warning_fraction()==0.1,"HUD owns imported literals");
        const std::string start="\\f[1]\\c[510543555]";
        for(int n:{-1,0})require(data.number(n)==start+"00\\c[0]0","nonpositive number");
        require(data.number(1)==start+"00\\c[0]1","one digit");
        require(data.number(12)==start+"0\\c[0]12","two digits");
        require(data.number(123)==start+"\\c[0]123","three digits");
        require(data.number(1000)==start+"\\c[0]999","numeric clamp");
        for(unsigned n:{0u,8u,79u,351u,353u}){b=fixture();b.resize(n);rejects([&]{okage::BattleHudData bad(b);});}
        for(unsigned at:{0u,8u,12u,16u}){b=fixture();put(b,at,0);rejects([&]{okage::BattleHudData bad(b);});}
        b=fixture();put(b,156,0x7ff00000);rejects([&]{okage::BattleHudData bad(b);});
        b=fixture();std::fill(b.begin()+160,b.begin()+224,'a');rejects([&]{okage::BattleHudData bad(b);});
        b=fixture();b[223]='x';rejects([&]{okage::BattleHudData bad(b);});
        b=fixture();b[224]='X';const okage::BattleHudData bad_format(b);rejects([&]{bad_format.number(12);});
        std::cout<<"HUD data and numeric formatting tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
