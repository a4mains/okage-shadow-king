#include "okage/ui_window.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
namespace okage {
struct UiWindowBackgroundTestAccess {
    static void set(UiWindow& w,unsigned at,std::uint32_t value){w.put(at,value);}
};
}
namespace {
using namespace okage;using K=UiPacketOperationKind;using V=std::array<float,4>;
void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected background guard");}
void put(UiWindow& w,unsigned at,std::uint32_t value){UiWindowBackgroundTestAccess::set(w,at,value);}
void fp(UiWindow& w,unsigned at,float value){put(w,at,__builtin_bit_cast(std::uint32_t, (value)));}
UiWindow window(){UiWindow w(2,2,0,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
    put(w,0x25c,0x5c);put(w,0x24c,2);put(w,0x250,3);fp(w,0x560,100);fp(w,0x564,50);fp(w,0x568,-4);return w;}
const UiPacketOperation& operation(const UiWindow& w,K kind,unsigned n=0){require(w.background_packet(),"Missing background owner");
    for(auto& x:w.background_packet()->operations)if(x.kind==kind){if(!n--)return x;}throw std::runtime_error("Missing background operation");}
void bind(UiWindow& w){std::array<std::uint32_t,28> settings{};settings[0]=1;settings[21]=1;settings[25]=2;settings[26]=2;
    std::array<UiWindow::AtlasTuple,2> atlas{{{0,0,16,16},{}}};w.configure(settings,atlas,std::make_shared<int>(42));}
void reached_untextured(){auto w=window();put(w,0x2e0,0xff804307);put(w,0x2e4,0x00804020);put(w,0x2e8,0x80402010);put(w,0x2ec,0x01020304);
    put(w,0x1900,0x8645|0x200);put(w,0x3c08,0xdeadbeff);put(w,0x3e24,13);put(w,0x3e28,14);put(w,0xcc0,15);put(w,0xcc4,16);
    const auto side_g=w.word(0x268c),side_s=w.word(0x2690);require(!w.background_packet(),"Background owner starts absent");
    require(w.extra_cache(0).known_byte(0)==0&&w.extra_cache(0).known_byte(0x12bf)==0,"Constructor clears background allocation");
    w.build_background_packet();
    require(operation(w,K::Positions).first_positions_upload==true,"Background first upload metadata");
    require(!operation(w,K::Colors).first_positions_upload,"Nonposition metadata absent");
    require(operation(w,K::Positions).values==std::vector<V>{{-77,-27,6,1},{-27,-27,6,1},{-27,47,6,1},{-77,47,6,1}},"Source corner geometry");
    require(operation(w,K::UV).values==std::vector<V>{{0,0,0,0},{.03125f,0,0,0},{.03125f,.046875f,0,0},{0,.046875f,0,0}},"Wrapped dimensions and UV without half texel");
    const auto& c=operation(w,K::Colors).values;
    require(c==std::vector<V>{{7.f/128,67.f/128,1,255.f/128},{.25f,.5f,1,0},{.125f,.25f,.5f,1},{4.f/128,3.f/128,2.f/128,1.f/128}},"Each packed RGBA byte uses full precision including authored alpha");
    const auto& a=operation(w,K::AuxiliaryVectors);require(a.source_helper==0x1aeb10&&a.arguments[1]==1&&a.values==std::vector<V>{{0,0,0,0}},"Auxiliary source array preserved without invented meaning");
    require(operation(w,K::Primitive).arguments[0]==0x48&&!w.background_packet()->window_texture,"Mode3 untextured path");
    unsigned registers=0,textures=0,vertices=0;for(auto& o:w.background_packet()->operations){registers+=o.kind==K::Register;textures+=o.kind==K::Texture;if(o.kind==K::IndexedVertex){require(o.arguments==std::array<std::uint64_t,4>{vertices,0,vertices,vertices},"Ordered corner indices");++vertices;}}
    require(registers==1&&textures==0&&vertices==4&&operation(w,K::Register).arguments[0]==0x47&&operation(w,K::Register).arguments[1]==0x30003,"Untextured GS state");
    require(w.word(0x3c08)==0xdeadbeef&&w.word(0x1900)==(0x8645|0x200),"Only compiler availability bit cleared; dispatcher dirty retained");
    require(w.word(0x3e24)==13&&w.word(0x3e28)==14&&w.word(0xcc0)==15&&w.word(0xcc4)==16&&w.word(0x268c)==side_g&&w.word(0x2690)==side_s,"No selected-side/cursor finalization");
    require(!w.known_word(0x3d14)&&!w.extra_cache(1).known_byte(0)&&!w.extra_cache(2).known_byte(0),"No guest pointer or unrelated heap initialization");
    for(unsigned i=0;i<w.extra_cache(0).size();++i)require(!w.extra_cache(0).known_byte(i),"Typed cache does not claim raw encoded bytes");
    for(unsigned mode:{0u,1u}){put(w,0x25c,mode<<2);w.build_background_packet();require(operation(w,K::Primitive).arguments[0]==0x48,"All nontexture background modes use source primitive48");}
}
void textured_and_wrapped(){auto w=window();bind(w);fp(w,0x560,100);fp(w,0x564,50);fp(w,0x568,-4);put(w,0x25c,8);put(w,0x24c,2);put(w,0x250,3);w.build_background_packet();
    require(w.background_packet()->window_texture==w.texture_owner(),"Native texture retained");
    require(operation(w,K::Texture).arguments==std::array<std::uint64_t,4>{6,1,0xffffffffu,0x20000006a8000000ull},"Source TEX0 constants with explicit native binding");
    require(operation(w,K::Register).arguments[1]==((16ull<<4)|(16ull<<24)|15)&&operation(w,K::Primitive).arguments[0]==0x58,"Textured CLAMP and primitive");
    put(w,0x10c,0xffffffff);put(w,0x18c,0xfffffffe);w.build_background_packet();
    require(operation(w,K::Register).arguments[1]==((~0ull<<4)|((~0ull-1)<<24)|15),"CLAMP uses sign-extended64 bit shifts without masks");
    require(operation(w,K::UV).values[2]==V{-2.f/1024,-6.f/1024,0,0},"Signed products become numeric UV");
    put(w,0x24c,0x40000000);put(w,0x10c,16);w.build_background_packet();
    require(operation(w,K::Positions).values[1][0]==-75&&operation(w,K::UV).values[1][0]==0,"Geometry and UV integer multiplication wrap before conversion");
}
void arithmetic_and_guards(){auto w=window();put(w,0x564,0x447ffff5);const auto old=std::fegetround();std::fesetround(FE_UPWARD);w.build_background_packet();
    require(std::fegetround()==FE_UPWARD,"Host rounding restored");std::fesetround(old);
    const auto& p=operation(w,K::Positions).values;require(__builtin_bit_cast(std::uint32_t, (p[0][1]))==0xc47a3ff4&&__builtin_bit_cast(std::uint32_t, (p[1][1]))==0xc47a3ff5,"FirstY two operations differs from23-Y under source rounding");
    auto bad=window();put(bad,0x25c,8);const auto flags=bad.word(0x3c08);rejects([&]{bad.build_background_packet();});
    require(!bad.background_packet()&&bad.extra_cache(0).known_byte(0)==0&&bad.word(0x3c08)==flags,"Missing binding rejected before cache mutation");
    put(bad,0x25c,12);for(auto bits:{0x7fc00000u,0x7f800000u,1u}){put(bad,0x560,bits);rejects([&]{bad.build_background_packet();});require(!bad.background_packet(),"Exceptional input does not fabricate cache");}
}
}
int main(){try{reached_untextured();textured_and_wrapped();arithmetic_and_guards();std::cout<<"UI background tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
