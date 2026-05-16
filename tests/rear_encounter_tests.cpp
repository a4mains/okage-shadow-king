#include "okage/field_rear_banner.hpp"
#include "okage/event.hpp"
#include "okage/game_state.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F f,const char* why) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error(why);
}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("missing owned input "+path.string());
    return {std::istreambuf_iterator<char>(in),{}};
}
constexpr std::int32_t keep=okage::RuwResource::preserve_component;
std::uint32_t alpha(const okage::RuwResource& r,std::size_t group,std::int32_t id) {
    return r.widget(group,id).runtime->words[4]>>24;
}
std::vector<std::uint8_t> synthetic_ruw(unsigned groups) {
    std::vector<std::uint8_t> b;
    const auto put=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(std::uint8_t(v>>(8*i)));};
    put(0);put(0);put(0);put(1);put(groups);put(0);
    for(unsigned g=0;g<groups;++g)put(2);
    for(unsigned g=0;g<groups;++g)for(unsigned id=0;id<2;++id) {
        std::array<std::uint32_t,17> w{0x3f800000,0x3f800000,id?0x80ffffffu:0x5a553228u,0x80ffffff,id?0x805fu:0x807fu,id?1u:3u,
            0xffffffff,0,0xffffffff,0,0,id?0xc2u:0x95u,id?0x1au:0xdu,0,0,id,0};
        for(auto v:w)put(v);
    }
    return b;
}
void synthetic() {
    okage::RuwResource r(synthetic_ruw(17));
    std::uint32_t flag=7;
    okage::FieldRearBanner banner(r,"<%s>",flag);
    require(flag==1,"115b20 clears, then 115b50 sets 0x1f8608");
    const auto& text=*r.widget(16,1).runtime;
    require(std::string(text.text->data())=="<surprise attack>","sysmes0x72 %s format");
    require(std::bit_cast<std::int32_t>(text.words[6])==245&&text.words[7]==0x1a,"x=0x140-5*15, y kept");
    require(banner.controller_state()==0&&!banner.bound()&&banner.traversals()==0,"no callback before traversal");
    // Group1 runtime changed after load is reset by 1ab8b0(0,1,-1).
    r.set_position(1,1,9,9);
    okage::UiPartySelection party;std::array<std::uint8_t,0x1000> vars{};
    okage::set_event_variable(vars,24,4,3);
    std::uint32_t accumulator=0x1234;
    banner.traverse(r,party,vars,2,accumulator,flag);
    require(banner.controller_state()==1&&banner.layout_enabled()&&banner.traversals()==1,"state0 then 115bd8");
    require(accumulator==0,"1a9a48 resets resource-slot0 accumulator");
    require(r.widget(1,1).runtime->words[6]==0xc2,"1ab8b0 reset group1 class-1");
    // 1ab410 keeps the box cap in its alpha register for the later text widget.
    require(alpha(r,16,0)==0x5a&&alpha(r,16,1)==0x5a,"alpha128 after alpha0, capped by the box 0x5a");
    require((r.widget(16,0).words[4]&1)&&(r.widget(16,1).words[4]&1),"show bits");
    require(party.counts()[1]==1&&party.at(1,0)==0,"121688 party table refresh");
    // State1: no second binding/reset; 115bd8 with the flag cleared hides nothing.
    r.set_position(1,1,9,9);
    banner.traverse(r,party,vars,2,accumulator,0);
    require(banner.controller_state()==1&&!banner.layout_enabled()&&r.widget(1,1).runtime->words[6]==9,"state1 idle");
    require(alpha(r,16,1)==0x5a,"115bd8 without flag writes no alpha");
    banner.traverse(r,party,vars,2,accumulator,1);require(banner.layout_enabled(),"flag restores 1a5488(win,1)");
    banner.mark();require(banner.marked(),"phase10 mark");
    rejects([&]{banner.mark();},"double mark rejected");
    // A fresh child resets the global through 115b20 before 115b50 sets it.
    okage::RuwResource fresh(synthetic_ruw(17));
    okage::FieldRearBanner second(fresh,"%s",flag);require(flag==1,"115b20/115b50 order");
    rejects([&]{okage::RuwResource small(synthetic_ruw(16));okage::FieldRearBanner b(small,"%s",flag);},"missing group16");
    rejects([&]{okage::FieldRearBanner b(fresh,"%d",flag);},"unreached conversion");
    rejects([&]{okage::FieldRearBanner b(fresh,"%s%s",flag);},"second conversion");
    rejects([&]{okage::FieldRearBanner b(fresh,std::string(113,'x')+"%s",flag);},"128-byte stack buffer");
}
void owned(const fs::path& stage) {
    okage::RuwResource r(read(stage/"NATIVE_UI/FLD.RUW"));
    const okage::GameDataTables tables(read(stage/"CMNDATA/XDB/BKDATA.XDB"),read(stage/"CMNDATA/XDB/ITEM.XDB"));
    require(tables.system_message(0x72)=="\\f[2]\\s[0.95,1.0]%s","owned USA sysmes 0x72");
    require(r.widgets().size()>16&&r.widgets()[16].size()==2,"owned FLD.RUW group16 box+text");
    require(alpha(r,16,0)==0x5a&&alpha(r,16,1)==0x80,"authored group16 alpha");
    std::uint32_t flag=0;
    okage::FieldRearBanner banner(r,tables.system_message(0x72),flag);
    const auto& text=*r.widget(16,1).runtime;
    require(std::string(text.text->data())=="\\f[2]\\s[0.95,1.0]surprise attack","owned banner text");
    require(text.words[6]==245&&text.words[7]==26,"owned banner position");
    okage::UiPartySelection party;std::array<std::uint8_t,0x1000> vars{};std::uint32_t accumulator=5;
    banner.traverse(r,party,vars,2,accumulator,flag);
    require(alpha(r,16,0)==0x5a&&alpha(r,16,1)==0x5a&&banner.layout_enabled(),"owned visible banner");
    const std::array fonts{okage::read_fnt(read(stage/"CMNDATA/FONT/IQ24.FNT")),
        okage::read_fnt(read(stage/"CMNDATA/FONT/BMUI.FNT")),okage::read_fnt(read(stage/"CMNDATA/FONT/BM.FNT"))};
    const auto glyphs=banner.glyphs(r,fonts[2]);
    require(glyphs.size()==14&&glyphs.front().code=='s'&&glyphs.back().code=='k',"diagnostic banner glyphs");
    require(glyphs.front().color>>24==0x5a&&glyphs.front().x<glyphs.back().x,"diagnostic banner order/alpha");
    okage::FieldRearBanner unbound(r,tables.system_message(0x72),flag);
    require(unbound.glyphs(r,fonts[2]).empty(),"no diagnostic text before the window binds group16");
}
}
int main(int argc,char** argv) {
    try {
        if(argc<2)throw std::runtime_error("usage: rear-encounter-tests <game-data-stage>");
        synthetic();
        const fs::path stage=argv[1];
        if(!fs::exists(stage/"NATIVE_UI/FLD.RUW")) {
            std::cout<<"rear-encounter-tests: synthetic tests passed; owned stage absent, skipped\n";return 0;
        }
        owned(stage);
        std::cout<<"rear-encounter-tests: synthetic and owned tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
