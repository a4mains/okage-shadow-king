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
ProceduralEffectGeometry make_procedural_effect_shape(unsigned shape,const SineTable& sine,
        const std::function<std::uint32_t()>& random,const std::array<std::uint32_t,2>& texture_size) {
    if(shape!=43&&shape!=45&&shape!=55&&shape!=56&&shape!=62)
        throw std::runtime_error("Unrecovered procedural template shape");
    if(shape!=45&&!random)throw std::runtime_error("Procedural template requires RNG");
    const bool textured=shape==56||shape==62;
    if(textured&&(!texture_size[0]||!texture_size[1]||texture_size[0]>512||texture_size[1]>512))
        throw std::runtime_error("Procedural texture dimensions outside reviewed extent");
    struct Rounding {
        int old=std::fegetround();
        Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Cannot set procedural rounding");}
        ~Rounding(){std::fesetround(old);}
    } rounding;
    ProceduralEffectGeometry result;
    const auto uv=[&](float u,float v){return Float4{
        float(unsigned(u*float(texture_size[0])*32))/32768,
        float(unsigned(v*float(texture_size[1]+1)*32))/32768,0,0};};
    const auto append=[&](Float4 p,const Float4& c,const Float4& t,unsigned tag) {
        for(unsigned axis=0;axis<3;++axis)p[axis]=std::round(p[axis]*64)/64;
        p[3]=1;result.vertices.push_back({p,c,t});
        result.position_tags.push_back(std::uint8_t(tag));result.uv_tags.push_back(0);
    };
    if(shape==55||shape==56) {
        result.vertices=make_shadow_effect_shape(55,sine,random);
        result.position_tags.resize(result.vertices.size());result.uv_tags.resize(result.vertices.size());
        if(textured) {
            unsigned vertex=0;
            for(unsigned row=0;row<4;++row)for(unsigned column=0;column<10;++column) {
                const auto point=[&](unsigned col,unsigned r){return uv(r?float(col)/10:.5f,float(r)/4);};
                const std::array<Float4,4> coords{point(column,row),point(column,row+1),point(column+1,row+1),point(column+1,row)};
                for(unsigned i:{1u,0u,2u,0u,2u,3u})result.vertices[vertex++].uv=coords[i];
            }
        }
    } else if(shape==43||shape==45) {
        for(unsigned ray=0;ray<8;++ray) {
            Float4 rotation{};float scale=1;
            if(shape==43) {
                for(unsigned axis=0;axis<3;++axis)rotation[axis]=float(random()%1024)*.001953125f;
                scale=.6f/float(random()%7+1)+.4f;
            } else rotation[2]=float(ray)/8;
            const auto matrix=effect_matrix({},rotation,{scale,scale,scale,1},sine,false);
            const auto transformed=[&](float x){
                Float4 p;
                for(unsigned axis=0;axis<4;++axis) {
                    float v=matrix[0][axis]*x;v=v+matrix[1][axis]*6;
                    v=v+matrix[2][axis]*0;v=v+matrix[3][axis];p[axis]=v;
                }
                return p;
            };
            const auto a=transformed(-1),b=transformed(1);
            const unsigned tag=shape==43?random()%7+1:1u<<(ray&1);
            append({0,0,0,1},{255,255,255,255},{},0);
            append(a,{128,128,128,0},{},tag);append(b,{128,128,128,0},{},tag);
        }
    } else {
        const std::array<Float4,3> base{Float4{-1.5f,0,0,0},Float4{-1.5f,-1.5f,0,0},Float4{0,-1.5f,0,0}};
        const std::array<Float4,3> coords{uv(0,1),uv(0,0),uv(1,0)};
        const auto offset=[&](float scale){
            const auto word=random();Float4 p{};
            for(unsigned axis=0;axis<3;++axis)p[axis]=float(int((word>>(axis*6))&63)-32)*scale;
            return p;
        };
        for(unsigned triangle=0;triangle<20;++triangle) {
            const auto common=offset(.1f);
            for(unsigned corner=0;corner<3;++corner) {
                auto p=base[corner];const auto noise=offset(.05f);
                for(unsigned axis=0;axis<3;++axis){p[axis]=p[axis]+common[axis];p[axis]=p[axis]+noise[axis];}
                append(p,{255,255,255,255},coords[corner],0);
            }
        }
    }
    for(auto& v:result.vertices)for(unsigned axis=0;axis<3;++axis) {
        const float scaled=v.position[axis]*64;
        if(!(scaled>=-32768.f&&scaled<=32767.f)||float(int(scaled))!=scaled)
            throw std::runtime_error("Procedural template position is not a packed int16/64 value");
        v.position[axis]=float(std::int16_t(int(scaled)))/64;
    }
    return result;
}
EffectTemplatePreparation::EffectTemplatePreparation(const std::vector<EffectDefinition>& bank,
        const EffectTextureSizes& sizes,const SineTable& sine,const std::function<std::uint32_t()>& source,
        EffectPreparationTrace* trace) {
    if(!source)throw std::runtime_error("Effect noise grid requires template RNG");
    std::uint32_t draws=0;
    const std::function<std::uint32_t()> random=[&]{++draws;return source();};
    noise=make_effect_noise_grid(random);
    if(trace){trace->grid_draws=draws;if(trace->grid_complete)trace->grid_complete();}
    const auto request=[&](const EffectDefinition& definition,const EffectElement& e) {
        const auto before=draws;bool constructed=false;
        const bool miss=!cache_owners[e.shape][0];
        if(!e.shape||e.shape>=geometry.size()||e.shape==44||e.shape==46)
            throw std::runtime_error("Unrecovered template preparation shape");
        if(!cache_owners[e.shape][0]) {
            if(e.shape>=10&&e.shape<=17) {
                auto g=std::make_shared<ProceduralEffectGeometry>();
                g->vertices=make_effect_noise_shape(e.shape,noise);
                g->position_tags.resize(g->vertices.size());g->uv_tags.resize(g->vertices.size());
                geometry[e.shape]=std::move(g);constructed=true;
            } else if(e.shape==43||e.shape==45||e.shape==55||e.shape==56||e.shape==62) {
                std::array<std::uint32_t,2> size{};
                if(e.shape==56||e.shape==62) {
                    if(e.texture<0||unsigned(e.texture)>=sizes.size())throw std::runtime_error("Missing prepared shape texture");
                    size=sizes[unsigned(e.texture)];
                }
                geometry[e.shape]=std::make_shared<const ProceduralEffectGeometry>(make_procedural_effect_shape(e.shape,sine,random,size));
                constructed=true;
            }
        }
        cache_owners[e.shape]={definition.id,e.id};
        if(trace) {
            trace->requests.push_back({definition.id,e.id,e.shape,miss});
            if(constructed){trace->construction_order.push_back(e.shape);trace->construction_draws[e.shape]=draws-before;}
        }
    };
    //0x10ba38 ordinary pass, including0x10b318 range expansion. References
    //copy existing jobs in the second pass and do not construct new geometry.
    for(const auto& d:bank) {
        if(d.flags&0x800)continue;
        for(const auto& e:d.elements) {
            if(e.flags&0x10000) {
                if(!e.shape||e.shape>=geometry.size()||e.shape>e.primitive_flags)
                    throw std::runtime_error("Unrecovered template expansion range");
                for(const auto& source:d.elements) {
                    if(source.id<e.shape)continue;
                    if(source.id>e.primitive_flags)break;
                    if(source.flags&0x10000)throw std::runtime_error("Unrecovered nested template range");
                    request(d,source);
                }
                cache_owners[e.shape]={d.id,e.id};
            } else request(d,e);
        }
    }
}
}
