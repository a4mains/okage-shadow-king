#include "okage/results_progress.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool b){static unsigned n=0;++n;if(!b)throw std::runtime_error("results progress assertion "+std::to_string(n));}
template<class F>void rejects(F&& f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected results rejection");}
template<class C>void put(C& b,std::size_t at,std::uint32_t v,unsigned n=4){for(unsigned i=0;i<n;++i)b.at(at+i)=std::uint8_t(v>>(8*i));}
std::uint32_t get(okage::Bytes b,std::size_t at,unsigned n=4){if(at>b.size()||b.size()-at<n)throw std::runtime_error("test bytes bounds");std::uint32_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint32_t(b[at+i])<<(8*i);return v;}
std::string widget_text(const okage::RuwWidget& w){if(!w.runtime||!w.runtime->text)throw std::runtime_error("missing results widget text");return reinterpret_cast<const char*>(w.runtime->text->data());}
struct XSection {std::string name;std::vector<std::vector<std::string>> strings;unsigned columns;std::vector<std::vector<std::uint32_t>> values;};
std::vector<std::uint8_t> xdb(std::vector<XSection> sections) {
    std::vector<std::uint8_t> pool;
    auto intern=[&](std::string_view s){auto p=std::uint32_t(pool.size());pool.insert(pool.end(),s.begin(),s.end());pool.push_back(0);return p;};
    std::vector<std::uint32_t> names;for(auto& s:sections)names.push_back(intern(s.name));
    for(auto& s:sections)for(auto& row:s.strings)for(auto& v:row)v=std::to_string(intern(v));
    std::vector<std::vector<std::uint32_t>> data;std::uint32_t bytes=0;
    for(auto& s:sections) {
        auto values=s.values;
        if(!s.strings.empty())for(unsigned row=0;row<values.size();++row)
            for(unsigned col=0;col<s.columns;++col)values[row][col]=std::uint32_t(std::stoul(s.strings[row][col]));
        data.push_back({});for(auto& row:values)data.back().insert(data.back().end(),row.begin(),row.end());
        bytes+=std::uint32_t(data.back().size()*4);
    }
    const auto base=32+sections.size()*16;std::vector<std::uint8_t> out(base+bytes+pool.size());
    put(out,0,0x424458);put(out,4,bytes);put(out,8,pool.size());put(out,12,sections.size());put(out,16,0xffffffff);
    std::size_t rec=0;
    for(unsigned i=0;i<sections.size();++i) {
        const auto dir=32+i*16;put(out,dir,names[i]);put(out,dir+4,rec);put(out,dir+8,data[i].size());
        for(auto v:data[i]){put(out,base+rec,v);rec+=4;}
    }
    std::copy(pool.begin(),pool.end(),out.begin()+base+bytes);return out;
}
okage::GameDataTables tables() {
    std::vector<std::vector<std::uint32_t>> units(1,std::vector<std::uint32_t>(27));
    units[0][2]=100;units[0][3]=98;units[0][4]=40;units[0][5]=98;units[0][6]=30;units[0][7]=98;units[0][8]=20;units[0][9]=98;
    for(unsigned i=10;i<27;++i)units[0][i]=i;
    std::vector<std::vector<std::string>> unit_strings{{"Hero","hero_model"}};
    std::vector<std::vector<std::uint32_t>> unit_values=units;
    for(auto& row:unit_values)row.resize(27);
    // Strings are patched into the two XDB columns by the fixture builder.
    std::vector<XSection> battle;
    battle.push_back({"unit",unit_strings,2,unit_values});
    battle.push_back({"learn",{},6,std::vector<std::vector<std::uint32_t>>(12,std::vector<std::uint32_t>(6))});
    std::vector<std::vector<std::string>> messages(0xa7,std::vector<std::string>(1,"%s"));
    for(unsigned i=0x96;i<=0xa2;++i)messages[i][0]="%s";
    messages[0x88][0]="No new skill";
    messages[0x94][0]="Learned %s";
    messages[0x95][0]="%s reached %s";
    battle.push_back({"sysmes",messages,1,std::vector<std::vector<std::uint32_t>>(messages.size(),std::vector<std::uint32_t>(1))});
    auto item_values=std::vector<std::vector<std::uint32_t>>(1,std::vector<std::uint32_t>(15));
    std::vector<XSection> item{{"item",{{"item","pad"}},2,item_values}};
    return okage::GameDataTables(xdb(battle),xdb(item));
}
std::vector<std::uint8_t> hud_data() {
    std::vector<std::uint8_t> b(480);const std::string magic="OKBHUD2";std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,2);put(b,12,400);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)b[16+i]=std::uint8_t(std::stoul(hash.substr(i*2,2),nullptr,16));
    for(unsigned i=0;i<18;++i)put(b,80+i*4,i+1);
    const auto ratio=std::bit_cast<std::uint64_t>(0.1);put(b,152,std::uint32_t(ratio));put(b,156,std::uint32_t(ratio>>32));
    unsigned at=160;for(const std::string s:{"\\f[1]\\c[%d]00\\c[0]0","%03d","\\f[1]\\c[%d]%s\\c[0]%s","\\f[1]\\c[%d]0\\c[0]0","%02d"}){std::copy(s.begin(),s.end(),b.begin()+at);at+=64;}return b;
}
std::vector<std::uint8_t> ruw() {
    constexpr unsigned total=136;
    std::vector<std::uint8_t> b(80+total*68);put(b,12,1);put(b,16,14);put(b,20,0);
    for(unsigned g=0;g<14;++g)put(b,24+g*4,g==1?6:(g==13?130:0),2);
    unsigned i=0;
    for(unsigned g=0;g<14;++g)for(unsigned n=0;n<(g==1?6:(g==13?130:0));++n,++i){const auto p=80+i*68;put(b,p+8,0x80808080);put(b,p+12,0x80808080);put(b,p+16,0x41);put(b,p+20,1);put(b,p+60,n);}
    return b;
}
void source_order_xp_and_snapshot_boundaries() {
    auto t=tables();std::vector<std::uint8_t> vars(0xd78);const unsigned a=0x5c4;
    vars[a]=1;put(vars,a+2,1,2);put(vars,a+4,10,2);put(vars,a+6,990,2);put(vars,0x570,3);
    okage::UiPartySelection party;party.refresh(vars,2);auto reward=std::vector<std::uint8_t>(56);put(reward,0x26,20,2);
    okage::ResultsProgress progress;okage::RuwResource resource(ruw());okage::BattleHudData hud(hud_data());
    progress.scratch.fill(0xcd);unsigned jingles=0;
    progress.initialize(vars,t,party,reward,resource,hud,2,[&](unsigned id){require(id==5);++jingles;});
    require(jingles==0);require(get(vars,a+4,2)==11&&get(vars,a+6,2)==10);
    require(progress.notice==0&&progress.notice_row==0&&progress.notice_wait==0);
    require(progress.notice_counters==std::array<std::int32_t,3>{-1,-1,-1});
    require(get(progress.scratch,0x5a0,4)==1010);
    require(get(progress.scratch,0x1e0+4,2)==11&&get(progress.scratch,0x1e0+6,2)==10);
    require(get(progress.scratch,0xf0+6,2)==1010);
    // The bank copies are 0x52 bytes at 0x50 strides, preserving overlap and
    // following source refresh; unrelated scratch bytes remain untouched.
    require(progress.scratch[0]==1&&progress.scratch[0x50]==0);
    require(progress.scratch[0x5ab]==0xcd);
    require(resource.widget(13,0x46).runtime->text->data()!=nullptr);
    require(resource.widget(13,0x28).runtime->words[0]==0x38);
}
void invalid_inputs_are_rejected_before_xp_mutation() {
    auto t=tables();std::vector<std::uint8_t> vars(0xd78);vars[0x5c4]=1;put(vars,0x5c6,1,2);put(vars,0x5c8,10,2);put(vars,0x570,3);
    okage::UiPartySelection party;party.refresh(vars,2);std::vector<std::uint8_t> reward(56);put(reward,0x26,3,2);
    okage::ResultsProgress p;okage::RuwResource r(ruw());okage::BattleHudData hud(hud_data());auto before=vars;
    rejects([&]{p.initialize(vars,t,party,reward,r,hud,4,{});});require(vars==before);
    rejects([&]{p.initialize(vars,t,party,std::span<std::uint8_t>(reward).first(0x31),r,hud,2,{});});require(vars==before);
    auto shortvars=std::vector<std::uint8_t>(0xd77);rejects([&]{p.initialize(shortvars,t,party,reward,r,hud,2,{});});
}
void false_update_preserves_source_level_notice_order() {
    auto t=tables();std::vector<std::uint8_t> vars(0xd78);const unsigned a=0x5c4;
    vars[a]=1;put(vars,a+2,1,2);put(vars,a+4,10,2);put(vars,a+6,999,2);put(vars,0x570,3);
    okage::UiPartySelection party;party.refresh(vars,2);
    std::vector<std::uint8_t> reward(56);put(reward,0x26,1,2);
    okage::ResultsProgress p;okage::RuwResource r(ruw());okage::BattleHudData hud(hud_data());
    p.scratch.fill(0);unsigned jingles=0;
    put(p.scratch,0x2d0+4,10,2);put(p.scratch,0x2d0+6,999,2);
    put(p.scratch,0xf0+6,1000,2);put(p.scratch,0x3c0+4,10,2);
    put(p.scratch,0x1e0+4,11,2);put(p.scratch,0x4b0+4,11,2);put(p.scratch,0x4b0+6,1000,2);
    const bool done=p.update_character(0,false,vars,t,party,r,hud,
        [&](unsigned id){require(id==5);++jingles;});
    require(!done&&jingles==1);
    require(p.notice==1&&p.notice_row==0&&p.notice_wait==0&&p.notice_counters[0]==0);
    require(r.widget(1,5).runtime->text&&r.widget(1,4).runtime->text);
    require(get(p.scratch,0x3c0+4,2)==11);
    require(get(p.scratch,0x2d0+6,2)==1000);
    require(get(p.scratch,0x4b0+4,2)==11);
    put(p.scratch,0x2d0+6,51,2);put(p.scratch,0xf0+6,50,2);
    const bool overshoot_done=p.update_character(0,false,vars,t,party,r,hud,{});
    require(overshoot_done);
    require(get(p.scratch,0x2d0+6,2)==50&&get(p.scratch,0xf0+6,2)==50);
}
void grant_clamps_and_number2_stat_sources() {
    auto t=tables();okage::BattleHudData hud(hud_data());
    const auto run_grant=[&](std::uint8_t state,std::int16_t level,std::int16_t xp,std::int16_t grant) {
        std::vector<std::uint8_t> vars(0xd78);const unsigned a=0x5c4;
        vars[a]=state;put(vars,a+2,1,2);put(vars,a+4,std::uint16_t(level),2);put(vars,a+6,std::uint16_t(xp),2);put(vars,0x570,3);
        okage::UiPartySelection party;party.refresh(vars,2);
        std::vector<std::uint8_t> reward(56);put(reward,0x26,std::uint16_t(grant),2);
        okage::ResultsProgress p;okage::RuwResource r(ruw());
        p.initialize(vars,t,party,reward,r,hud,2,{});
        return std::array<std::uint32_t,4>{get(reward,0x26,2),get(vars,a+4,2),get(vars,a+6,2),get(p.scratch,0xf0+6,2)};
    };
    const auto capped=run_grant(1,10,0,20000);
    require(capped[0]==9999&&capped[1]==19&&capped[2]==999&&capped[3]==9999);
    const auto minimum=run_grant(1,10,0,0);
    require(minimum[0]==1&&minimum[2]==1);
    const auto dead=run_grant(6,10,0,7);
    require(dead[0]==0&&dead[2]==0);

    std::vector<std::uint8_t> vars(0xd78);const unsigned a=0x5c4;
    vars[a]=1;put(vars,a+2,1,2);put(vars,a+4,10,2);put(vars,a+6,100,2);put(vars,0x570,3);
    okage::UiPartySelection party;party.refresh(vars,2);
    okage::ResultsProgress p;p.scratch.fill(0);
    // Logical result banks: animated=0x2d0, xp target=0xf0, final=0x1e0.
    put(p.scratch,0x2d0+4,10,2);put(p.scratch,0x2d0+6,50,2);put(p.scratch,0xf0+6,100,2);
    put(p.scratch,0x1e0+4,99,2);
    put(p.scratch,0x3c0+4,10,2);put(p.scratch,0x4b0+4,8,2);
    put(p.scratch,4,7,2);put(p.scratch,0x1a,11,2);put(p.scratch,0x1c,12,2);put(p.scratch,0x1e,13,2);
    put(p.scratch,0x4b0+6,100,2);
    put(p.scratch,0x4b0+0x1a,21,2);put(p.scratch,0x4b0+0x1c,22,2);put(p.scratch,0x4b0+0x1e,23,2);
    okage::RuwResource r(ruw());
    (void)p.update_character(0,false,vars,t,party,r,hud,{});
    require(widget_text(r.widget(13,0x4b)).find("\\c[0]7")!=std::string::npos);
    require(widget_text(r.widget(13,0x4c)).find("\\c[0]8")!=std::string::npos);
    require(widget_text(r.widget(13,0x4d)).find("11")!=std::string::npos);
    require(widget_text(r.widget(13,0x4e)).find("21")!=std::string::npos);
    require(widget_text(r.widget(13,0x4f)).find("12")!=std::string::npos);
    require(widget_text(r.widget(13,0x50)).find("22")!=std::string::npos);
    require(widget_text(r.widget(13,0x51)).find("\\c[0]13")!=std::string::npos);
    require(widget_text(r.widget(13,0x52)).find("\\c[0]23")!=std::string::npos);
}
void three_party_snapshot_stride_keeps_two_byte_overlap() {
    auto t=tables();std::vector<std::uint8_t> vars(0xd78);
    for(unsigned slot=0;slot<3;++slot) {
        const auto at=0x5c4+slot*0x50;vars[at]=1;put(vars,at+2,1,2);
        put(vars,at+4,10,2);put(vars,at+6,100,2);put(vars,0x570+slot*4,3);
    }
    okage::UiPartySelection party;party.refresh(vars,2);
    std::vector<std::uint8_t> reward(56);for(unsigned row=0;row<3;++row)put(reward,0x26+row*4,20,2);
    okage::ResultsProgress p;p.scratch.fill(0xcd);okage::RuwResource r(ruw());okage::BattleHudData hud(hud_data());
    p.initialize(vars,t,party,reward,r,hud,2,{});
    constexpr std::array<std::size_t,6> banks{0,0x2d0,0xf0,0x1e0,0x3c0,0x4b0};
    for(const auto bank:banks) {
        // The row-1 0x52-byte copy starts 0x50 bytes after row 0 and wins the
        // overlapping two bytes, matching the source's row-major call order.
        require(p.scratch[bank+0x50]==1&&p.scratch[bank+0x51]==0&&p.scratch[bank+0x52]==1);
    }
}
}
int main(){try{source_order_xp_and_snapshot_boundaries();invalid_inputs_are_rejected_before_xp_mutation();false_update_preserves_source_level_notice_order();grant_clamps_and_number2_stat_sources();three_party_snapshot_stride_keeps_two_byte_overlap();std::cout<<"ResultsProgress checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
