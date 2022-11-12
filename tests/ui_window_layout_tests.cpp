#include "okage/ui_layout.hpp"
#include "okage/ruw.hpp"
#include "okage/font.hpp"
#include "okage/battle_interface.hpp"
#include "okage/enemy.hpp"
#include <bit>
#include <cfenv>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace okage;
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t w){for(unsigned i=0;i<4;++i)b.at(at+i)=w>>(8*i);}
std::uint32_t bits(float f){return __builtin_bit_cast(std::uint32_t, (f));}
std::shared_ptr<RuwResource> resource(unsigned text_count=1,unsigned sprite_count=1){
    // One style, one template group, one widget group. Generic authored data.
    const unsigned count=text_count+sprite_count;
    std::vector<std::uint8_t> b(28+16+5*68+count*68);put(b,8,1);put(b,12,1);put(b,16,1);put(b,20,5);put(b,24,count);
    put(b,28,8);put(b,32,16);put(b,36,12);put(b,40,8);
    for(unsigned i=0;i<count;++i){const auto at=384+i*68;
        put(b,at,bits(1));put(b,at+4,bits(1));put(b,at+8,0x80808080);put(b,at+12,0x80808080);
        put(b,at+16,1);put(b,at+20,i<text_count?1:0);put(b,at+44,28);put(b,at+48,51);
        put(b,at+52,12);put(b,at+56,8);put(b,at+60,i);
    }
    return std::make_shared<RuwResource>(b);
}
std::shared_ptr<std::vector<Font>> fonts(){
    auto out=std::make_shared<std::vector<Font>>(3);
    for(auto& f:*out){f.first=32;f.last=126;f.glyphs.resize(95);
        for(unsigned i=0;i<95;++i)f.glyphs[i].metrics={std::uint16_t(i+32),0,0,192,8,8,200,392,192,384};
    }return out;
}
UiWindow window(std::shared_ptr<RuwResource> r,unsigned a=4,unsigned b=4){
    UiWindow w(a,b,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
    std::array<std::uint32_t,28> settings{};settings[0]=1;settings[1]=40;settings[2]=6;
    settings[3]=0;settings[20]=1;settings[21]=1;settings[25]=a;settings[26]=b;
    const std::array<UiWindow::AtlasTuple,2> atlas{{{0,0,16,16},{}}};
    w.configure(settings,atlas,std::make_shared<int>(1));
    const std::array<std::size_t,1> groups{0};w.bind_ruw(r,groups);return w;
}
void traversal(){
    auto r=resource();r->set_text(0,0,"\\f[1]\\s[0.5,1.0]AB ");auto w=window(r);auto f=fonts();
    std::array<float,360> sine{};UiLayoutShared shared;UiPacketBuildContext packets{f,64,32};unsigned draws=0;
    const auto old_round=std::fegetround();std::fesetround(FE_UPWARD);
    w.layout_mode2({shared,*f,sine,[&](){++draws;return 1u;},7,packets});
    require(std::fegetround()==FE_UPWARD,"Layout restores caller rounding");std::fesetround(old_round);
    require(w.word(0x218)==12&&w.word(0x21c)==24,"Mode2 skips metric command");
    require(w.word(0x269c)==7&&w.word(0x2edc)==8,"Text command cursor differs from source stream cursor");
    require(w.word(0xcdc)==3&&draws==33,"Three glyph placements then fifteen two-draw sentinel icons");
    require(w.glyph_buffer(1).word(8)==bits(65)&&w.glyph_buffer(1).word(24)==bits(66)&&
            w.glyph_buffer(1).word(40)==bits(32),"Control sequences do not consume glyph slots");
    require(r->widget(0,0).runtime->words[8]==std::uint32_t(-11)&&
            r->widget(0,0).runtime->words[9]==27,"Source text extent includes original coordinate subtraction");
    require(w.sprite_buffer(0).word(0)==bits(28)&&w.sprite_buffer(0).word(4)==bits(44),"Widget geometry uses resource offset");
    require(w.word(0xcc0)==1&&w.word(0xcc4)==1&&w.glyph_packet(0,w.word(0x268c))&&
            w.sprite_packet(0,w.word(0x2690)),"Traversal builds actual retained glyph and sprite packets");
    require(shared.layer==0&&shared.sprite_dirty&&shared.sprite_groups==0,"Shared final state follows builders");
    require(!w.glyph_cache(0,0).known_byte(0),"Typed packet does not invent raw guest DMA bytes");
    draws=0;w.layout_mode2({shared,*f,sine,[&](){++draws;return 2u;},7,packets});
    require(draws==33&&!shared.sprite_dirty,"Unchanged sprite cache still performs placement RNG");
}
void boundaries(){
    auto r=resource(1,0);auto f=fonts();std::array<float,360> sine{};UiLayoutShared shared;UiPacketBuildContext packets{f};
    r->set_text(0,0,"AAAAAAAAAAAAAAAA");auto w=window(r);unsigned draws=0;
    w.layout_mode2({shared,*f,sine,[&](){++draws;return 3u;},0,packets});
    require(draws==48&&w.word(0xcf0)==1&&w.word(0xcec)==0&&w.word(0xcc0)==2,"Exact glyph group wrap includes hidden following group");
    require(w.glyph_packet(0,w.word(0x268c))&&!w.glyph_packet(1,w.word(0x268c)),"Hidden group has no fabricated packet");
    r->set_text(0,0,"\\q[1]A");bool failed=false;
    try{w.layout_mode2({shared,*f,sine,[](){return 0u;},0,packets});}catch(const std::runtime_error&){failed=true;}
    require(failed,"Unrecovered controls remain explicit errors");
}
void preparation(){
    auto r=resource();r->set_text(0,0,"A");auto w=window(r);auto f=fonts();
    std::array<float,360> sine{};UiLayoutShared shared;UiPacketBuildContext packets{f};unsigned draws=0;
    UiLayoutContext context{shared,*f,sine,[&](){++draws;return 1u;},0,packets};
    w.set_opacity_tween(0,128,0,128);
    require(w.prepare_mode2(context)&&draws==31,"Dispatcher reaches layout with nonzero current opacity");
    require(w.background_packet()&&!(w.word(0x1900)&0x201)&&w.word(0x268c)==0&&w.word(0x2694)==0,
            "Background builds before layout and source packet sides toggle");
    require(!w.extra_cache(0).known_byte(0),"Background cache no longer falsely known-zero");
    w.set_layout_enabled(false);draws=0;
    require(!w.prepare_mode2(context)&&draws==0&&w.word(0x268c)==0,"Completed layout skips with disabled continuous layout");
    w.set_layout_enabled(true);w.set_opacity_tween(0,0,0,128);
    require(!w.prepare_mode2(context)&&draws==0&&w.word(0x268c)==1&&!(w.word(0x1900)&1),
            "Zero opacity and delta skip traversal but retain selector and flag side effects");
    w.set_opacity_tween(.2f,0,0,128);
    require(w.prepare_mode2(context)&&draws==31,"Nonzero delta requests layout even at zero current opacity");
    require(w.opacity()==0,"Preparation does not advance clock or opacity");
}
std::vector<std::uint8_t> read(const std::filesystem::path& p){
    std::ifstream in(p,std::ios::binary);require(bool(in),"Owned layout input missing");
    return {std::istreambuf_iterator<char>(in),{}};
}
void owned(const char* battle,const char* font_directory){
    const std::filesystem::path b=battle,f=font_directory;
    auto resource=std::make_shared<RuwResource>(read(b/"BTL.RUW"));
    const BattleUiData data(read(b/"battle-ui.bin"));
    auto fonts=std::make_shared<std::vector<Font>>();
    for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts->push_back(read_fnt(read(f/name)));
    BattleUiShared state;BattleInterface ui(state,data,std::make_shared<int>(1),0,[](unsigned){});
    BattleUiPad pad;ui.first_update(resource,pad,{},60);
    const auto sine=make_sine_table();UiLayoutShared layout;UiPacketBuildContext packets{fonts};GameRandom random{1};
    for(unsigned i=0;i<2;++i){auto& w=*state.registry[ui.handles()[i]];unsigned draws=0;
        w.advance_clock();require(w.prepare_mode2({layout,*fonts,sine,[&](){++draws;return random.next();},0,packets}),"Owned dispatcher reaches layout");
        std::cout<<"Owned constructor text slot="<<i<<" glyphs="<<w.word(0xcdc)<<" glyph_groups="<<w.word(0xcc0)
                 <<" sprite_groups="<<w.word(0xcc4)<<" rng_draws="<<draws<<'\n';
    }
}
}
int main(int argc,char** argv){try{traversal();boundaries();preparation();if(argc==3)owned(argv[1],argv[2]);else require(argc==1,"usage: ui-window-layout-tests [BATTLE_DIRECTORY FONT_DIRECTORY]");std::cout<<"UI mode2 layout tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
