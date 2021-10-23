#include "okage/field_raster.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(old!=FE_TOWARDZERO && std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Field raster rounding unavailable");}
    ~Rounding(){if(old!=FE_TOWARDZERO)std::fesetround(old);}
};
Float4 project(const Float4& p,const ShadowScreenParameters& s) {
    // Keep all four terms, including zero products, in the VU's order.
    const Matrix4 m{{{s[0][0],0,0,0},{0,s[0][1],0,0},{0,0,s[0][2],s[0][3]},s[1]}};
    Float4 c;
    for(unsigned r=0;r<4;++r){float v=m[0][r]*p[0];v=v+m[1][r]*p[1];v=v+m[2][r]*p[2];c[r]=v+m[3][r]*p[3];}
    return c;
}
Float4 screen(const Float4& c,const ShadowScreenParameters& s,bool clamp) {
    const float q=c[3]==0?std::copysign(std::numeric_limits<float>::max(),c[3]):1.f/c[3];
    Float4 out;
    for(unsigned r=0;r<4;++r){float v=s[4][r]*(r==3?c[2]:c[3]);v=v+c[r]*s[5][r];v=v*q;out[r]=clamp?std::max(v,0.f):v;}
    if(clamp)out[3]=std::min(out[3],255.f);
    return out;
}
using Vertex=FieldRasterVertex;
Vertex intersection(const Vertex& outside,const Vertex& inside,unsigned axis,float plane) {
    const float t=(plane-outside.position[axis])/(inside.position[axis]-outside.position[axis]);
    Vertex result;
    for(unsigned r=0;r<4;++r){
        result.position[r]=outside.position[r]+(inside.position[r]-outside.position[r])*t;
        result.color[r]=outside.color[r]+(inside.color[r]-outside.color[r])*t;
        result.uv[r]=outside.uv[r]+(inside.uv[r]-outside.uv[r])*t;
    }
    return result;
}
//0x3900 clips a pair of planes together; intersections start at the outside
// endpoint (0x3c98/0x3d18), which matters for floating-point boundary results.
std::vector<Vertex> clip_range(const std::vector<Vertex>& input,unsigned axis,float low,float high) {
    std::vector<Vertex> out;
    if(input.empty())return out;
    auto side=[&](const Vertex& v){return v.position[axis]<low?-1:v.position[axis]>high?1:0;};
    auto previous=input.back();int a=side(previous);
    for(const auto& current:input){
        const int b=side(current);
        if(!a)out.push_back(previous);
        if(a!=b){
            if(a)out.push_back(intersection(previous,current,axis,a<0?low:high));
            if(b)out.push_back(intersection(current,previous,axis,b<0?low:high));
        }
        previous=current;a=b;
    }
    return out;
}
}
std::vector<FieldScreenVertex> field_screen_vertices(std::span<const Float4> points,const ShadowScreenParameters& params) {
    for(const auto& v:params)for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite field screen parameter");
    Rounding rounding;std::vector<FieldScreenVertex> out;out.reserve(points.size());
    for(const auto& p:points){
        for(float f:p)if(!std::isfinite(f))throw std::runtime_error("Nonfinite field position");
        FieldScreenVertex v;v.clip=project(p,params);v.screen=screen(v.clip,params,true);
        for(float f:v.clip)if(!std::isfinite(f))throw std::runtime_error("Field clip overflow");
        for(float f:v.screen)if(!std::isfinite(f))throw std::runtime_error("Field screen overflow");
        const auto w=std::abs(v.clip[3]);
        for(unsigned r=0;r<3;++r){if(v.clip[r]>w)v.flags|=1u<<(2*r);if(v.clip[r]<-w)v.flags|=2u<<(2*r);}
        out.push_back(v);
    }
    return out;
}
std::array<std::uint32_t,4> field_pack_xyzf(const Float4& value) {
    Rounding rounding;std::array<std::uint32_t,4> out;
    for(unsigned r=0;r<4;++r){
        if(!std::isfinite(value[r]))throw std::runtime_error("Nonfinite field packing value");
        const float scaled=value[r]*16;
        const auto integer=scaled>=2147483648.f?INT32_MAX:scaled< -2147483648.f?INT32_MIN:static_cast<std::int32_t>(scaled);
        out[r]=static_cast<std::uint32_t>(integer);
    }
    return out;
}
std::vector<FieldRasterVertex> field_raster_triangles(std::span<const Vertex> input,const ShadowScreenSettings& settings,bool gs_color_units) {
    if(input.size()%3)throw std::runtime_error("Field raster requires triangles");
    const auto params=shadow_screen_parameters(settings);
    Rounding rounding;std::vector<Vertex> out;out.reserve(input.size());
    for(std::size_t at=0;at<input.size();at+=3){
        std::vector<Vertex> polygon(input.begin()+at,input.begin()+at+3);
        std::uint32_t flags=0,common=63;
        for(auto& v:polygon){
            for(const auto& value:{v.position,v.color,v.uv})for(float f:value)
                if(!std::isfinite(f))throw std::runtime_error("Nonfinite field vertex");
            v.position=project(v.position,params);
            if(gs_color_units)for(float& c:v.color)c=std::min(c,255.f);
            std::uint32_t f=0;const auto w=std::abs(v.position[3]);
            for(unsigned r=0;r<3;++r){if(v.position[r]>w)f|=1u<<(2*r);if(v.position[r]<-w)f|=2u<<(2*r);}
            flags|=f;common&=f;
        }
        if(common)continue;
        if(flags&0x30)polygon=clip_range(polygon,2,params[6][2],params[7][2]);
        for(auto& v:polygon){
            const float q=1.f/v.position[3];
            // Clipped path retains negative screen values until XY clipping.
            v.position=screen(v.position,params,!flags);
            v.uv={v.uv[0]*q,v.uv[1]*q,q,0};
        }
        if(flags){polygon=clip_range(polygon,0,params[6][0],params[7][0]);polygon=clip_range(polygon,1,params[6][1],params[7][1]);}
        for(auto& v:polygon){
            if(gs_color_units)for(float& c:v.color)c=std::trunc(c);
            v.position[3]=std::clamp(v.position[3],0.f,255.f);
            const auto packed=field_pack_xyzf(v.position);
            const float x=float(packed[0]&0xffff)/16.f-(settings.center_x-320.f)+.5f;
            const float y=float(packed[1]&0xffff)/16.f-(settings.center_y-112.f)+.5f;
            v.position={2*x/640.f-1,1-2*y/224.f,v.uv[2],1};
            v.uv={v.uv[0],v.uv[1],float((packed[2]&0x0ffffff0)>>4),1};
        }
        for(std::size_t i=2;i<polygon.size();++i){out.push_back(polygon[0]);out.push_back(polygon[i-1]);out.push_back(polygon[i]);}
    }
    return out;
}
}
