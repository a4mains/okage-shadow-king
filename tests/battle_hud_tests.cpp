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
std::vector<std::uint8_t> fixture2() {
    auto b=fixture();b.resize(480);b[6]='2';put(b,8,2);put(b,12,400);
    unsigned at=352;
    for(const std::string s:{"\\f[1]\\c[%d]0\\c[0]0","%02d"}) {
        std::copy(s.begin(),s.end(),b.begin()+at);at+=64;
    }
    return b;
}
okage::GameDataTables synthetic_tables() {
    const auto xdb=[](bool battle) {
        std::vector<std::uint8_t> strings;
        const auto str=[&](std::string_view s) {
            const auto at=strings.size();strings.insert(strings.end(),s.begin(),s.end());strings.push_back(0);
            return std::uint32_t(at);
        };
        const auto section=str(battle?"unit":"item"),messages=str("sysmes");
        const unsigned rows=battle?2:1,columns=battle?27:15;
        std::vector<std::uint32_t> words(rows*columns);
        for(unsigned i=0;i<rows;++i) {
            words[i*columns]=str(i?"Ally":"Owned Hero");
            words[i*columns+(battle?1:14)]=str("model");
        }
        const unsigned units_size=words.size();
        if(battle)for(unsigned i=0;i<=0xa6;++i) {
            std::string_view s="";
            if(i==0x8b)s="            ";if(i==0xa3)s="HP:%s|%s";
            if(i==0xa4)s="Name:%s";if(i==0xa5)s="Resource:%s/%s";
            words.push_back(str(s));
        }
        const unsigned sections=battle?2:1,base=32+sections*16;
        std::vector<std::uint8_t> b(base+words.size()*4+strings.size());
        put(b,0,0x424458);put(b,4,words.size()*4);put(b,8,strings.size());put(b,12,sections);put(b,16,0xffffffff);
        put(b,32,section);put(b,40,units_size);
        if(battle){put(b,48,messages);put(b,52,units_size*4);put(b,56,0xa7);}
        for(unsigned i=0;i<words.size();++i)put(b,base+i*4,words[i]);
        std::copy(strings.begin(),strings.end(),b.begin()+base+words.size()*4);return b;
    };
    return okage::GameDataTables(xdb(true),xdb(false));
}
std::vector<std::uint8_t> type3_ruw() {
    // One empty template group, five empty widget groups and45 actual records.
    std::vector<std::uint8_t> b(48+45*68);put(b,12,1);put(b,16,6);put(b,44,45);
    for(unsigned i=0;i<45;++i) {
        const unsigned at=48+i*68;put(b,at+8,0x80203040);put(b,at+12,0x80506070);
        put(b,at+16,0x41);put(b,at+20,i>=8&&i<=13?2:1);put(b,at+60,i);
        put(b,at+52,101);put(b,at+56,203);
    }
    return b;
}
std::vector<std::uint8_t> type3_update_ruw() {
    std::vector<std::uint8_t> b(48);put(b,12,1);put(b,16,6);
    for(unsigned group=0;group<6;++group) {
        const unsigned count=group==2||group==3||group==5?57:0;put(b,24+group*4,count);
        for(unsigned id=0;id<count;++id) {
            const unsigned at=b.size();b.resize(at+68);
            put(b,at+8,0x80203040);put(b,at+12,0x80506070);put(b,at+16,0x41);
            put(b,at+20,group==5&&((id>=23&&id<=28)||id==44)?1:2);
            put(b,at+32,id?0xffffffff:0);put(b,at+52,100);put(b,at+56,100);put(b,at+60,id);
        }
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
void type3_checks() {
    const okage::BattleHudData data(fixture2());const auto tables=synthetic_tables();
    okage::RuwResource r(type3_ruw());std::vector<std::uint8_t> vars(0xd78);
    put(vars,0x570,3);put(vars,0x574,3);vars[0x5c6]=1;vars[0x616]=2;
    put(vars,0x5cc,101);vars[0x5dc]=202;vars[0x5ea]=0xce;vars[0x5eb]=0xff;
    put(vars,0x61c,0);vars[0x62c]=50;vars[0x63a]=100;
    put(vars,0x590,120);put(vars,0x594,0xffffffff);
    const std::string name="Renamed";std::copy(name.begin(),name.end(),vars.begin()+0x844);
    okage::UiPartySelection party;party.refresh(vars,3);const auto vars_before=vars;const auto party_before=party.rows();
    okage::BattleHud h;h.formatted.fill('F');h.padded_name.fill('P');h.current_number.fill('C');h.maximum_number.fill('M');
    h.type3_formatted.fill('x');h.type3_padded_name.fill('y');h.type3_current_number.fill('z');h.type3_maximum_number.fill('w');
    h.menu_state=19;h.mode_counter=77;h.type3_cursor=23;
    const auto original_rounding=std::fegetround();require(std::fesetround(FE_UPWARD)==0,"set hostile rounding");
    h.refresh_type3(r,data,tables,vars,party);
    require(std::fegetround()==FE_UPWARD,"type3 restores rounding");require(std::fesetround(original_rounding)==0,"restore test rounding");
    require(text(r,5,23)=="Name:Renamed     "&&text(r,5,24)=="Name:Ally        "&&text(r,5,25)=="Name:            ","owned/player/empty names");
    require(text(r,5,26)=="HP:"+data.number(101)+"|"+data.number(202),"real HP formatting");
    require(text(r,5,27)=="HP:"+data.number(0)+"|"+data.number(50)&&text(r,5,28)=="            ","zero HP and empty row");
    require(text(r,5,44)=="Resource:"+data.number2(99)+"/"+data.number2(-1),"resource upper clamp and signed values");
    require(r.widget(5,8).words[6]==50&&r.widget(5,9).words[6]==std::uint32_t(-101),"signed HP/charge fill order");
    require(r.widget(5,10).words[6]==0&&r.widget(5,11).words[6]==101,"zero HP remains active");
    for(unsigned id=8;id<=13;++id) {
        const auto alpha=id<12?128u:0u;
        require(r.widget(5,id).runtime->words[4]>>24==alpha&&r.widget(5,id).runtime->words[5]>>24==alpha,"both bar colors");
    }
    require(h.type3_ready==1&&h.menu_state==19&&h.mode_counter==77&&h.type3_cursor==23&&!h.bank2_bound&&!h.copied_unit,"helper owns only its source globals");
    require(h.formatted[0]=='F'&&h.padded_name[0]=='P'&&h.current_number[0]=='C'&&h.maximum_number[0]=='M',"type2 scratch remains separate");
    require(h.type3_formatted.back()=='x'&&h.type3_padded_name[13]=='y'&&h.type3_current_number.back()=='z'&&h.type3_maximum_number.back()=='w',"scratch tails preserved");
    require(vars==vars_before&&party.rows()==party_before,"refresh consumes but does not rebuild party or variables");
    put(vars,0x570,0);party.refresh(vars,3);h.type3_ready=0;h.refresh_type3(r,data,tables,vars,party);
    require(text(r,5,23)=="Name:Ally        "&&text(r,5,24)=="Name:            "&&h.type3_ready==1,"cache order drives actual rows");
    require(r.widget(5,10).runtime->words[4]>>24==0&&r.widget(5,10).runtime->words[5]>>24==0,"removed row hides both colors");
    rejects([&]{h.refresh_type3(r,data,tables,okage::Bytes(vars).first(0xd77),party);});
    vars[0x62c]=0;h.type3_ready=0;rejects([&]{h.refresh_type3(r,data,tables,vars,party);});
    require(!h.type3_ready,"unsupported division cannot claim completion");
}
void type3_update_checks() {
    const okage::BattleHudData data(fixture2());const auto tables=synthetic_tables();
    std::vector<std::uint8_t> vars(0xd78);okage::UiPartySelection party;
    for(unsigned i=0;i<3;++i) {
        put(vars,0x570+i*4,3);const unsigned c=0x5c4+i*0x50;
        vars[c+2]=i?2:1;put(vars,c+8,i==0?4:i==1?5:0);vars[c+0x18]=50;
    }
    vars[0x5c5]=0xff;party.refresh(vars,3);const auto before=vars;const auto cache=party.rows();
    okage::RuwResource r(type3_update_ruw());okage::BattleHud h;
    h.type3_blink=59;h.type3_cursor=72;h.mode_counter=81;
    std::vector<bool> calls;const auto initial_text=text(r,5,44);
    const auto setter=[&](bool enabled) {
        calls.push_back(enabled);
        if(!enabled)require(h.type3_ready==0&&text(r,5,44)==initial_text,"disable precedes refresh");
        else require(h.type3_ready==1&&text(r,5,44).starts_with("Resource:"),"enable follows completed refresh");
    };
    h.update_type3(r,data,tables,vars,party,3,setter);
    require(calls==std::vector<bool>({false,true})&&!h.type3_ready,"layout order and consumed readiness");
    require(h.type3_blink==0&&h.type3_cursor==72&&h.mode_counter==81,"distinct blink counter wraps at60");
    const auto rgb=[&](unsigned id){return r.widget(5,id).runtime->words[4]&0xffffff;};
    const auto a=[&](unsigned id,unsigned color=4){return r.widget(5,id).runtime->words[color]>>24;};
    require(rgb(26)==0x2d28be&&rgb(27)==0xffffff&&rgb(28)==0x2d28be,"strict threshold and zeroHP warning");
    require(r.widget(2,26).runtime->words[0]==60&&r.widget(3,48).runtime->words[0]==60&&r.widget(5,15).runtime->words[0]==60,"signed icon updates all three groups");
    for(unsigned id=48;id<54;++id)require(a(id)==64,"fixed overlay opacity");
    for(unsigned id=54;id<57;++id)require(a(id)==128,"active indicators");
    for(unsigned id=8;id<14;++id)require(a(id)==128&&a(id,5)==128,"active bars restore both colors");
    require(a(0)==0&&vars==before&&party.rows()==cache,"normal class reset and read-only state inputs");
    const auto ordinary_setter=[](bool){};
    h.type3_blink=29;h.update_type3(r,data,tables,vars,party,3,ordinary_setter);
    require(rgb(26)==0xffffff&&rgb(28)==0x2d28be,"blink off half preserves zeroHP red");
    h.type3_blink=std::uint32_t(-2);h.update_type3(r,data,tables,vars,party,3,ordinary_setter);
    require(h.type3_blink==std::uint32_t(-1)&&rgb(26)==0x2d28be,"negative source remainder");
    h.type3_blink=0x7fffffff;h.update_type3(r,data,tables,vars,party,3,ordinary_setter);
    require(h.type3_blink==std::uint32_t(-8),"signed32 addition wraps before remainder");
    for(unsigned count=0;count<3;++count) {
        for(unsigned i=0;i<3;++i)put(vars,0x570+i*4,i<count?3:0);
        party.refresh(vars,3);h.update_type3(r,data,tables,vars,party,3,ordinary_setter);
        for(unsigned i=0;i<3;++i) {
            require(a(54+i)==(i<count?128:0),"actual cache count indicators");
            require(a(8+i*2)==(i<count?128:0)&&a(8+i*2,5)==(i<count?128:0),"actual cache count bar colors");
        }
    }
    h.type3_reset=1;h.type3_blink=17;
    const auto icons_before=r.widget(2,26).runtime->words;
    h.update_type3(r,data,tables,vars,party,3,ordinary_setter);
    require(a(0)==128&&a(1)==0&&h.type3_blink==17&&r.widget(2,26).runtime->words==icons_before,"alternate class skips normal row/blink work");
    h.type3_reset=0;calls.clear();
    rejects([&]{h.update_type3(r,data,tables,vars,party,4,[&](bool e){calls.push_back(e);});});
    require(calls==std::vector<bool>({false,true})&&!h.type3_ready&&h.type3_blink==17&&h.type3_cursor==72&&a(52)==64,"demo guard follows known prefix and precedes demo mutation");
    h.type3_ready=77;rejects([&]{h.update_type3(r,data,tables,vars,party,3,{});});
    require(h.type3_ready==77,"missing host setter rejects before mutation");
    for(unsigned i=0;i<4;++i){put(vars,0x570+i*4,3);vars[0x5c6+i*0x50]=1;vars[0x5dc+i*0x50]=50;}
    party.refresh(vars,3);rejects([&]{h.update_type3(r,data,tables,vars,party,3,ordinary_setter);});
}
void owned_checks(const okage::BattleHudData& data,const okage::GameDataTables& tables,okage::Bytes raw,std::vector<std::uint8_t> vars) {
    okage::RuwResource r(raw);okage::BattleHud h;okage::UiPartySelection party;
    // This independent branch exercise uses native variables, not reference RAM.
    party.refresh(vars,3);h.refresh(r,data,tables,vars,party,3,0);
    require(text(r,2,35)=="\\s[0.76,1.0]\\f[2]Ari         ","name must retain twelve-character padding");
    require(r.widget(2,42).words[6]==r.widget(2,42).words[13],"full health bar width");
    h.refresh_type3(r,data,tables,vars,party);
    require(h.type3_ready==1&&r.widget(5,8).words[6]==r.widget(5,8).words[13],"owned type3 update and health fill");
    require(text(r,5,23).find("Ari         ")!=std::string::npos,"owned type3 player name formatting");
    std::vector<bool> layout_calls;
    h.update_type3(r,data,tables,vars,party,3,[&](bool e){layout_calls.push_back(e);});
    require(layout_calls==std::vector<bool>({false,true})&&!h.type3_ready&&h.type3_blink==1,"owned full type3 callback");
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
        rejects([&]{(void)data.number2(1);});
        const okage::BattleHudData data2(fixture2());
        require(data2.number(12)==data.number(12),"v2 preserves constructor formatting");
        require(data2.number2(0)==start+"0\\c[0]0"&&data2.number2(-100)==data2.number2(0),"two-digit nonpositive");
        require(data2.number2(1)==start+"0\\c[0]1"&&data2.number2(12)==start+"\\c[0]12"&&data2.number2(999)==start+"\\c[0]99","two-digit source format/clamp");
        type3_checks();type3_update_checks();
        for(unsigned n:{0u,8u,79u,351u,353u}){b=fixture();b.resize(n);rejects([&]{okage::BattleHudData bad(b);});}
        for(unsigned at:{0u,8u,12u,16u}){b=fixture();put(b,at,0);rejects([&]{okage::BattleHudData bad(b);});}
        b=fixture();put(b,156,0x7ff00000);rejects([&]{okage::BattleHudData bad(b);});
        b=fixture();std::fill(b.begin()+160,b.begin()+224,'a');rejects([&]{okage::BattleHudData bad(b);});
        b=fixture();b[223]='x';rejects([&]{okage::BattleHudData bad(b);});
        b=fixture();b[224]='X';const okage::BattleHudData bad_format(b);rejects([&]{bad_format.number(12);});
        b=fixture2();put(b,8,1);rejects([&]{okage::BattleHudData bad(b);});
        b=fixture2();b[479]='x';rejects([&]{okage::BattleHudData bad(b);});
        b=fixture2();b[416]='X';const okage::BattleHudData bad_short_format(b);rejects([&]{bad_short_format.number2(12);});
        std::cout<<"HUD data and numeric formatting tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
