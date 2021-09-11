#include "okage/effect.hpp"
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
std::vector<MapEffectVertex> make_shadow_effect_shape(unsigned shape,const SineTable& sine,
        const std::function<std::uint32_t()>& random) {
    if(shape!=3&&shape!=55)throw std::runtime_error("Unrecovered procedural shadow shape");
    if(shape==55&&!random)throw std::runtime_error("Shadow shape55 requires template RNG");
    struct Rounding {
        int old=std::fegetround();
        Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set geometry rounding");}
        ~Rounding(){std::fesetround(old);}
    } rounding;
    const auto angle=[](float t){return (unsigned(int(t*32768.f))>>1)&16383;};
    const auto packed=[](Float4 p){for(unsigned i=0;i<3;++i)p[i]=std::round(p[i]*64.f)/64.f;p[3]=1;return p;};
    std::vector<MapEffectVertex> result;
    const Float4 white{255,255,255,255};
    const auto triangle=[&](const Float4& a,const Float4& b,const Float4& c,const Float4& color){
        for(const auto& p:{a,b,c})result.push_back({packed(p),color,{}});
    };
    const auto quad=[&](const std::array<Float4,4>& points,const std::array<Float4,4>& colors){
        //0x112d20 emits B,A,C,D, preserving the A-C diagonal.
        for(unsigned i:{1u,0u,2u,0u,2u,3u})result.push_back({packed(points[i]),colors[i],{}});
    };
    if(shape==3) {
        constexpr unsigned n=16,half=n/2;
        const auto sphere=[&](unsigned column,unsigned row){
            const auto az=angle(float(column%n)/float(n)),el=angle(float(row)/float(n));
            return Float4{sine[(az+4096)&16383]*sine[el],sine[(el+4096)&16383],sine[az]*sine[el],1};
        };
        for(unsigned i=0;i<n;++i)triangle({0,1,0,1},sphere(i+1,1),sphere(i,1),white);
        for(unsigned row=1;row<half-1;++row)for(unsigned i=0;i<n;++i)
            quad({sphere(i,row),sphere(i+1,row),sphere(i+1,row+1),sphere(i,row+1)},
                 {white,white,white,white});
        for(unsigned i=0;i<n;++i)triangle({0,-1,0,1},sphere(i,half-1),sphere(i+1,half-1),white);
    } else {
        constexpr unsigned n=10,rows=n/2-1;
        const auto surface=[&](unsigned column,unsigned row){
            const float height=(float(row)/float(n/2))*2.f;
            const float radius=std::sqrt(height);const auto a=angle(float(column%n)/float(n));
            return Float4{sine[(a+4096)&16383]*radius,-height,sine[a]*radius,1};
        };
        for(unsigned row=0;row<rows;++row) {
            const float top=255.f*(1.f-float(row)/float(rows));
            const float bottom=255.f*(1.f-float(row+1)/float(rows));
            const Float4 upper{255,255,255,float(unsigned(top))},lower{255,255,255,float(unsigned(bottom))};
            for(unsigned i=0;i<n;++i) {
                std::array<Float4,4> points{surface(i,row),surface(i,row+1),surface(i+1,row+1),surface(i+1,row)};
                if(row==rows-1) {
                    points[2][1]-=float(random()%25)*.045f;
                    points[1][1]-=float(random()%25)*.045f;
                }
                quad(points,{upper,lower,lower,upper});
            }
        }
    }
    return result;
}
}
