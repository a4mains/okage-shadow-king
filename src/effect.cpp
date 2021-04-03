#include "okage/effect.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <set>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
struct Reader {
    Bytes b;
    void range(std::size_t p,std::size_t n)const{require(p<=b.size()&&n<=b.size()-p,"EFD record outside input");}
    std::uint32_t word(std::size_t p)const{range(p,4);return std::uint32_t(b[p])|(std::uint32_t(b[p+1])<<8)|(std::uint32_t(b[p+2])<<16)|(std::uint32_t(b[p+3])<<24);}
    float number(std::size_t p)const{const auto f=__builtin_bit_cast(float, (word(p)));require(std::isfinite(f),"Non-finite EFD value");return f;}
    Float4 vector(std::size_t p)const{return {number(p),number(p+4),number(p+8),number(p+12)};}
};
struct Rounding {
    int old=std::fegetround();
    Rounding(){require(!std::fesetround(FE_TOWARDZERO),"Cannot set effect rounding");}
    ~Rounding(){std::fesetround(old);}
};
unsigned angle(float t){return (static_cast<std::uint32_t>(static_cast<std::int32_t>(t*32768.0f))>>1)&16383;}
}
std::vector<EffectDefinition> read_efd(Bytes bytes) {
    Reader r{bytes};r.range(0,16);require(r.word(0)==0x20444645,"Invalid EFD signature");
    const auto count=r.word(4);require(count<=128,"EFD exceeds retail template limit");
    std::vector<EffectDefinition> result;std::set<std::uint32_t> ids;
    std::size_t p=16;
    for(unsigned i=0;i<count;++i) {
        r.range(p,48);EffectDefinition d;
        d.id=r.word(p);d.duration=r.word(p+4);d.flags=r.word(p+8);
        require(d.id<128&&ids.insert(d.id).second,"Invalid or duplicate EFD ID");
        const auto n=r.word(p+16);require(n<=(bytes.size()-p-48)/116,"EFD element count outside input");
        require(r.word(p+20)==p+48,"EFD element pointer does not match sequential records");
        d.parameters=r.vector(p+32);
        for(unsigned j=0;j<n;++j) {
            const auto a=p+48+j*116;EffectElement e;
            e.flags=r.word(a);e.id=r.word(a+4);e.shape=r.word(a+8);
            e.primitive_flags=r.word(a+12);e.channels=r.word(a+16);e.program=r.word(a+20);
            e.texture=__builtin_bit_cast(std::int32_t, (r.word(a+24)));e.change_time=r.number(a+28);e.next_program=r.word(a+32);
            for(unsigned k=0;k<5;++k)e.values[k]=r.vector(a+36+k*16);
            d.elements.push_back(e);
        }
        p+=48+n*116;result.push_back(std::move(d));
    }
    require(p==bytes.size(),"Trailing EFD data");return result;
}
MapEffect::MapEffect(const EffectDefinition& d,const Float4& p,const Float4& r,
                     const Float4& s,const EffectTextureSizes& sizes,
                     const std::function<std::uint32_t()>& random):
    id(d.id),flags(d.flags&~0x4000u),duration(d.duration),position(p),rotation(r),scale(s),
    parameters(d.parameters),texture_sizes_(sizes) {
    require((d.id==25&&d.elements.size()==8)||(d.id==34&&d.elements.size()==14),
            "Unrecovered map effect template");
    require(duration>0&&duration<=0x7fffffff,"Invalid map effect duration");
    Rounding rounding;inverse_duration=1.0f/float(duration);
    const std::array<Float4,5> defaults={Float4{1,1,1,1},Float4{},Float4{},
                                       Float4{1,1,1,1},Float4{0,1,1,0}};
    for(const auto& source:d.elements) {
        require(!(source.channels&~31u),"Unrecovered map effect channel modifiers");
        MapEffectElement e;
        const auto channel=[&](unsigned i){return (source.channels&(1u<<i))?source.values[i]:defaults[i];};
        e.position=channel(2);e.rotation=channel(1);e.scale=channel(3);e.color=channel(0);e.interval=channel(4);
        e.id=source.id;e.shape=source.shape;e.flags=source.flags;e.primitive_flags=source.primitive_flags;
        e.program=source.program|((source.primitive_flags&0x300u)<<22);
        e.change_time=source.change_time;e.next_program=source.next_program;
        e.rendered={e.position,e.rotation,e.scale,e.color};
        unsigned texture;
        switch(e.shape){case 24:texture=20;break;case 29:texture=5;break;
                        case 28:texture=4;break;case 31:texture=17;break;
                        default:throw std::runtime_error("Unrecovered map effect shape");}
        set_texture(e,texture);elements.push_back(e);
    }
    phase=random()%duration;
}
void MapEffect::set_texture(MapEffectElement& e,unsigned texture) {
    require(texture<texture_sizes_.size(),"Invalid logical effect texture");
    const auto size=texture_sizes_[texture];
    require(size[0]>0&&size[1]>0&&size[0]<=4096&&size[1]<=4096,"Missing effect texture dimensions");
    e.texture=texture;e.uv={0,0,std::uint16_t(size[0]-1),std::uint16_t(size[1]-1)};
}
void MapEffect::set_parameter(unsigned selector,const Float4& payload) {
    require(selector<=0xfff,"Invalid effect parameter selector");
    if(selector<0xf00)parameters.fill(float(selector));
    else {
        Float4* target=nullptr;
        switch(selector){case 0xfff:target=&rotation;break;case 0xffe:target=&scale;break;case 0xffd:target=&color;break;}
        // Other reserved selectors consume their payload but perform no write.
        if(target)*target={payload[0],payload[1],payload[2],1};
    }
}
std::vector<MapEffectDraw> map_effect_draws(const MapEffect& effect,const Matrix4& view,
        const Float4& camera,float far_clip,const SineTable& sine) {
    if(effect.flags&0x4300)return {};
    require(effect.id==25&&effect.elements.size()==8,"Unrecovered map effect geometry template");
    require(!(effect.flags&0x400),"Deferred map effect rendering is unrecovered");
    require(std::isfinite(far_clip)&&far_clip>0,"Invalid effect far clip");
    Rounding rounding;
    const auto job=effect_matrix(effect.position,effect.rotation,effect.scale,sine,false);
    float fade=1;
    if(effect.flags&0x1000) {
        float z=view[0][2]*effect.position[0];z+=view[1][2]*effect.position[1];
        z+=view[2][2]*effect.position[2];z+=view[3][2];
        if(far_clip<z+40)fade=std::max(0.0f,(far_clip-z)*.025f);
    }
    std::vector<MapEffectDraw> result;
    for(const auto& e:effect.elements) {
        require((e.primitive_flags&255)==1&&!(e.flags&0x400000),"Unrecovered map effect material");
        require(e.uv[0]==0&&e.uv[1]==0&&!e.render_flags,"Unrecovered map effect texture region");
        auto rotation=e.rendered[1];
        switch(e.flags&15){case 0:break;case 1:
            rotation[0]=(1-camera[0])-effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];rotation[3]=camera[3];break;
            default:throw std::runtime_error("Unrecovered map effect orientation");}
        auto matrix=effect_matrix(e.rendered[0],rotation,e.rendered[2],sine,true);
        matrix=compose_pose(job,std::span(&matrix,1))[0];
        matrix=compose_pose(view,std::span(&matrix,1))[0];
        Float4 color{};for(unsigned i=0;i<4;++i)color[i]=e.rendered[3][i]*effect.color[i];color[3]*=fade;
        MapEffectDraw draw{matrix,color,e.texture,e.uv,{}};
        const auto base=effect.texture_sizes().at(e.shape==24?20:5);
        const auto current=effect.texture_sizes().at(e.texture);
        const bool replace=(e.program>>30)==1;
        //0x1122e8 constructs T using height+1.0x1055d8 rescales the
        // original packed UVs with truncated5-bit fixed ratios on replacement.
        auto uv=[&](float u,float v){
            int s=int(u*float(base[0])*32),t=int(v*float(base[1]+1)*32);
            require(s>=0&&s<=32767&&t>=0&&t<=32767,"Unrecovered effect UV packing range");
            if(replace){s=(s*int((current[0]<<5)/base[0]))>>5;t=(t*int((current[1]<<5)/base[1]))>>5;}
            return Float4{float(std::uint16_t(s))/32768,float(std::uint16_t(t))/32768,0,0};
        };
        auto quad=[&](const std::array<Float4,4>& p,const std::array<Float4,4>& c,const std::array<Float4,4>& uv) {
            // Original strip order B,A,C,D, hence the shared A-C diagonal.
            for(unsigned i:{1u,0u,2u,0u,2u,3u})draw.vertices.push_back({p[i],c[i],uv[i]});
        };
        const Float4 white{255,255,255,255},clear{255,255,255,0};
        if(e.shape==24) {
            auto point=[&](unsigned i,float y) {
                //0x113d88 packs nearest signed1/64 positions. Source W is a
                // separate byte tag; VU rendering supplies homogeneous1.
                return Float4{std::round(sine[(i*1024+4096)&16383]*64)/64,y,
                              std::round(sine[(i*1024)&16383]*64)/64,1};
            };
            for(unsigned i=0;i<16;++i){
                const float a=float(i)/16,b=float(i+1)/16;
                quad({point(i,-3),point(i,0),point(i+1,0),point(i+1,-3)},
                    {clear,white,white,clear},
                    {uv(a,0),uv(a,1),uv(b,1),uv(b,0)});
            }
        } else if(e.shape==29) {
            quad({Float4{-.5f,-1,0,1},Float4{.5f,-1,0,1},Float4{.5f,0,0,1},Float4{-.5f,0,0,1}},
                 {white,white,white,white},
                 {uv(0,0),uv(1,0),uv(1,1),uv(0,1)});
        } else throw std::runtime_error("Unrecovered map effect primitive geometry");
        result.push_back(std::move(draw));
    }
    return result;
}
void MapEffect::tick(const std::function<std::uint32_t()>& random,bool clock_enabled) {
    if(!clock_enabled)return;
    require(!(flags&0x100),"Cannot update a removed map effect");
    require(id==25,"Unrecovered map effect update program");
    Rounding rounding;
    if(phase>duration){if(!(flags&0x8000)){request_removal();return;}phase-=duration;}
    const float time=float(phase)*inverse_duration;
    for(auto& e:elements) {
        require(e.change_time>=1,"Unrecovered map effect program change");
        require(e.interval[0]<e.interval[1],"Invalid map effect interval");
        float t=time<e.interval[0]?0:time>e.interval[1]?1:(time-e.interval[0])/(e.interval[1]-e.interval[0]);
        if(!(flags&0x20)) {
            const auto mode=e.program>>30;
            require(mode!=3&&!(e.flags&0x800000),"Unrecovered map effect texture mode");
            if(mode)set_texture(e,unsigned(e.interval[2]));
            e.interval[2]=0;
        }
        auto program=e.program;
        if((e.flags&0x2000)&&e.interval[2]<t)program&=0xc0ffffff;
        const auto timing=(program>>24)&63;
        require(timing==0||timing==7,"Unrecovered map effect time program");
        if(timing==7){t=t*3.0f+e.interval[3];t-=float(int(t));}
        if((e.flags&0x1000)&&e.interval[2]<t)program&=0xfffc0fff;
        auto& p=e.rendered[0];auto& r=e.rendered[1];auto& s=e.rendered[2];auto& c=e.rendered[3];
        switch((program>>12)&63) {
        case 0:break;
        case 42:
            if(e.interval[2]<t) {
                const auto theta=angle(t*e.position[3]+e.position[2]);
                p[0]=e.position[0]*sine_[(theta+4096)&16383];
                p[1]=e.position[1]+sine_[angle((t*3.0f)*e.position[3])];
                p[2]=e.position[0]*sine_[theta];
            } else {
                e.position[1]=float(int(random()%70)-100)*.1f;
                e.position[2]=float(random()%100)*.01f;
                e.position[3]=float(int(random()%100)-50)*.02f;
            }
            break;
        default:throw std::runtime_error("Unrecovered map effect position program");
        }
        if((e.flags&0x4000)&&e.interval[2]<t)program&=0xfffff03f;
        switch((program>>6)&63){case 0:break;case 11:r[1]=float(random()%360+360)*.00278f;break;
                              default:throw std::runtime_error("Unrecovered map effect rotation program");}
        switch((program>>18)&63){case 0:break;case 13:s[0]=t*e.scale[0];s[1]=(1.0f-t)*e.scale[1];s[2]=t*e.scale[2];break;
                               default:throw std::runtime_error("Unrecovered map effect scale program");}
        if((e.flags&0x8000)&&e.interval[2]<t)program&=0xffffffc0;
        switch(program&63) {
        case 0:break;
        case 15:case 17: {
            const float head=(program&63)==15?.6f:.4f,tail=(program&63)==15?.8f:.7f;
            float alpha=0;
            if(t!=0&&t!=1){alpha=1;if(t>tail)alpha=1.0f-(t-tail)/(1.0f-tail);else if(t<head)alpha=t/head;alpha*=e.color[3];}
            c[3]=alpha;break;
        }
        default:throw std::runtime_error("Unrecovered map effect color program");
        }
        e.interval[2]=t;
        if(!(flags&0x20)) {
            if(e.flags&0x200)e.program&=0xc0ffffff;
            if(e.flags&0x100)e.program&=0xfffc0fff;
            if(e.flags&0x400)e.program&=0xfffff03f;
            if(e.flags&0x800)e.program&=0xffffffc0;
        }
    }
    flags|=0x20;if(!(flags&0x2000))++phase;
}
BubbleEffect::BubbleEffect(const EffectDefinition& d,int parameter):definition_(d),parameter_(parameter) {
    require(d.id==30&&d.flags==0x8400&&d.duration>0&&d.elements.size()==2,"Unsupported speech-bubble definition");
    require(parameter>=0&&parameter<32,"Unsupported speech-bubble symbol");
    flags=d.flags&~0x4000u;
    for(unsigned i=0;i<2;++i) {
        const auto& e=d.elements[i];auto& s=sprites[i];
        require(e.shape==34&&e.flags==(i?0x241u:1u)&&e.channels==(i?29u:25u)&&
                e.program==(i?0x1961e000u:0x00600000u)&&e.primitive_flags==(i?0x140u:0x40u)&&
                e.texture==int(14+i),"Unsupported speech-bubble element program");
        require(e.values[4][0]==0&&e.values[4][1]==1&&e.change_time>1,"Unsupported speech-bubble time interval");
        s.color=e.values[0];s.rotation={0,0,0,0};s.position=i?e.values[2]:Float4{};
        s.scale=e.values[3];s.damping=s.scale[3];s.previous_time=e.values[4][2];
        s.program=e.program|((e.primitive_flags&0x300)<<22);
        s.texture=14;s.uv={0,0,31,31};
    }
}
void BubbleEffect::tick(const Float4& camera) {
    require(!(flags&0x100),"Cannot update a removed effect");
    for(float f:camera)require(std::isfinite(f)&&std::abs(f)<1000,"Invalid effect camera rotation");
    Rounding rounding;
    if(phase>definition_.duration)phase-=definition_.duration;
    const float time=float(phase)*(1.0f/float(definition_.duration));
    const auto inverse=instance_matrix({0,0,0,1},{1.0f-camera[0],1.0f-camera[1],0,0},{1,1,1,1},sine_);
    for(unsigned i=0;i<2;++i) {
        auto& s=sprites[i];const auto& e=definition_.elements[i];
        if(i) {
            if(!(flags&0x20)) {
                s.texture=parameter_<16?15:16;
                const auto x=std::uint16_t((parameter_&15)*16);s.uv={x,0,std::uint16_t(x+16),15};
                s.program&=0xc0ffffff;
            }
            const auto a=e.values[2];
            Float4 p{sine_[angle(time)]*a[3]+a[0],
                     sine_[angle(time+time-float(int(time+time)))]*a[3]+a[1],0,0};
            for(unsigned r=0;r<3;++r)s.position[r]=inverse[0][r]*p[0]+inverse[1][r]*p[1]+inverse[2][r]*p[2];
        }
        const float cycle=time*20.0f-float(int(time*20.0f));
        s.scale[1]=(1.0f-(sine_[angle(cycle)]*s.damping+s.damping))*e.values[3][1];
        s.damping*=0.9f;s.previous_time=time;
    }
    flags|=0x20;++phase;
}
}
