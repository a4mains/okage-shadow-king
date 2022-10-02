#include "okage/ui_layout_geometry.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace okage;
namespace {
void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected geometry rejection");}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t n){for(unsigned i=0;i<4;++i)b.at(at+i)=n>>(i*8);}
std::vector<std::uint8_t> fixture(bool missing_corner=false,unsigned count=11) {
    std::vector<std::uint8_t> b(24+7*16+count*68);put(b,8,7);put(b,12,1);put(b,20,count);
    const std::array<std::array<int,4>,7> styles={{{10,20,6,5},{30,40,5,6},{50,60,7,5},{70,80,8,6},{90,100,4,4},{110,120,3,2},{130,140,3,3}}};
    for(unsigned i=0;i<7;++i)for(unsigned j=0;j<4;++j)put(b,24+i*16+j*4,styles[i][j]);
    for(unsigned i=0;i<count;++i){const auto at=24+7*16+i*68;put(b,at+28,i<4?i:4);put(b,at+20,missing_corner&&i==0?2:0);}
    return b;
}
RuwWidget widget(int type,int x=100,int y=200,int w=40,int h=30,int split=40,bool panel=false) {
    RuwWidget out;out.words[0]=out.words[1]=0x3f800000;out.words[2]=0x80402010;out.words[3]=0x80ffffff;
    out.words[4]=1|(panel?0x80:0);out.words[5]=type;out.words[6]=split;out.words[7]=0;
    out.words[11]=x;out.words[12]=y;out.words[13]=w;out.words[14]=h;return out;
}
void eq(const UiLayoutSprite& o,int x,int y,int w,int h,int u,int v,float sx,float sy,const char* why) {
    require(o.x==x&&o.y==y&&o.width==w&&o.height==h&&o.u==u&&o.v==v&&o.style.scale_x==sx&&o.style.scale_y==sy,why);
}
void checks() {
    RuwResource r(fixture());UiLayoutSpriteStyle initial{2,3,{0x12345678,UiLayoutColorConversion::Direct}};
    auto a=widget(0);a.words[7]=6;a.words[0]=0x40000000;a.words[1]=0x40400000;
    auto batch=layout_ruw_sprites(r,a,7,initial);require(batch.sprites.size()==1,"simple count");
    eq(batch.sprites[0],100,193,3,3,130,140,2,3,"simple fields");
    require(batch.final_style.scale_x==2&&batch.final_style.scale_y==3&&batch.final_style.color.conversion==UiLayoutColorConversion::Convert19bdf8,"simple retains style");
    a.words[7]=std::uint32_t(-8);batch=layout_ruw_sprites(r,a,0); //abs(-8+2)=6, fallback4.
    eq(batch.sprites[0],100,200,4,4,90,100,1.5f,2.25f,"negative selector uses fallback geometry with scaled target dimensions");
    a.words[7]=std::uint32_t(-1);batch=layout_ruw_sprites(r,a,0,initial);
    require(batch.sprites.empty()&&batch.final_style.color==initial.color&&batch.final_style.scale_x==2,"selector-1 preserves style");
    a=widget(0);a.words[4]=0;batch=layout_ruw_sprites(r,a,0,initial);require(batch.sprites.empty()&&batch.final_style.scale_y==3,"hidden no-op");
    a=widget(0);a.runtime.emplace();a.runtime->words={6,0,0x3f000000,0xbf800000,0x10203040,0,7,9,300,400};
    batch=layout_ruw_sprites(r,a,3);eq(batch.sprites[0],7,6,3,3,130,140,.5f,-1,"mutable payload overrides inline fields");
    require(a.runtime->words[8]==300,"input immutable");
    a=widget(2,100,200,0,12);batch=layout_ruw_sprites(r,a,7);
    eq(batch.sprites[0],86,192,4,4,90,100,.25f,3,"vertical rectangle");
    require(batch.sprites[0].style.color.conversion==UiLayoutColorConversion::Direct&&batch.final_style.scale_x==1,"rectangle direct color/reset");
    a.words[13]=20;a.words[14]=999;batch=layout_ruw_sprites(r,a,7);eq(batch.sprites[0],86,192,4,4,90,100,5,.5,"horizontal rectangle ignores height");
    a.words[11]=0x80000000;batch=layout_ruw_sprites(r,a,0);require(batch.sprites[0].x==2147483634,"signed position subtraction wraps");
    a=widget(3);batch=layout_ruw_sprites(r,a,7);require(batch.sprites.size()==7,"seven-piece ignores later template ordinals");
    eq(batch.sprites[1],135,193,5,6,30,40,1,1,"frame top right");
    eq(batch.sprites[3],132,217,8,6,70,80,1,1,"frame bottom right");
    eq(batch.sprites[4],106,193,4,4,90,100,7.25f,7.5f,"frame center");
    eq(batch.sprites[6],135,198,4,4,90,100,1.25f,4.5f,"frame right edge uses right corner heights");
    a.words[13]=1;a.words[14]=1;batch=layout_ruw_sprites(r,a,0);require(batch.sprites[4].style.scale_x==2.5f&&batch.sprites[5].style.scale_y==2.25f,"frame negative spans absolute after division");
    a=widget(3,-3000,-3000);batch=layout_ruw_sprites(r,a,0);require(batch.sprites.size()==6&&batch.sprites[0].piece==1,"panel coordinate sentinel only skips coincident pair");
    a=widget(0,-3000,-3000);batch=layout_ruw_sprites(r,a,0);require(batch.sprites.size()==1,"simple sprite does not share panel sentinel");
    a=widget(3,100,200,40,30,40,true);batch=layout_ruw_sprites(r,a,0);require(batch.sprites.size()==7,"full split panel has seven candidates");
    for(const auto& op:batch.sprites)require(op.style.color.rgba==a.words[2],"full split first color");
    a.words[6]=0;batch=layout_ruw_sprites(r,a,0);for(const auto& op:batch.sprites)require(op.style.color.rgba==a.words[3],"empty split second color");
    a.words[6]=20;batch=layout_ruw_sprites(r,a,0);require(batch.sprites.size()==8,"interior split eight candidates");
    eq(batch.sprites[4],106,200,4,4,90,100,3.5f,7.5f,"interior left fill");
    eq(batch.sprites[5],120,200,4,4,90,100,3.75f,7.5f,"interior right fill");
    a.words[6]=3;batch=layout_ruw_sprites(r,a,0);require(batch.sprites.size()==11,"left-edge complementary pieces");
    eq(batch.sprites[8],103,200,3,5,13,20,1,1,"left upper UV trim");
    eq(batch.sprites[9],103,225,4,5,53,60,1,1,"left lower UV trim");
    require(batch.sprites[4].style.scale_x==1.5f,"source left-edge center recipe uses whole left corner width");
    a.words[6]=38;batch=layout_ruw_sprites(r,a,0);
    eq(batch.sprites[3],138,224,2,6,33,80,1,1,"right-bottom U comes from top-right texture while V stays bottom");
    eq(batch.sprites[8],135,200,3,6,30,40,1,1,"right upper complementary crop");
    eq(batch.sprites[9],132,224,6,6,70,80,1,1,"right lower complementary crop");
    require(batch.sprites[5].style.scale_x==0,"zero width source operation retained");
    a=widget(3,100,200,36,5,36,true);batch=layout_ruw_sprites(r,a,0);
    require(batch.sprites.size()==2&&batch.sprites[0].texture_index==4&&batch.sprites[1].style.scale_x==0,"thin full panel retains second zero-width piece");
    eq(batch.sprites[0],100,200,4,4,90,100,9,1.25f,"thin fill uses T4");
    a=widget(3,100,20,40,30,-10,true);batch=layout_ruw_sprites(r,a,0);
    require(batch.sprites.size()==5,"vertical dispatch only five emitting recipes");
    eq(batch.sprites[4],100,25,4,4,90,100,2.5f,2.5f,"vertical fill retains source Y-dependent numerator");
    auto empty=fixture(false,0);rejects([&]{layout_ruw_sprites(RuwResource(empty),widget(0),0);});
    rejects([&]{layout_ruw_sprites(RuwResource(fixture(true)),widget(3),0);});
    rejects([&]{layout_ruw_sprites(r,widget(1),0);});
    a=widget(0);a.words[7]=100;rejects([&]{layout_ruw_sprites(r,a,0);});
    auto zero=fixture();put(zero,24+4*16+8,0);rejects([&]{layout_ruw_sprites(RuwResource(zero),widget(2),0);});
    a=widget(0);a.words[0]=1;rejects([&]{layout_ruw_sprites(r,a,0);});
    a.words[0]=0x7f800000;rejects([&]{layout_ruw_sprites(r,a,0);});
    a.words[0]=0x7f7fffff;a.words[7]=std::uint32_t(-2);rejects([&]{layout_ruw_sprites(r,a,0);});
    auto thirds=fixture();put(thirds,24+4*16+8,3);put(thirds,24+8,1);
    RuwResource third_resource(thirds);a=widget(0);a.words[7]=std::uint32_t(-2);a.words[0]=0x40400000;
    batch=layout_ruw_sprites(third_resource,a,0);
    require(__builtin_bit_cast(std::uint32_t, (batch.sprites[0].style.scale_x))==0x3f7fffff,"division rounds before authored scale multiply");
    a=widget(2,0,0,16777219,1);batch=layout_ruw_sprites(third_resource,a,0);
    require(__builtin_bit_cast(std::uint32_t, (batch.sprites[0].style.scale_x))==0x4aaaaaac,"CVT.S.W toward-zero precedes division");
    a=widget(0);a.words[0]=0x00800000;a.words[7]=std::uint32_t(-2);
    rejects([&]{layout_ruw_sprites(third_resource,a,0);});
    const auto convert=[](std::uint32_t c){return ui_layout_color_rgba({c,UiLayoutColorConversion::Convert19bdf8});};
    require(convert(0x80f8f8f7)==0x80747473&&convert(0x80f8f8f8)==0x7f808080,"packed white interval lower boundary");
    require(convert(0x80ff0000)==0x7f808080,"packed near-white interval is not per-channel");
    require(convert(0x00ffffff)==0x00808080&&convert(0x01100f11)==0x01000000,"alpha saturation and channel clamps");
    require(ui_layout_color_rgba({0x12345678,UiLayoutColorConversion::Direct})==0x12345678,"direct color preserved");
    std::fenv_t saved;std::fegetenv(&saved);std::fesetround(FE_UPWARD);std::feraiseexcept(FE_INVALID);
    a=widget(2,0,0,1,1);batch=layout_ruw_sprites(r,a,0);
    require(std::fegetround()==FE_UPWARD&&(std::fetestexcept(FE_INVALID)&FE_INVALID),"full host fenv restored");std::fesetenv(&saved);
}
std::vector<std::uint8_t> read(const char* p){std::ifstream f(p,std::ios::binary);require(bool(f),"owned RUW input missing");return {(std::istreambuf_iterator<char>(f)),{}};}
}
int main(int argc,char** argv) {
    try {
        checks();
        if(argc==2){RuwResource r(read(argv[1]));std::size_t records=0,ops=0;
            for(unsigned group:{2u,3u,4u,5u})for(const auto& w:r.widgets().at(group))if(w.words[5]!=1){const auto b=layout_ruw_sprites(r,w,0);++records;ops+=b.sprites.size();}
            std::cout<<"owned authored sprite records="<<records<<" emitted operations="<<ops<<'\n';
        } else require(argc==1,"usage: ui-layout-geometry-tests [owned BTL.RUW]");
        std::cout<<"UI layout geometry tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
