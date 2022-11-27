#include "okage/ui_projection.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
float normal(float f){
    const auto bits=__builtin_bit_cast(std::uint32_t, (f)),exp=bits&0x7f800000u;
    require(exp!=0x7f800000u&&(exp!=0||(bits&0x7fffffu)==0),
            "UI projection exceptional float");return f;
}
template<class T> void validate(const T& a){for(float f:a)normal(f);}
struct Rounding {
    std::fenv_t saved;
    Rounding(){require(std::fegetenv(&saved)==0,"UI projection save fenv");
        if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI projection rounding");}}
    ~Rounding(){std::fesetenv(&saved);}
};
float op(float a,float b,char kind){
    volatile float x=normal(a),y=normal(b),out;
    require(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI projection clear fenv");
    if(kind=='+')out=x+y;else if(kind=='-')out=x-y;else if(kind=='*')out=x*y;
    else{require(y!=0,"UI projection zero divisor");out=x/y;}
    require(!std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW),"UI projection overflow/underflow");
    return normal(out);
}
float mul(float a,float b){return op(a,b,'*');}
float add(float a,float b){return op(a,b,'+');}
float sub(float a,float b){return op(a,b,'-');}
std::uint32_t ftoi4(float value){
    if(value>=134217728.f)return 0x7fffffffu;
    if(value< -134217728.f)return 0x80000000u;
    return std::uint32_t(static_cast<std::int32_t>(mul(value,16.f)));
}
float area(const Float4& a,const Float4& b,const Float4& c){
    const auto bx=sub(b[0],a[0]),by=sub(b[1],a[1]);
    const auto cx=sub(c[0],a[0]),cy=sub(c[1],a[1]);
    return sub(add(mul(1.f,__builtin_bit_cast(float, (0xfe967699u))),mul(bx,cy)),mul(cx,by));
}
}
Float4 ui_projection_color_cache(const Float4& value,const Float4& scale,const Float4& bias){
    validate(value);validate(scale);validate(bias);Rounding rounding;Float4 out;
    for(unsigned i=0;i<4;++i)out[i]=add(mul(1.f,bias[i]),mul(scale[i],value[i]));
    return out;
}
std::array<float,2> ui_projection_uv_cache(const std::array<float,2>& value,
    const std::array<float,2>& scale,const std::array<float,2>& bias){
    validate(value);validate(scale);validate(bias);Rounding rounding;std::array<float,2> out;
    for(unsigned i=0;i<2;++i)out[i]=add(mul(1.f,bias[i]),mul(scale[i],value[i]));
    return out;
}
UiProjectedQuad ui_project_quad(const UiProjectionParameters& p,const Matrix4& matrix,
    const std::array<UiProjectionVertex,4>& input,bool textured){
    for(const auto& v:p)validate(v);
    for(const auto& v:matrix)validate(v);
    for(const auto& v:input)validate(v.position);
    Rounding rounding;Matrix4 combined;
    for(unsigned c=0;c<4;++c){
        for(unsigned r=0;r<4;++r)
            combined[c][r]=add(mul(p[0][r],matrix[c][r<2?r:2]),mul(p[1][r],matrix[c][3]));
    }
    UiProjectedQuad out;unsigned any=0,common=63;
    for(unsigned i=0;i<4;++i){
        auto& v=out.vertices[i];const auto& source=input[i].position;
        for(unsigned r=0;r<4;++r){
            auto acc=mul(combined[0][r],source[0]);
            acc=add(acc,mul(combined[1][r],source[1]));
            acc=add(acc,mul(combined[2][r],source[2]));
            v.clip[r]=add(acc,mul(combined[3][r],source[3]));
        }
        const auto w=std::abs(v.clip[3]);
        for(unsigned r=0;r<3;++r){
            if(v.clip[r]>w)v.clip_flags|=1u<<(r*2);
            if(v.clip[r]<-w)v.clip_flags|=2u<<(r*2);
        }
        any|=v.clip_flags;common&=v.clip_flags;
        const auto q=op(1.f,v.clip[3],'/');
        for(unsigned r=0;r<4;++r){
            const auto a=mul(p[4][r],v.clip[r==3?2:3]);
            const auto screen=mul(add(a,mul(v.clip[r],p[5][r])),q);
            // VU MAX chooses its second operand (+0) on an equal value.
            v.screen[r]=screen>0?screen:0.f;
        }
        if(v.screen[3]>255.f)v.screen[3]=255.f;
    }
    if(common)return out;
    require(!any,"UI projection mixed clipping is unrecovered");
    const auto a=area(out.vertices[0].screen,out.vertices[1].screen,out.vertices[2].screen);
    const auto b=area(out.vertices[0].screen,out.vertices[2].screen,out.vertices[3].screen);
    if(!(a<0)&&!(b<0))return out;
    for(unsigned i=0;i<4;++i){
        auto& v=out.vertices[i];const auto& source=input[i];
        validate(source.source_color);validate(source.auxiliary_color);
        if(textured){
            validate(source.uv);
            //2d6c/74: Q is reconstructed from the SCREEN Z cache, not1/clipW.
            const auto q=add(mul(1.f,p[7][3]),mul(v.screen[2],p[6][3]));
            v.stq={mul(source.uv[0],q),mul(source.uv[1],q),mul(1.f,q)};
        }
        for(unsigned r=0;r<4;++r){
            v.xyzf[r]=ftoi4(v.screen[r]);
            const auto product=mul(source.source_color[r],source.auxiliary_color[r]);
            require(product>=0,"UI projection negative color product is unrecovered");
            const auto color=add(mul(1.f,8388608.f),product);
            v.rgba[r]=__builtin_bit_cast(std::uint32_t, (std::min(color,8388863.f)));
        }
    }
    out.emitted=true;return out;
}
}
