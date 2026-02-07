#include "okage/image_quad.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <cfenv>
#pragma STDC FENV_ACCESS ON

namespace okage {
ImageQuad source_image_quad(std::uint32_t width,std::uint32_t height,
        const std::array<std::uint32_t,6>& words,const SineTable& sine) {
    if(!width||width>=4096||!height||height>=4096)throw std::runtime_error("Image source descriptor dimensions");
    std::array<float,6> f;for(unsigned i=0;i<6;++i)f[i]=std::bit_cast<float>(words[i]);
    for(float value:f)if(!std::isfinite(value))throw std::runtime_error("Non-finite source image transform");
    if(f[2]<=0||f[3]<=0||std::abs(f[4])>65535||f[5]<-16777216.f||f[5]>=16777216.f)
        throw std::runtime_error("Source image transform outside recovered range");
    struct Rounding {int previous=std::fegetround();Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Image rounding mode");}
        ~Rounding(){std::fesetround(previous);}} rounding;
    const auto mul=[](float a,float b){volatile float value=a*b;return value;};
    const auto add=[](float a,float b){volatile float value=a+b;return value;};
    const auto index=std::uint32_t(std::int32_t(mul(f[4],16384.f)))&0x3fff;
    const float s=sine[index],c=sine[(index+4096)&0x3fff];
    const float a=mul(float(width*8),f[2]),b=mul(float(height*4),f[3]);
    const float ns=-s,nc=-c,cb=mul(c,b),nsa=mul(ns,a),nca=mul(nc,a),nsb=mul(ns,b);
    const float sa=-nsa,ncb=-cb;
    const std::array<std::array<float,2>,4> local{{{add(sa,nca),add(ncb,nsb)},
        {add(sa,-nca),add(ncb,-nsb)},{add(nsa,-nca),add(cb,-nsb)},{add(nsa,nca),add(cb,nsb)}}};
    const float px=mul(c,0.f),nq=mul(ns,0.f),nx=mul(ns,0.f),ncq=mul(nc,0.f);
    const std::array<float,2> center{add(add(f[0],add(nx,-ncq)),32768.f),
        add(add(f[1],mul(add(px,-nq),.5f)),32768.f)};
    ImageQuad result{};
    for(unsigned i=0;i<4;++i)for(unsigned k=0;k<2;++k) {
        //VUadds3374f0 to corrected position, then to corners, thenVFT0I0.
        const float coordinate=add(local[i][k],center[k]);
        if(!std::isfinite(coordinate)||coordinate<0||coordinate>65535)
            throw std::runtime_error("Image XY outside native GS16 support");
        result.xy16[i][k]=std::uint32_t(coordinate);
    }
    result.st={.5f/1024,.5f/1024,(float(width)-.5f)/1024,(float(height)-.5f)/1024};
    result.rgba=(std::uint32_t(std::int32_t(mul(f[5],128.f)))<<24)+0x808080;result.z=0xffff;
    return result;
}
ImageQuad axis_aligned_image_quad(std::uint32_t width,std::uint32_t height,
                                 const std::array<std::uint32_t,6>& words) {
    const auto check=[](bool v,const char* why){if(!v)throw std::runtime_error(why);};
    check(width>0&&width<4096&&height>0&&height<4096,"Image source descriptor dimensions");
    check(words[2]==0x3f800000&&words[3]==0x3f800000&&words[4]==0,
          "Unrecovered image scale/rotation");
    const float x=std::bit_cast<float>(words[0]),y=std::bit_cast<float>(words[1]);
    check(std::isfinite(x)&&std::isfinite(y)&&x==std::trunc(x)&&y==std::trunc(y)&&
          x>=-65535&&x<=65535&&y>=-65535&&y<=65535,"Unrecovered fractional/out-of-range image position");
    const auto cx=32768+std::int32_t(x),cy=32768+std::int32_t(y);
    const auto dx=std::int32_t(width*8),dy=std::int32_t(height*4);
    const auto left=cx-dx,right=cx+dx,top=cy-dy,bottom=cy+dy;
    check(left>=0&&right<=65535&&top>=0&&bottom<=65535,"Image XY outside native GS16 support");
    const float alpha=std::bit_cast<float>(words[5]);
    check(std::isfinite(alpha)&&alpha>=-1.f&&alpha<=1.f,"Image alpha outside recovered title range");
    const auto packed_alpha=std::uint32_t(std::int32_t(alpha*128.f))<<24;
    return {{{{std::uint32_t(left),std::uint32_t(top)},
              {std::uint32_t(right),std::uint32_t(top)},
              {std::uint32_t(right),std::uint32_t(bottom)},
              {std::uint32_t(left),std::uint32_t(bottom)}}},
        {.5f/1024,.5f/1024,(float(width)-.5f)/1024,(float(height)-.5f)/1024},
        packed_alpha+0x808080,0xffff};
}
}
