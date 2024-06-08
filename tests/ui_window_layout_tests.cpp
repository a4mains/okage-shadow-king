#include "okage/ui_layout.hpp"
#include "okage/ruw.hpp"
#include "okage/font.hpp"
#include "okage/battle_interface.hpp"
#include "okage/enemy.hpp"
#include "okage/ui_draw.hpp"
#include "okage/ui_geometry.hpp"
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
std::uint32_t bits(float f){return std::bit_cast<std::uint32_t>(f);}
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
UiWindow window(std::shared_ptr<RuwResource> r,unsigned a=4,unsigned b=4,unsigned lines=6){
    UiWindow w(a,b,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
    std::array<std::uint32_t,28> settings{};settings[0]=1;settings[1]=40;settings[2]=lines;
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
void missing_multibyte(){
    auto r=resource(1,0);auto w=window(r);auto f=fonts();std::array<float,360> sine{};
    UiLayoutShared shared;UiPacketBuildContext packets{f};unsigned draws=0;
    r->set_text(0,0,std::string("A")+"\x81\x75"+"B"+"\x81\x76");
    const auto layout=[&](){w.layout_mode2({shared,*f,sine,[&](){++draws;return draws;},0,packets});};
    layout();
    require(w.word(0xcdc)==4&&draws==36,"Two-byte missing characters each retain one RNG-consuming glyph slot");
    const auto& meta=w.glyph_buffer(1);const auto& positions=w.glyph_buffer(0);
    require(meta.word(8)==bits(65)&&meta.word(24)==bits(-1)&&meta.word(40)==bits(66)&&meta.word(56)==bits(-1),
            "Wide punctuation consumes two bytes and leaves null slots between real glyphs");
    for(unsigned i=0;i<4;++i){
        require(meta.word(i*16)==bits(12)&&meta.word(i*16+4)==bits(i%2?24.f:48.f),
                "Null wide glyph retains parser dimensions independently of real glyph output height");
        if(i)require(std::bit_cast<float>(positions.word(i*16))-std::bit_cast<float>(positions.word((i-1)*16))==12,
                     "Missing glyph advances by prior width without collapsing following text");
    }
    require(!shared.glyph_metric,"Last null wide metric clears shared metric ownership");
    draws=0;layout();require(draws==36,"Unchanged missing glyph slots still consume placement RNG");
    r->set_text(0,0,"\x81\x75");draws=0;layout();
    require(w.word(0xcdc)==1&&draws==33&&meta.word(8)==bits(-1)&&meta.word(0)==bits(0)&&meta.word(4)==bits(0),
            "Initial null wide glyph retains zero initial dimensions and still places a slot");
    r->set_text(0,0,"\x81");draws=0;layout();
    require(w.word(0xcdc)==1&&draws==33&&meta.word(8)==bits(-1),"Zero trail is consumed as a wide code, not ASCII termination");
    w.set_transform_parameter(4);draws=0;bool rejected=false;
    try{layout();}catch(const std::runtime_error&){rejected=true;}
    require(rejected&&draws==0,"Alternate multibyte encoding rejects before glyph placement or RNG");
}
void newlines(){
    auto r=resource(1,0);auto f=fonts();std::array<float,360> sine{};
    r->set_position(0,0,47,58);
    r->set_text(0,0,"\\h[17,9]\\s[1,0.5]AB\\nC\\n\\n");
    auto w=window(r,4,4,2);UiLayoutShared shared;UiPacketBuildContext packets{f};unsigned draws=0;
    w.layout_mode2({shared,*f,sine,[&](){++draws;return 1u;},0,packets});
    const auto& p=w.glyph_buffer(0);const auto& m=w.glyph_buffer(1);
    require(p.word(0)==bits(43)&&p.word(16)==bits(55)&&p.word(32)==bits(43),
            "Newline restores widget X without consuming a glyph slot");
    require(p.word(4)==bits(67)&&p.word(20)==bits(67)&&p.word(36)==bits(76),
            "Newline uses the integer line metric independently of vertical glyph scaling");
    require(m.word(8)==bits(65)&&m.word(24)==bits(66)&&m.word(40)==bits(67)&&m.word(36)==bits(24),
            "Newline preserves the selected font and glyph dimensions");
    require(w.word(0xcdc)==3&&w.word(0xcec)==3&&draws==35,
            "Only three glyphs and sixteen sentinel icons consume RNG");
    require(w.word(0x250)==2&&w.word(0x254)==4&&w.word(0x260)==18,
            "Consecutive trailing newlines extend line count and scroll extent at the source threshold");
    require(w.word(0xcb0)==43&&w.word(0xcb4)==9,
            "Final cursor reflects trailing newline X and clamps Y to the configured last line");
    require(r->widget(0,0).runtime->words[8]==std::uint32_t(-16)&&r->widget(0,0).runtime->words[9]==18,
            "Trailing newline does not fabricate a per-glyph widget extent update");
    require(w.word(0x269c)==5,"Newlines do not add typed stream entries");

    r=resource(1,0);r->set_text(0,0,"\\n");auto retained=window(r);draws=0;
    retained.layout_mode2({shared,*f,sine,[&](){++draws;return 1u;},0,packets});
    require(retained.word(0xcdc)==9&&draws==41&&retained.glyph_buffer(1).word(8)==bits(49)&&
            retained.glyph_buffer(1).word(8*16+8)==bits(103),
            "Terminal control consumes its NUL and parses the source-preserved previous text tail");
    require(retained.glyph_buffer(0).word(4)==bits(84),"Retained text begins on the advanced line");

    r=resource(1,0);r->set_text(0,0,"ABC");r->set_text(0,0,"\\n");auto empty=window(r);draws=0;
    empty.layout_mode2({shared,*f,sine,[&](){++draws;return 1u;},0,packets});
    require(empty.word(0xcdc)==0&&empty.word(0xcec)==0&&draws==32,
            "A terminal newline allocates no glyph and consumes only sentinel RNG");
    require(empty.word(0xcb0)==24&&empty.word(0xcb4)==48&&empty.word(0x260)==0,
            "Empty terminal newline advances from widget Y using the default line metric");
    require(!empty.glyph_packet(0,empty.word(0x268c)),"Empty newline emits no glyph packet");

    r=resource(1,0);r->set_text(0,0,"A\\n" "\x81\x75");auto missing=window(r);draws=0;
    missing.layout_mode2({shared,*f,sine,[&](){++draws;return 1u;},0,packets});
    require(missing.glyph_buffer(1).word(24)==bits(-1)&&missing.glyph_buffer(1).word(16)==bits(12)&&
            missing.glyph_buffer(1).word(20)==bits(24),
            "Newline preserves parser width and height for a following missing wide glyph");
    require(missing.glyph_buffer(0).word(20)==bits(84)&&draws==34,
            "Missing glyph occupies the new line and retains its actual RNG call");
}
void boundaries(){
    {
        auto r=resource(1,0);auto w=window(r);auto f=fonts();std::array<float,360> sine{};
        UiLayoutShared shared;UiPacketBuildContext packets{f};
        r->set_text(0,0,"\\h[29,0]\\f[2]AB");w.set_transform_parameter(128);
        w.layout_mode2({shared,*f,sine,[](){return 0u;},0,packets});
        require(w.word(0x244)==29&&w.word(0x218)==29&&w.word(0x248)==0&&w.word(0x21c)==0,"Metric control updates saved/current pairs");
        const auto a=std::bit_cast<float>(w.glyph_buffer(0).word(0)),b=std::bit_cast<float>(w.glyph_buffer(0).word(16));
        require(b-a==29,"Centered text uses authored horizontal advance");
        r->set_text(0,0,"\\h[ -3,+7]A");w.layout_mode2({shared,*f,sine,[](){return 0u;},0,packets});
        require(w.word(0x218)==std::uint32_t(-3)&&w.word(0x21c)==7,"Metrics parse signed decimal");
    }
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
void constructor_stream(){
    auto f=fonts();std::array<float,360> sine{};UiLayoutShared shared;UiPacketBuildContext packets{f};
    auto stream=std::make_shared<UiWindowStream>(UiWindowStream{1,{
        UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
        UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}}});
    UiWindow w(4,4,0,stream);
    std::array<std::uint32_t,28> settings{};settings[0]=1;settings[1]=40;settings[2]=6;
    settings[20]=1;settings[21]=1;settings[25]=4;settings[26]=4;
    const std::array<UiWindow::AtlasTuple,2> atlas{{{0,0,16,16},{}}};
    w.configure(settings,atlas,std::make_shared<int>(1));
    w.set_opacity_tween(0,128,0,128);
    unsigned draws=0;auto random=[&](){++draws;return 1u;};
    require(w.word(0x3bec)==0,"Initial results constructor stream remains mode0 before RUW binding");
    require(w.prepare_mode2({shared,*f,sine,random,0,packets}),"Dispatcher runs the bounded constructor stream");
    require(w.word(0x218)==12&&w.word(0x21c)==24&&w.word(0xcd4)==0xfffffc18u,
            "Constructor14/6 applies authored metrics and signed reveal period");
    require(w.word(0x269c)==4&&w.word(0x2edc)==0&&w.flags().word(0)==0x202,
            "Mode0 advances its command cursor, leaves the later RUW cursor untouched, and marks both source command bytes");
    require(w.word(0xcc0)==1&&w.word(0xcc4)==0&&(w.word(0x1900)&0xa2)==0xa2,
            "Mode0 runs the empty glyph/sprite footer and terminal flags");
    require(draws==32&&shared.layer==0&&!shared.sprite_dirty,
            "Sixteen sentinel icon placements retain their two source RNG draws each");
    require(!w.glyph_packet(0,0),"An all-sentinel glyph pass does not fabricate a packet");

    auto r=resource();r->set_text(0,0,"A");const std::array<std::size_t,1> groups{0};w.bind_ruw(r,groups);
    w.set_opacity_tween(0,128,0,128);draws=0;
    require(w.word(0x3bec)==2&&w.prepare_mode2({shared,*f,sine,random,0,packets}),
            "The same window reuses mode2 after its later RUW bind");
    require(w.word(0x218)==12&&w.word(0x21c)==24&&w.word(0x269c)==7,
            "Later mode2 still applies its separate typed stream semantics");

    auto wrong=std::make_shared<UiWindowStream>(*stream);wrong->entries.push_back(UiStreamText{{0}});
    UiWindow unsupported(4,4,0,wrong);unsupported.configure(settings,atlas,std::make_shared<int>(1));
    unsupported.set_opacity_tween(0,128,0,128);bool failed=false;
    try{unsupported.prepare_mode2({shared,*f,sine,random,0,packets});}catch(const std::runtime_error&){failed=true;}
    require(failed,"Other pre-bind mode0 stream shapes remain explicitly guarded");
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
    UiDrawState draw_state;
    const auto draw_list=build_ui_draw_list(state.registry,draw_state,initial_ui_projection(),sine);
    std::array<unsigned,3> replay_counts{};
    for(const auto& command:draw_list.commands())if(const auto* replay=std::get_if<UiDrawReplay>(&command))
        ++replay_counts[unsigned(replay->family)];
    require(replay_counts[0]>0&&replay_counts[1]>0&&replay_counts[1]==replay_counts[2],
            "Owned layout reaches glyph and paired sprite header/element submissions");
    std::cout<<"Owned draw commands="<<draw_list.commands().size()<<" glyph_replays="<<replay_counts[0]
             <<" sprite_header_replays="<<replay_counts[1]<<" sprite_element_replays="<<replay_counts[2]<<'\n';
    const auto geometry=resolve_ui_geometry(draw_list);unsigned primitives=0;
    for(const auto& command:geometry.commands)if(std::holds_alternative<UiResolvedPrimitive>(command))++primitives;
    require(primitives>0,"Owned UI draw resolves projected primitives");
    std::cout<<"Owned projected primitives="<<primitives<<'\n';
}
}
int main(int argc,char** argv){try{traversal();missing_multibyte();newlines();boundaries();preparation();constructor_stream();if(argc==3)owned(argv[1],argv[2]);else require(argc==1,"usage: ui-window-layout-tests [BATTLE_DIRECTORY FONT_DIRECTORY]");std::cout<<"UI mode0/2 layout tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
