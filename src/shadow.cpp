#include "okage/shadow.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct ProjectionRounding {
    int old=std::fegetround();
    ProjectionRounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set shadow projection rounding");}
    ~ProjectionRounding(){std::fesetround(old);}
};
Float4 shadow_transform(const Matrix4& m,const Float4& v) {
    Float4 result{};
    for(unsigned row=0;row<4;++row) {
        float f=m[0][row]*v[0];f=f+m[1][row]*v[1];f=f+m[2][row]*v[2];
        result[row]=f+m[3][row]*v[3];
    }
    return result;
}
Matrix4 shadow_multiply(const Matrix4& a,const Matrix4& b) {
    Matrix4 result;
    for(unsigned col=0;col<4;++col)result[col]=shadow_transform(a,b[col]);
    return result;
}
Float4 shadow_normalize(const Float4& v) {
    float square=v[0]*v[0];square=square+v[1]*v[1];square=square+v[2]*v[2];
    if(!(square>0)||!std::isfinite(square))throw std::runtime_error("Invalid shadow projection direction");
    const float inverse=1.f/std::sqrt(square);
    return {v[0]*inverse,v[1]*inverse,v[2]*inverse,0};
}
//0x1e2b20's directional branch. Keep even zero products/signs and the scalar
// operation order: normalized and unnormalized directions differ in rounding.
Matrix4 shadow_plane(float y,const Float4& d) {
    constexpr float x=0,z=0;
    const float a=x*d[0],b=y*d[1],c=z*d[2];
    float dot=a+b;dot=dot+c;
    if(!std::isfinite(dot)||dot==0)throw std::runtime_error("Degenerate shadow projection plane");
    const float inv=-1.f/dot;
    return Matrix4{{{inv*(a-dot),inv*(x*d[1]),inv*(x*d[2]),0},
                    {inv*(y*d[0]),inv*(b-dot),inv*(y*d[2]),0},
                    {inv*(z*d[0]),inv*(z*d[1]),inv*(c-dot),0},
                    {inv*-d[0],inv*-d[1],inv*-d[2],inv*-dot}}};
}
Matrix4 shadow_translate(Matrix4 m,const Float4& p) {
    for(unsigned i=0;i<3;++i)m[3][i]=m[3][i]+p[i];
    return m;
}
}
ShadowProjectionContext::ShadowProjectionContext(const Float4& light,const Matrix4& camera):view(camera) {
    for(float f:light)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow light direction");
    for(const auto& col:view)for(float f:col)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow view");
    if(view[0][3]!=0||view[1][3]!=0||view[2][3]!=0||view[3][3]!=1)
        throw std::runtime_error("Shadow view must be affine");
    ProjectionRounding rounding;
    slope=light[0];span=slope+slope;
    world_direction=shadow_normalize(light);
    planes={shadow_plane(-4,world_direction),shadow_plane(4,world_direction)};
    //0x1e2350 uses the transposed basis and a three-term translation product.
    // This intentionally is not a general inverse for scaled camera matrices.
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)inverse[col][row]=view[row][col];
    inverse[3][3]=1;
    for(unsigned row=0;row<3;++row) {
        float f=inverse[0][row]*view[3][0];f=f+inverse[1][row]*view[3][1];
        f=f+inverse[2][row]*view[3][2];inverse[3][row]=0.f-f;
    }
    view_direction=shadow_normalize(shadow_transform(view,world_direction));
}
ShadowProjection ShadowProjectionContext::project(unsigned mode,float height,float factor,
        const std::array<std::uint32_t,3>& tint) const {
    if(mode>7||!std::isfinite(height)||!std::isfinite(factor))throw std::runtime_error("Invalid shadow projection request");
    for(auto c:tint)if(c>255)throw std::runtime_error("Shadow tint byte overflow");
    ProjectionRounding rounding;
    ShadowProjection out;
    if(mode==2||mode==3||mode==6||mode==7)out.color=tint;
    if(mode<4)return out;
    out.has_matrices=true;
    Matrix4 first,second;float rate=slope,range=span,offset_z=0;
    if(mode==4) {
        rate=slope*factor;
        second=planes[1];
        if(factor!=1) {
            const Float4 direction{rate,1,0,0};
            out.direction=shadow_normalize(shadow_transform(view,direction));
            //0x1b8ec4 normalizes into the later translation temporary. Only
            // X/Y are replaced at0x1b8f58/0x1b8fd0; retain its direction Z.
            offset_z=(*out.direction)[2];
            second=shadow_plane(4,direction);
        }
        height=height-8;range=span+8;
        first=Matrix4{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    } else {
        first=shadow_translate(planes[0],{rate*height,height,0,0});
        second=planes[1];
    }
    const float upper=height+range;
    second=shadow_translate(second,{rate*upper,upper,offset_z,0});
    out.matrices={shadow_multiply(view,shadow_multiply(first,inverse)),
                  shadow_multiply(view,shadow_multiply(second,inverse))};
    return out;
}
namespace {
Float4 shadow_cross(const Float4& a,const Float4& b) {
    return {a[1]*b[2]-b[1]*a[2],a[2]*b[0]-b[2]*a[0],a[0]*b[1]-b[0]*a[1],0};
}
float shadow_dot(const Float4& a,const Float4& b) {
    float v=a[0]*b[0];v=v+a[1]*b[1];v=v+a[2]*b[2];
    if(!std::isfinite(v))throw std::runtime_error("Shadow prism arithmetic overflow");
    return v;
}
std::uint32_t shadow_fixed(float value) {
    const float scaled=value*16;
    if(!std::isfinite(scaled)||scaled>=2147483648.f||scaled< -2147483648.f)
        throw std::runtime_error("Shadow vertex exceeds fixed-point range");
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled));
}
}
ShadowPrism shadow_prism(const ShadowPrismInput& input) {
    if(input.clip_status< -1||input.clip_status>1)throw std::runtime_error("Invalid shadow clip status");
    for(const auto& values:{input.camera,input.screen})for(const auto& v:values)for(float f:v)
        if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow prism vertex");
    for(float f:input.direction)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow prism direction");
    std::uint32_t clip_union=0,clip_intersection=63;
    for(auto f:input.clip) {
        if(f>63)throw std::runtime_error("Invalid shadow prism clip flags");
        clip_union|=f;clip_intersection&=f;
    }
    ShadowPrism out;
    if(input.clip_status<0)return out;
    ProjectionRounding rounding;
    if(clip_union&&(clip_intersection||(clip_union&0x30)))return out;
    const auto& p=input.camera;
    Float4 edge_a{},edge_b{};
    for(unsigned i=0;i<3;++i){edge_a[i]=p[0][i]-p[1][i];edge_b[i]=p[1][i]-p[2][i];}
    if(shadow_dot(input.direction,shadow_cross(edge_b,edge_a))<=0)return out;
    if(clip_union) {
        for(const auto& v:input.screen)for(unsigned i=0;i<2;++i)
            if(v[i]<=16||v[i]>=4080)return out;
    }
    constexpr std::array<unsigned,12> order{0,1,2,5,1,4,0,3,2,5,4,3};
    for(unsigned pass=0;pass<3;++pass)for(unsigned i=0;i<12;++i) {
        auto v=input.screen[order[i]];
        v[2]=v[2]+(pass==1?200.f:-200.f);
        if(pass==1||i==3||i==10)v[3]=v[3]+2048.f;
        for(unsigned j=0;j<4;++j)out.passes[pass][i][j]=shadow_fixed(v[j]);
    }
    auto face=[&](unsigned point,unsigned a,unsigned b,std::initializer_list<unsigned> vertices) {
        const auto normal=shadow_cross(p[a],p[b]);
        const float distance=std::sqrt(shadow_dot(p[point],p[point]));
        const float size=std::sqrt(shadow_dot(normal,normal));
        const float bias=__builtin_bit_cast(float, (0xb851b717u))*distance;
        const float value=shadow_dot(p[point],normal)+bias*size;
        if(value<=0)return;
        for(auto i:vertices) {
            const float w=input.screen[order[i]][3];
            out.passes[0][i][3]=out.passes[2][i][3]=shadow_fixed(w+2048.f);
            out.passes[1][i][3]=shadow_fixed(w);
        }
    };
    face(0,2,1,{2});face(2,5,1,{4,5});face(1,4,0,{6,7});
    face(0,3,2,{8,9});face(5,3,4,{11});
    out.emitted=true;
    return out;
}
void ShadowTransition::tick(const Float4& hero_rotation,const Float4& hero_position) {
    if(phase>5||!std::isfinite(factor)||!std::isfinite(projection_factor))
        throw std::runtime_error("Unrecovered shadow transition state");
    for(const auto& v:{hero_rotation,hero_position,stun_rotation,stun_position,stun_scale})
        for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow transform");
    struct Rounding {
        int old=std::fegetround();
        Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set shadow rounding");}
        ~Rounding(){std::fesetround(old);}
    } rounding;
    if(!follow_disabled) {
        stun_rotation=hero_rotation;stun_position=hero_position;
        stun_position[1]-=.01f;
    }
    const auto shrink=[&] {if(factor>0){factor-=.05f;return false;}factor=0;return true;};
    const auto grow=[&] {if(factor<1){factor+=.05f;return false;}factor=1;return true;};
    switch(phase) {
    case 0:case 3:break;
    case 1:
        if(shrink()){phase=2;hero_shadow=0xffff;}
        projection_factor=factor;break;
    case 2:
        if(grow())phase=3;
        stun_scale={factor,factor,factor,1};stun_visible=true;break;
    case 4:
        if(shrink()){phase=5;stun_visible=false;}
        stun_scale={factor,factor,factor,1};break;
    case 5:
        if(grow())phase=0;
        projection_factor=factor;hero_shadow=4;break;
    }
}
}
