#include "okage/results_interface.hpp"
#include "okage/event.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace okage;
namespace {
void require(bool b,const std::source_location at=std::source_location::current()) {
    if(!b)throw std::runtime_error("Results interface assertion line "+std::to_string(at.line()));
}
void put(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v,unsigned n=4) {
    for(unsigned i=0;i<n;++i)b.at(at+i)=std::uint8_t(v>>(i*8));
}
void hex(std::vector<std::uint8_t>& b,unsigned at,std::string_view s) {
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<s.size()/2;++i)b.at(at+i)=std::uint8_t(digits.find(s[i*2])*16+digits.find(s[i*2+1]));
}
struct XSection {std::string name;std::vector<std::vector<std::string>> strings;unsigned columns;std::vector<std::vector<std::uint32_t>> values;};
std::vector<std::uint8_t> xdb(std::vector<XSection> sections) {
    std::vector<std::uint8_t> pool;
    auto intern=[&](std::string_view s){auto p=std::uint32_t(pool.size());pool.insert(pool.end(),s.begin(),s.end());pool.push_back(0);return p;};
    std::vector<std::uint32_t> names;for(auto& s:sections)names.push_back(intern(s.name));
    for(auto& s:sections)for(auto& row:s.strings)for(auto& v:row)v=std::to_string(intern(v));
    std::vector<std::vector<std::uint32_t>> data;std::uint32_t bytes=0;
    for(auto& s:sections) {
        auto values=s.values;
        for(unsigned row=0;row<values.size();++row)
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
    std::vector<std::vector<std::string>> messages(0x105,std::vector<std::string>(1,"%s"));
    for(unsigned i=0x96;i<=0xa2;++i)messages[i][0]="%s";
    messages[0x88][0]="Empty";messages[0x91][0]="None";messages[0x95][0]="%s %s";
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
ResultsUiData data_fixture() {
    std::vector<std::uint8_t> b(1080);
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','S','U','I','1',0};
    std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1);put(b,12,28);put(b,16,16);put(b,20,49);
    hex(b,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    hex(b,56,"9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183");
    for(const auto [word,value]:std::array<std::pair<unsigned,unsigned>,11>{{
        {0,1},{1,44},{2,40},{3,1},{21,1},{22,1},{23,1},{24,11},{25,33},{26,17},{27,0}}})put(b,88+word*4,value);
    constexpr std::array<unsigned,16> ids={64,65,66,67,114,115,116,117,110,111,112,113,118,119,120,121};
    for(unsigned i=0;i<ids.size();++i)put(b,200+i*4,ids[i]);
    unsigned record=0;
    for(unsigned size:std::array<unsigned,4>{14,24,6,5}) {
        for(unsigned i=0;i<size;++i,++record) {
            const auto at=264+record*16;
            put(b,at,i+1==size?0xffffffffu:i);
            put(b,at+4,i+1==size?0xffffffffu:(i==0?1u:(size==24&&(i==2||i==4)?2u:0u)));
            put(b,at+8,i+1==size?0u:40u);
        }
    }
    constexpr std::array<std::uint32_t,8> streams={12,24,6,18,0xfffffc18u,10000,0,0x3f800000u};
    for(unsigned i=0;i<streams.size();++i)put(b,1048+i*4,streams[i]);
    return ResultsUiData(b);
}
ResultsTextData text_data() {
    std::vector<std::uint8_t> b(400);const std::string magic="OKRTXT1";
    std::copy(magic.begin(),magic.end(),b.begin());put(b,8,1);put(b,12,320);
    hex(b,16,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    const std::array<std::string_view,5> formats={"000,000,00","0","%03d,%03d,%03d","%d:%s%s","%d:%s%s"};
    for(unsigned i=0;i<formats.size();++i)std::copy(formats[i].begin(),formats[i].end(),b.begin()+80+i*64);
    return ResultsTextData(b);
}
std::shared_ptr<RuwResource> resource_fixture() {
    std::vector<std::uint8_t> b(80);put(b,12,1);put(b,16,14);
    for(unsigned group=0;group<14;++group) {
        const auto count=group==1?8u:group==13?130u:0u;put(b,24+group*4,count);
        for(unsigned id=0;id<count;++id) {
            const auto at=b.size();b.resize(at+68);
            put(b,at,0x3f800000);put(b,at+4,0x3f800000);
            put(b,at+8,0x80102030);put(b,at+12,0x80405060);
            put(b,at+16,0x61);put(b,at+20,1);put(b,at+60,id);
            unsigned cls=group==13&&id>=70?0u:0xffffffffu;
            if(group==13&&(id==2||id==56))cls=10;
            if(group==13&&id==3)cls=11;
            put(b,at+32,cls);put(b,at+44,20);put(b,at+48,40);
            put(b,at+52,600);put(b,at+56,20);
        }
    }
    return std::make_shared<RuwResource>(b);
}
struct Fixture {
    GameDataTables game=tables();
    BattleHudData numbers{hud_data()};
    ResultsUiData definition=data_fixture();
    ResultsTextData text=text_data();
    std::vector<std::uint8_t> variables=std::vector<std::uint8_t>(0xd78);
    std::vector<std::uint8_t> reward=std::vector<std::uint8_t>(0x38);
    UiPartySelection party;
    UiResourceCoordinator::Registry registry{};
    UiResourceCoordinator coordinator;
    std::array<std::uint32_t,16> accumulators{};
    ResultsUiShared shared;
    std::shared_ptr<RuwResource> resource=resource_fixture();
    std::shared_ptr<const void> texture=std::make_shared<int>(123); //Typed test owner; no raster is run.
    unsigned jingles=0;
    std::unique_ptr<ResultsInterface> ui;
    Fixture() {
        const unsigned at=0x5c4;
        variables[at]=1;put(variables,at+2,1,2);put(variables,at+4,10,2);put(variables,at+6,100,2);
        put(variables,at+8,75);put(variables,0x570,3);
        variables[0x844]='A';variables[0x845]=0;
        set_event_variable(variables,1,3,3);set_event_variable(variables,3,4,1000);
        put(reward,2,30,2);put(reward,4,1,2);put(reward,6,2,2);
        put(reward,8,1,2);put(reward,10,4,2);
        put(reward,0x26,20,2);
        ui=std::make_unique<ResultsInterface>(registry,coordinator,accumulators,shared,
            definition,resource,texture,0,context());
    }
    ResultsUiContext context() {
        return {variables,reward,game,party,numbers,text,[this](unsigned id){require(id==5);++jingles;},2};
    }
    std::int32_t state() const {return std::bit_cast<std::int32_t>(ui->words()[0xe4c/4]);}
    void clock() {registry[ui->handle()]->advance_clock();}
    void tick(std::uint32_t pressed=0) {clock();ui->first_update(pressed,context());ui->second_update(context());}
    void reach(unsigned desired) {
        for(unsigned i=0;i<4000&&state()!=std::int32_t(desired);++i)tick();
        require(state()==std::int32_t(desired));
    }
    void uncommitted() const {
        require(event_variable(variables,0x30001)==3&&event_variable(variables,0x40003)==1000);
    }
};
void actual_opening_and_dispatch_cursors() {
    Fixture f;require(f.state()==0&&f.shared.item_count==2&&f.party.counts()[3]==1);
    require(f.ui->window().word(0x3c00)==33&&f.ui->window().word(0x3c04)==17);
    f.ui->first_update(0,f.context());require(f.state()==1&&f.shared.animation.timer==0);
    require(f.ui->words()[0xe8c/4]==1&&f.ui->words()[0xa8/4]==0);
    require(f.ui->window().word(0x3bec)==2&&f.ui->window().opacity()==1.f);
    f.ui->second_update(f.context());require(f.shared.animation.timer==1&&f.shared.command==0);
    f.ui->first_update(0,f.context());require(f.shared.animation.timer==1&&f.ui->words()[0xa8/4]==1);
    f.ui->second_update(f.context());require(f.shared.animation.timer==2&&f.shared.command==1);
    f.reach(4);f.uncommitted();
    // First callback selects command4; only second callback owns its two increments.
    const auto timer=f.shared.animation.timer;f.ui->first_update(0,f.context());
    require(f.shared.animation.timer==timer);
    f.ui->second_update(f.context());
    require(f.shared.animation.timer==timer+2||f.shared.animation.timer==0);
    f.reach(10);f.uncommitted();require(f.jingles==0);
}
void confirm_gate_once_and_fade() {
    Fixture f;f.reach(10);f.uncommitted();
    f.ui->first_update(0x40,f.context());require(f.state()==10);f.uncommitted(); //Loader not ready.
    f.shared.loader_ready=2;f.ui->first_update(0x40,f.context());require(f.state()==10);f.uncommitted();
    f.shared.loader_ready=1;
    f.ui->first_update(0,f.context());require(f.state()==10);f.uncommitted(); //Held without a new pressed edge.
    f.ui->first_update(0x20,f.context());require(f.state()==10);f.uncommitted();
    f.ui->first_update(0x40,f.context());
    require(f.state()==-1&&f.ui->window().opacity()==128.f&&!f.ui->dead());
    require(event_variable(f.variables,0x30001)==9&&event_variable(f.variables,0x40003)==1030);
    const auto committed=f.variables;
    f.ui->first_update(0x40,f.context());require(f.variables==committed&&f.state()==-1); //No duplicate transfer.
    for(unsigned i=1;i<=31;++i) {
        f.clock();require(f.ui->window().opacity()==float(128-4*i));
        f.ui->first_update(0,f.context());f.ui->second_update(f.context());require(f.state()==-1&&!f.ui->dead());
    }
    f.clock();require(f.ui->window().opacity()==0.f);
    f.ui->first_update(0,f.context());require(f.state()==-2&&!f.ui->dead());
    f.ui->second_update(f.context());require(!f.ui->dead()&&f.variables==committed);
    f.ui->first_update(0,f.context());require(f.ui->dead()&&f.variables==committed);
    const auto handle=f.ui->handle();f.ui.reset();require(!f.registry[handle]);
}
void notice_wait_strict_gate() {
    Fixture f;f.reach(7);
    f.shared.progress.notice=1;f.shared.progress.notice_wait=0;
    f.ui->first_update(0,f.context());require(f.state()==-4&&f.shared.progress.notice_wait==0);
    for(unsigned i=1;i<=360;++i) {
        f.ui->first_update(0x40,f.context());
        require(f.state()==-4&&f.shared.progress.notice_wait==i);
    }
    f.ui->first_update(0,f.context());require(f.state()==-4&&f.shared.progress.notice_wait==361);
    f.ui->first_update(0x40,f.context());require(f.state()==7&&f.shared.progress.notice==2);
    require(f.ui->words()[0xa8/4]==7&&f.ui->words()[0xa4/4]==7);
    f.uncommitted();
}
}
int main() {
    try {actual_opening_and_dispatch_cursors();confirm_gate_once_and_fade();notice_wait_strict_gate();
        std::cout<<"Results interface lifecycle tests passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
