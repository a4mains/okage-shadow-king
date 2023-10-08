#include "okage/ui_window.hpp"
#include "okage/font.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <iostream>
#include <fstream>
#include <stdexcept>
namespace okage {
struct UiWindowPacketTestAccess {
    static void set(UiWindow& w,unsigned at,std::uint32_t v){w.put(at,v);}
    static void glyph(UiWindow& w,unsigned group,unsigned slot,std::array<float,4> p,std::array<float,4> m){for(unsigned i=0;i<4;++i){w.glyph_[0].put_word((group*16+slot)*16+i*4,std::bit_cast<std::uint32_t>(p[i]));w.glyph_[1].put_word((group*16+slot)*16+i*4,std::bit_cast<std::uint32_t>(m[i]));}}
    static void sprite(UiWindow& w,unsigned slot,std::array<float,4> p,std::array<float,4> m){for(unsigned i=0;i<4;++i){w.sprite_[0].put_word(slot*16+i*4,std::bit_cast<std::uint32_t>(p[i]));w.sprite_[1].put_word(slot*16+i*4,std::bit_cast<std::uint32_t>(m[i]));}}
};
}
namespace {
using namespace okage;using K=UiPacketOperationKind;using V=std::array<float,4>;
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected packet guard");}
std::uint32_t bits(float f){return std::bit_cast<std::uint32_t>(f);}
void put(UiWindow& w,unsigned at,std::uint32_t v){UiWindowPacketTestAccess::set(w,at,v);}
void fput(UiWindow& w,unsigned at,float f){put(w,at,bits(f));}
UiWindow window(){return UiWindow(2,2,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));}
void initialize_glyphs(UiWindow& w){for(unsigned g=0;g<2;++g)for(unsigned s=0;s<16;++s)UiWindowPacketTestAccess::glyph(w,g,s,{0,0,0,0},{0,0,-1,0});}
const UiPacketOperation& array(const std::vector<UiPacketOperation>& ops,K kind){auto it=std::find_if(ops.begin(),ops.end(),[&](const auto& x){return x.kind==kind;});require(it!=ops.end(),"Missing typed packet array");return *it;}
std::vector<UiPacketOperation> select(const std::vector<UiPacketOperation>& ops,K kind){std::vector<UiPacketOperation> out;for(auto& o:ops)if(o.kind==kind)out.push_back(o);return out;}
UiPacketBuildContext context(){auto fonts=std::make_shared<std::vector<Font>>(1);auto& f=(*fonts)[0];f.first=65;f.last=66;f.glyphs.resize(2);f.atlas.width=256;f.atlas.height=256;
    f.glyphs[0].metrics={0,0,0,0,168,328,488,648,0,0};
    f.glyphs[1].metrics={0,0,0,0,328,488,648,808,0,0};return {fonts,64,32};}
