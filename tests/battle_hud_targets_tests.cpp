#include "okage/battle_hud_targets.hpp"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    const auto ratio=std::bit_cast<std::uint64_t>(0.1);put(b,152,std::uint32_t(ratio));put(b,156,std::uint32_t(ratio>>32));
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
okage::GameDataTables synthetic_tables(bool commands=false) {
    const auto xdb=[commands](bool battle) {
        std::vector<std::uint8_t> strings;
        const auto str=[&](std::string_view s) {
            const auto at=strings.size();strings.insert(strings.end(),s.begin(),s.end());strings.push_back(0);
            return std::uint32_t(at);
        };
        const auto section=str(battle?"unit":"item"),messages=str("sysmes");
        const unsigned rows=battle?(commands?177:2):1,columns=battle?27:15;
        std::vector<std::uint32_t> words(rows*columns);
        for(unsigned i=0;i<rows;++i) {
            words[i*columns]=str(i==169?"SlotHero":i==170?"SlotAlly":i?"Ally":"Owned Hero");
            words[i*columns+(battle?1:14)]=str("model");
            if(battle)words[i*columns+15]=i%4;
        }
        const unsigned units_size=words.size();
        if(battle)for(unsigned i=0;i<=0xa6;++i) {
            std::string_view s="";
            if(i==0x8b||i==0x7e)s="            ";if(i==0xa3||i==0x55)s="HP:%s|%s";
            if(i==0x85)s="ALL TARGETS";if(i==0x8e)s="Command:%s";if(i==0x8f)s="Cost:%s/%s";
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

std::vector<std::uint8_t> command_fixture() {
    std::vector<std::uint8_t>b(384);const std::string magic="OKBCMD2";
    std::copy(magic.begin(),magic.end(),b.begin());put(b,8,2);put(b,12,304);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)b[16+i]=std::stoul(hash.substr(i*2,2),nullptr,16);
    const int words[]={5,19,20,6,2,0,4,6,12,24,6,18,-1000,0,512,2048};
    for(unsigned i=0;i<16;++i)put(b,80+i*4,words[i]);
    for(unsigned i=0;i<12;++i)put(b,144+i*4,i+10);
    unsigned at=192;for(const std::string text:{" ","Count:%s","Name:%s"}) {
        std::copy(text.begin(),text.end(),b.begin()+at);at+=64;
    }
    return b;
}
std::vector<std::uint8_t> ruw_fixture() {
    std::vector<std::uint8_t>b(40+76*68);put(b,12,1);put(b,16,4);put(b,36,76);
    for(unsigned i=0;i<76;++i) {
        const unsigned at=40+i*68;
        put(b,at+8,0x40123456);put(b,at+12,0x20445566);
        put(b,at+16,0x8061);put(b,at+20,1);put(b,at+60,i);
    }
    return b;
}
std::string text(const okage::RuwResource& r,int id) {
    return r.widget(3,id).runtime->text->data();
}
std::vector<std::uint8_t> read(const char* p) {
    std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("fixture input unavailable");
    return {std::istreambuf_iterator<char>(f),{}};
}
}
int main(int argc,char** argv) {
 try {
    okage::BattleHudData numbers(fixture2());okage::BattleUiCommandData commands(command_fixture());
    auto tables=synthetic_tables();okage::RuwResource r(ruw_fixture());okage::BattleHud hud;
    hud.formatted.fill('z');hud.padded_name.fill('q');hud.current_number.fill('x');
    hud.command_state_timer=777;
    okage::BattleUiFormationSummary formation;formation.count=2;
    formation.unit_ids={1,2};formation.counts={1,123};
    update_battle_target_list(r,hud,numbers,commands,tables,formation);
    require(text(r,30)=="Name:Owned Hero"&&text(r,31)=="Name:Ally","actual unit names in formation order");
    require(text(r,68)=="Count:"+numbers.number2(1)&&text(r,69)=="Count:"+numbers.number2(99),"source two digit counts and saturation");
    require(text(r,70)==" "&&text(r,32)=="            ","four slot default reset");
    require(r.widget(3,25).runtime->words[0]==38&&r.widget(3,27).runtime->words[0]==71,"occupied and unused icon selectors");
    require(r.widget(3,25).runtime->words[4]==0x400c0b0a&&r.widget(3,26).runtime->words[4]==0x400f0e0d,"unit palette and preserved alpha");
    require(std::string(hud.padded_name.data())=="Ally        "&&hud.padded_name[13]=='q',"memcpy name without nul and persistent scratch tail");
    require(hud.copied_unit->name=="Ally"&&hud.formatted[250]=='z',"last unit and formatted tail preserved");
    hud.type2_flag_b=1;update_battle_target_list(r,hud,numbers,commands,tables,formation);
    require(text(r,30)==" "&&text(r,31)==" "&&hud.type2_flag_a==1&&hud.command_state_timer==0,"source flag override and shared timer");
    require(text(r,68)=="Count:"+numbers.number2(1),"flag keeps count text");
    hud.command_state_timer=91;formation.count=0;
    update_battle_target_list(r,hud,numbers,commands,tables,formation);
    require(hud.command_state_timer==91&&hud.copied_unit->name=="Ally","empty list preserves loop-only state");
    require(text(r,68)==" "&&text(r,30)=="            ","empty list clears all four rows");
    formation.count=9;rejects([&]{update_battle_target_list(r,hud,numbers,commands,tables,formation);});
    auto bad=command_fixture();bad[192+64+6]='d';rejects([&]{okage::BattleUiCommandData d(bad);});
    auto v1=command_fixture();v1.resize(144);v1[6]='1';put(v1,8,1);put(v1,12,64);
    okage::BattleUiCommandData old(v1);rejects([&]{(void)old.target_palette();});
    //15e900 first branch falls through into option highlighting; option2 exits.
    r.set_rgba(3,3,{-3000,-3000,-3000,0});r.set_rgba(3,15,{-3000,-3000,-3000,0});
    override_battle_target_display(r,commands,tables,0,1,1,true);
    require(text(r,30)=="ALL TARGETS"&&text(r,68)==" ","single selected-action override");
    require(r.widget(3,3).runtime->words[4]>>24==64&&r.widget(3,15).runtime->words[4]>>24==64,"fallthrough alpha adds respect authored caps");
    r.set_text(3,30,"unchanged");override_battle_target_display(r,commands,tables,1,1,1,true);
    require(text(r,30)=="unchanged","category excludes special override");
    r.set_rgba(3,4,{-3000,-3000,-3000,0});r.set_rgba(3,39,{-3000,-3000,-3000,0});
    override_battle_target_display(r,commands,tables,2,2,3,false);
    require(text(r,30)=="ALL TARGETS"&&r.widget(3,39).runtime->words[4]>>24==64,"option2 override independent of availability");
    require(r.widget(3,4).runtime->words[4]>>24==0,"option2 returns before normal option highlight");
    if(argc==6) {
        okage::BattleUiCommandData owned_commands(read(argv[1]));okage::BattleHudData owned_numbers(read(argv[2]));
        okage::GameDataTables owned_tables(read(argv[3]),read(argv[4]));okage::RuwResource owned_ruw(read(argv[5]));
        okage::BattleHud actual;formation={};formation.count=1;formation.unit_ids[0]=1;formation.counts[0]=2;
        update_battle_target_list(owned_ruw,actual,owned_numbers,owned_commands,owned_tables,formation);
        require(text(owned_ruw,30)==owned_commands.target_name_text(owned_tables.ui_unit(1).name),"owned data target name");
        override_battle_target_display(owned_ruw,owned_commands,owned_tables,0,2,1,true);
    } else require(argc==1,"expected zero or five owned fixture paths");
    std::cout<<"battle target list tests passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
