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
