#include "okage/ui_window.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
using I=std::int32_t;
using V=std::array<float,4>;
using K=UiPacketOperationKind;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
I si(std::uint32_t value){return __builtin_bit_cast(I, (value));}
float ordinary(float value){const auto b=__builtin_bit_cast(std::uint32_t, (value)),e=b&0x7f800000u;
    check(e!=0x7f800000u&&(e!=0||(b&0x7fffffu)==0),"UI background exceptional float");return value;}
struct Rounding {
    std::fenv_t saved;
    Rounding(){check(std::fegetenv(&saved)==0,"UI background save fenv");if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI background rounding");}}
    ~Rounding(){std::fesetenv(&saved);}
};
float num(I value){volatile float out=static_cast<float>(value);return out;}
float calculate(float a,float b,char operation){volatile float x=ordinary(a),y=ordinary(b),out;
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI background clear exceptions");
    if(operation=='+')out=x+y;else if(operation=='-')out=x-y;else out=x*y;
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)&(FE_OVERFLOW|FE_UNDERFLOW)),"UI background overflow/underflow");return ordinary(out);}
float add(float a,float b){return calculate(a,b,'+');}
float sub(float a,float b){return calculate(a,b,'-');}
float mul(float a,float b){return calculate(a,b,'*');}
float value(const UiWindow& w,unsigned at){return ordinary(__builtin_bit_cast(float, (w.word(at))));}
void append(std::vector<UiPacketOperation>& out,K kind,unsigned helper,
            std::array<std::uint64_t,4> args={},std::vector<V> values={}){
    out.push_back({kind,helper,args,std::move(values)});
}
}
void UiWindow::build_background_packet(){
    check(extra_[0].size()==0x12c0,"Missing UI background cache allocation");
    const bool textured=((word(0x25c)>>2)&3)==2;
    check(!textured||bool(texture_),"Missing UI background texture owner");
    Rounding rounding;
    const auto x=value(*this,0x560),y=value(*this,0x564),z=value(*this,0x568);
    const auto columns=word(0x24c),rows=word(0x250);
    // 1a7c78..1a7d10 MULT/addiu retain low32 before signed CVT.S.W.
    const auto right=sub(num(si(columns*24u+25u)),x);
    const auto bottom=sub(num(si(rows*24u+25u)),y);
    const auto left=sub(23.f,x),depth=sub(2.f,z);
    // 1a7c34/58 differs from the other top corner's single23-Y subtraction.
    const auto first_top=add(sub(-1.f,y),24.f),second_top=sub(23.f,y);
    UiNativePacket packet;auto& ops=packet.operations;
    append(ops,K::CompilerBoundary,0x1ad1f8);append(ops,K::CompilerBoundary,0x1af978);
    append(ops,K::Positions,0x1ae940,{0,4},{{left,first_top,depth,1},
        {right,second_top,depth,1},{right,bottom,depth,1},{left,bottom,depth,1}});
    //1a7b78: ad1f8/ad1e0 and af978 set333be4=1 before ae940 consumes it.
    ops.back().first_positions_upload=true;
    append(ops,K::AuxiliaryVectors,0x1aeb10,{0,1},{{0,0,0,0}});
    std::vector<V> colors;
    for(unsigned i=0;i<4;++i){const auto rgba=word(0x2e0+i*4);V color{};
        for(unsigned j=0;j<4;++j)color[j]=mul(num((rgba>>(j*8))&255),1.f/128.f);
        colors.push_back(color);
    }
    append(ops,K::Colors,0x1aeb48,{0,4},std::move(colors));
    // 1a7ebc..1a7f24: wrapped integer products then19c348, no half-texel.
    const auto u=mul(num(si(columns*word(0x10c))),1.f/1024.f);
    const auto v=mul(num(si(rows*word(0x18c))),1.f/1024.f);
    append(ops,K::UV,0x1aed70,{0,4},{{0,0,0,0},{u,0,0,0},{u,v,0,0},{0,v,0,0}});
    if(textured){
        packet.window_texture=texture_;
        append(ops,K::Texture,0x1aedd8,{6,1,0xffffffffu,0x20000006a8000000ull});
        // 1a8000/08 DSLL operates on sign-extended LW operands. No masks.
        const auto width=std::uint64_t(std::int64_t(si(word(0x10c))));
        const auto height=std::uint64_t(std::int64_t(si(word(0x18c))));
        append(ops,K::Register,0x1aedd8,{8,(width<<4)|(height<<24)|15u});
    }
    append(ops,K::Register,0x1aedd8,{0x47,0x30003});
    append(ops,K::CompilerBoundary,0x1ad268,{0});
    // Source compiler input;1af3e8 adds primitive-kind bits5, as with glyphs.
    append(ops,K::Primitive,0x1af3e8,{textured?0x58u:0x48u});
    for(unsigned i=0;i<4;++i)append(ops,K::IndexedVertex,0x1af908,{i,0,i,i});
    append(ops,K::CompilerBoundary,0x1afa00);append(ops,K::CompilerBoundary,0x1ad240);
    put(0x3c08,word(0x3c08)|0x10);
    background_packet_=std::move(packet);
    std::fill(extra_[0].known_.begin(),extra_[0].known_.end(),false);
    put(0x3c08,word(0x3c08)&~0x10u);
}
}