void glyph_build_and_ownership(){auto w=window();initialize_glyphs(w);auto c=context();
    fput(w,0x560,1);fput(w,0x564,2);fput(w,0x568,3);put(w,0x268c,0);put(w,0x1900,w.word(0x1900)&~6u);
    for(unsigned i=0;i<4;++i){fput(w,0x868+i*16,float(i));fput(w,0x870+i*16,float(i+2));}fput(w,0x86c,5);fput(w,0x87c,99);fput(w,0x88c,98);fput(w,0x89c,97);
    // Packed6 RGB/A all32, source decoded normalized channels all1.
    UiWindowPacketTestAccess::glyph(w,0,0,{10.75f,30.75f,7,8521760},{20,10,65,0});
    require(w.build_glyph_packet(c)==1&&w.word(0x3e24)==1,"Glyph build and cursor");
    require((w.word(0x3c08)&6)==2,"Glyph clears available4/sets built2");
    auto* p=w.glyph_packet(0,0);require(p&&p->fonts==c.fonts&&!w.glyph_packet(0,1),"Selected-side owner retains fonts");
    require(array(p->operations,K::Positions).first_positions_upload==true,"Glyph first upload flag retained at compilation");
    require(!array(p->operations,K::Colors).first_positions_upload,"Nonposition operation has no upload flag");
    const auto& pos=array(p->operations,K::Positions).values;
    require(pos.size()==64&&pos[0]==V{9,33,6,1}&&pos[1]==V{30,33,7,1}&&pos[2]==V{31,43,8,1}&&pos[3]==V{12,43,9,1},"Geometry truncation and unusual shared Y offset");
    require(pos[4]==V{0,0,0,0}&&pos[5]==V{1,0,0,0},"Hidden glyph placeholder matrices");
    const auto& colors=array(p->operations,K::Colors).values;require(colors[0]==V{1,1,1,1}&&colors[1]==colors[0],"Packed color decode");
    const auto& uv=array(p->operations,K::UV).values;
    require(uv[0]==V{10.5f/1024,20.5f/1024,0,0}&&uv[2]==V{30.5f/1024,40.f/1024,0,0},"Authored8/8 UV subtraction and half-edge distinction");
    const auto vertices=select(p->operations,K::IndexedVertex);require(vertices.size()==4&&vertices[2].arguments==std::array<std::uint64_t,4>{2,0,0,2},"Glyph primitive indices");
    const auto textures=select(p->operations,K::Texture);require(textures.size()==1&&textures[0].arguments[1]==0&&textures[0].arguments[2]==0,"Typed font binding");
    require(!w.glyph_cache(0,0).known_byte(0)&&!w.known_word(0x3d0c),"No fabricated guest cache bytes/pointers");
    const auto first=p;put(w,0x3e24,0);put(w,0x268c,1);w.reset_glyph_packets();w.build_glyph_packet(c);
    require(w.glyph_packet(0,0)==first&&w.glyph_packet(0,1),"Both selected cache generations retained");
}
void cursor_and_hidden(){auto w=window();auto c=context();
    rejects([&]{w.build_glyph_packet(c);});require(!w.glyph_packet(0,1),"Unknown glyph Y not invented");initialize_glyphs(w);
    put(w,0x3e24,5);put(w,0xcc0,99);put(w,0xcc4,88);put(w,0x3c08,0);put(w,0x3c0c,0);
    w.reset_glyph_packets();require(w.word(0x3e24)==5&&w.word(0xcc0)==0&&w.word(0xcc4)==88&&(w.word(0x3c08)&4),"58f8 preserves cursor/other counter");
    require(w.build_glyph_packet({})==0&&w.word(0x3e24)==2,"All-hidden modulo group selection without font owner");
    put(w,0x3e24,1);put(w,0x3c08,4);put(w,0x3c0c,0);
    require(w.build_glyph_packet({})==0&&w.word(0x3e24)==1,"Unavailable current group searches from group0");
    require(!w.glyph_packet(0,0)&&!w.glyph_packet(0,1),"Hidden scan creates no cache");
    put(w,0x3e24,0xffffffff);rejects([&]{w.build_glyph_packet(c);});
}
void results_glyph_capacity() {
    UiWindow w(33,17,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
    for(unsigned g=0;g<33;++g)for(unsigned slot=0;slot<16;++slot)
        UiWindowPacketTestAccess::glyph(w,g,slot,{0,0,0,0},{0,0,-1,0});
    const auto c=context();put(w,0x3e24,32);put(w,0x268c,0);
    UiWindowPacketTestAccess::glyph(w,32,0,{10,30,7,8521760},{20,10,65,0});
    require(w.build_glyph_packet(c)==1,"Results33rd glyph group builds");
    require(w.word(0x3e24)==33&&(w.word(0x3c88)&6)==2,"Results33rd glyph cursor/flags");
    require(w.glyph_packet(32,0)&&w.glyph_packet(32,0)->fonts==c.fonts,"Results33rd glyph cache retains font");
    require(!w.glyph_packet(31,0),"Results33rd group does not alias its neighbor");
}
void glyph_special_branches(){auto w=window();initialize_glyphs(w);auto c=context();put(w,0x268c,0);fput(w,0x560,0);fput(w,0x564,0);
    UiWindowPacketTestAccess::glyph(w,0,0,{0,12,0,8521760},{10,20,256,0});put(w,0x4f8,4);
    w.build_glyph_packet(c);auto* p=w.glyph_packet(0,0);const auto& uv=array(p->operations,K::UV).values;
    require(uv[0][0]==10.5f/1024,"Packet encoded index subtracts1 unlike placement helper");
    const auto& colors=array(p->operations,K::Colors).values;require(colors[0][3]==.5f&&colors[1][3]==1,"Scroll top edge alpha factors");
    const auto vertices=select(p->operations,K::IndexedVertex);require(vertices[2].arguments[2]==1&&vertices[3].arguments[2]==1,"Top edge lower vertices use second color");
    put(w,0x3e24,0);w.reset_glyph_packets();UiWindowPacketTestAccess::glyph(w,0,0,{0,30,0,8521760},{10,20,67,0});put(w,0x4f8,0);
    require(w.build_glyph_packet(c)==1&&array(w.glyph_packet(0,0)->operations,K::Positions).values[0]==V{0,0,0,1},"Source missing glyph metrics still builds zero geometry");
    put(w,0x3e24,0);w.reset_glyph_packets();put(w,0x860,1);put(w,0xcf0,0);put(w,0xcec,0);
    rejects([&]{w.build_glyph_packet(c);});put(w,0x860,0);put(w,0xcd4,0);rejects([&]{w.build_glyph_packet(c);});
}
void reveal_colors(){auto w=window();initialize_glyphs(w);auto c=context();put(w,0x268c,0);put(w,0xcd4,4);put(w,0xcd0,1);
    UiWindowPacketTestAccess::glyph(w,0,0,{0,30,0,8521760},{10,20,65,0});w.build_glyph_packet(c);
    auto* p=w.glyph_packet(0,0);const auto& colors=array(p->operations,K::Colors).values;
    require(colors[0][3]==.5f&&colors[1][3]==0,"Selected glyph uses separate reveal ramp colors");
    const auto vertices=select(p->operations,K::IndexedVertex);require(vertices[0].arguments[2]==0&&vertices[1].arguments[2]==1&&vertices[3].arguments[2]==0,"Reveal assigns middle corners second color");
    put(w,0x3e24,0);put(w,0xcd0,0);w.reset_glyph_packets();w.build_glyph_packet(c);
    require(bits(array(w.glyph_packet(0,0)->operations,K::Colors).values[0][3])==0x3a83126f,"Zero phase authored epsilon");
    put(w,0x3e24,0);put(w,0xcd4,0xffffffff);w.reset_glyph_packets();w.build_glyph_packet(c);
    require(array(w.glyph_packet(0,0)->operations,K::Colors).values[0][3]==1,"Negative period bypass after arithmetic");
}
void owned_font(const char* path){std::ifstream input(path,std::ios::binary);require(bool(input),"Owned font input");std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};auto fonts=std::make_shared<std::vector<Font>>();fonts->push_back(read_fnt(bytes));UiPacketBuildContext c{fonts,64,32};auto w=window();initialize_glyphs(w);put(w,0x268c,0);unsigned groups=0,placed=0;
    for(unsigned base=(*fonts)[0].first;base<=(*fonts)[0].last;base+=16){for(unsigned i=0;i<16;++i){unsigned code=base+i;UiWindowPacketTestAccess::glyph(w,0,i,{float(20+i*20),40,0,8521760},{16,24,code<=(*fonts)[0].last?float(code):-1.f,0});if(code<=(*fonts)[0].last)++placed;}put(w,0x3e24,0);w.reset_glyph_packets();groups+=w.build_glyph_packet(c);}
    std::cout<<"Owned font glyphs="<<placed<<" nonempty packets="<<groups<<'\n';
}
void bind_texture(UiWindow& w){std::array<std::uint32_t,28> settings{};settings[0]=1;settings[21]=1;settings[25]=2;settings[26]=2;std::array<UiWindow::AtlasTuple,2> atlas{{{0,0,16,16},{}}};w.configure(settings,atlas,std::make_shared<int>(42));}
void sprite_build(){auto w=window();bind_texture(w);auto c=context();put(w,0x2690,1);fput(w,0x560,2);fput(w,0x564,3);fput(w,0x568,4);
    put(w,0xc,10);put(w,0x8c,20);put(w,0x10c,30);put(w,0x18c,40);
    for(unsigned i=0;i<4;++i)put(w,0x2e0+4*i,0x80402010);
    UiWindowPacketTestAccess::sprite(w,0,{100,12,1,0},{45,50,0,123});
    UiWindowPacketTestAccess::sprite(w,1,{100,100,131075,0},{2,3,34,655380}); //dim2x3, UV10x20.
    UiWindowPacketTestAccess::sprite(w,2,{1,2,3,0},{4,5,32,6});
    UiWindowPacketTestAccess::sprite(w,3,{0,0,7,0},{8,9,33,0});
    UiWindowPacketTestAccess::sprite(w,4,{0,0,0,0},{0,0,99,0});
    require(!w.sprite_cache(0,1).known_byte(0),"Raw sprite cache starts unknown");
    w.build_sprite_packet(c);require(w.word(0x3e28)==1&&!(w.word(0x3c08)&8),"Sprite cursor/available flag");
    auto* p=w.sprite_packet(0,1);require(p&&p->window_texture==w.texture_owner(),"Sprite cache retains native texture");
    const auto& colors=array(p->operations,K::Colors).values;require(colors.size()==5&&colors[0]==V{255,255,255,0}&&colors[1]==V{.125f,.25f,.5f,.5f}&&colors[3][3]==.25f,"Background palette and explicit shared alphas");
    const auto& uv=array(p->operations,K::UV).values;require(uv[0]==V{10.5f/1024,32.5f/1024,0,0}&&uv[2][1]==60.f/1024,"Icon top-clipped UV preserves bottom");
    require(uv[4]==V{10.5f/1024,20.5f/1024,0,0}&&uv[6]==V{12.5f/1024,23.f/1024,0,0},"Packed panel dimensions/UV unpack numerically");
    require(array(*p->elements[0],K::Positions).values==std::vector<V>{{-2,21,-3,1},{43,21,-3,1},{43,47,-3,1},{-2,47,-3,1}},"Icon local geometry");
    require(array(*p->elements[1],K::Positions).values==std::vector<V>{{-2,-3,-4,1},{2,-3,-4,1},{2,6,-4,1},{-2,6,-4,1}},"Panel scaled local geometry");
    for(unsigned i=0;i<5;++i)require(array(*p->elements[i],K::Positions).first_positions_upload==true,"Every actual sprite subpacket retains its own first-upload flag");
    const auto line=select(*p->elements[2],K::IndexedVertex);require(line.size()==4&&line[3].arguments[0]==3,"Selector32 branches directly to finalize after four vertices");
    const auto gradient=select(*p->elements[3],K::IndexedVertex);require(gradient[0].arguments[2]==1&&gradient[2].arguments[2]==2,"Selector33 two-color primitive");
    require(array(*p->elements[4],K::Positions).values==std::vector<V>(4,V{}),"Other selector has actual degenerate geometry");
    require(!p->elements[5]&&!w.known_word(0x66c0)&&w.word(0x66c0+20)==0xffffffff,"Typed packet pointer excluded, sentinel known");
    require(w.word(0x5ec0)==0&&!w.sprite_cache(0,1).known_byte(0),"Opposite selector and raw packet heap preserved");
    put(w,0xcc4,22);const auto old=w.word(0x3e28);w.reset_sprite_packets();require(w.word(0xcc4)==0&&w.word(0x3e28)==old&&(w.word(0x3c08)&8)&&w.sprite_packet(0,1)==p,"Reset marks flags without deleting native cache");
    put(w,0x3e28,2);rejects([&]{w.build_sprite_packet(c);});
}
}
int main(int argc,char** argv){try{glyph_build_and_ownership();cursor_and_hidden();results_glyph_capacity();glyph_special_branches();reveal_colors();sprite_build();for(int i=1;i<argc;++i)owned_font(argv[i]);std::cout<<"UI window packet tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
