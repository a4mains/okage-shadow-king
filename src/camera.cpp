#include "okage/camera.hpp"
#include <cfenv>
#include <cmath>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Camera rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
}
AtanTable make_atan_table() {
    Rounding rounding;AtanTable result;
    for(unsigned i=0;i<result.size();++i) {
        float value=std::atan(float(i)*0.000244140625f);
        value=value*4.f;value=value*2048.f;value=value/3.1415927410125732421875f;
        result[i]=std::int16_t(value+0.5f);
    }
    return result;
}
float camera_angle(float y,float x,const AtanTable& table) {
    Rounding rounding;
    if(!std::isfinite(y)||!std::isfinite(x))throw std::runtime_error("Non-finite camera direction");
    const auto ax=std::fabs(x),ay=std::fabs(y);unsigned angle;
    if(ax==ay) {if(ax==0)return 0;angle=0x800;}
    else {
        float ratio=ay<ax ? (ay*4096.f)/ax : (ax*4096.f)/ay;
        if(!std::isfinite(ratio)||ratio<0 || ratio>=4096)throw std::runtime_error("Camera angle table bounds");
        const auto part=table[unsigned(ratio)];angle=ay<ax ? part : 0x1000-part;
    }
    if(y<0)angle=0x4000-angle;
    if(x<0)angle=0x2000-angle;
    return float(angle&0x3fff)*0.00006103515625f;
}
Float4 camera_look_at(const Float4& position,const Float4& target,const AtanTable& angles,const SineTable& sine) {
    Rounding rounding;Float4 d;
    for(unsigned i=0;i<4;++i) {
        if(!std::isfinite(position[i])||!std::isfinite(target[i]))throw std::runtime_error("Non-finite camera point");
        d[i]=position[i]+(-target[i]);
    }
    const auto yaw=camera_angle(-d[0],-d[2],angles);
    const auto index=(std::uint32_t(std::int32_t(-yaw*32768.f))>>1)&0x3fff;
    const float s=sine[index],c=sine[(index+4096)&0x3fff];
    // Original identity then Y rotation: z' = -sin(yaw)*x + cos(yaw)*z.
    float z=(-s)*d[0];z=z+0.f*d[1];z=z+c*d[2];z=z+0.f*d[3];
    return {camera_angle(d[1],-z,angles),yaw,0,1};
}
void CameraBlend::begin(float rate,const Float4& from_position,const Float4& from_target) {
    if(!std::isfinite(rate))throw std::runtime_error("Invalid camera blend rate");
    if(rate!=0) {phase=0;step=rate;position=from_position;target=from_target;}
}
void CameraBlend::apply(Float4& p,Float4& t) {
    Rounding rounding;
    if(phase==1)return;
    for(unsigned i=0;i<4;++i) {p[i]=position[i]+(p[i]-position[i])*phase;t[i]=target[i]+(t[i]-target[i])*phase;}
    phase=phase+step;if(phase>=0.99f)phase=1;
}
}
