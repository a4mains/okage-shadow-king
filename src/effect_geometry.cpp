#include "okage/effect.hpp"
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
EffectNoiseGrid make_effect_noise_grid(const std::function<std::uint32_t()>& random) {
    if(!random)throw std::runtime_error("Effect noise grid requires template RNG");
    EffectNoiseGrid result;
    for(auto& cell:result)cell=random()%7;
    return result;
}
std::vector<MapEffectVertex> make_effect_noise_shape(unsigned shape,const EffectNoiseGrid& grid) {
    if(shape<10||shape>17)throw std::runtime_error("Unrecovered effect noise shape");
    for(auto group:grid)if(group>6)throw std::runtime_error("Invalid effect noise group");
    std::vector<MapEffectVertex> result;
    //0x10e138 walks Z+/Z-, X+/X-, Y+/Y-, rows then columns. Coordinates
    //are exact multiples of1/4, so no host rounding-mode dependency remains.
    for(unsigned face=0;face<6;++face)for(unsigned row=0;row<8;++row)for(unsigned column=0;column<8;++column) {
        if(grid[face*64+row*8+column]!=shape-10)continue;
        const float a=float(column)/4-1,b=float(column+1)/4-1;
        const float c=float(row)/4-1,d=float(row+1)/4-1,side=face%2?-1:1;
        std::array<Float4,4> points;
        switch(face/2) {
            case 0:points={Float4{b,c,side,1},Float4{a,c,side,1},Float4{a,d,side,1},Float4{b,d,side,1}};break;
            case 1:points={Float4{side,d,a,1},Float4{side,c,a,1},Float4{side,c,b,1},Float4{side,d,b,1}};break;
            default:points={Float4{b,side,c,1},Float4{a,side,c,1},Float4{a,side,d,1},Float4{b,side,d,1}};break;
        }
        for(unsigned i:{1u,0u,2u,0u,2u,3u})result.push_back({points[i],{255,255,255,255},{}});
    }
    return result;
}
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
