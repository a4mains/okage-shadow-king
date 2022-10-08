#include "okage/battle_camera.hpp"
#include "okage/animation.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Battle camera rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
std::uint32_t word(Bytes b,std::size_t i) {
    return unsigned(b[i])|(unsigned(b[i+1])<<8)|(unsigned(b[i+2])<<16)|(unsigned(b[i+3])<<24);
}
void validate(const BattleCameraPreset& p) {
    if(p.position_mode>1||p.target_mode>1||p.position_end[3]!=0)
        throw std::runtime_error("Unrecovered battle camera mode or anchored endpoint");
    for(float f:{p.position_duration,p.target_duration,p.projection_duration,p.projection_value})
        if(!std::isfinite(f)||f<0)throw std::runtime_error("Battle camera parameter bounds");
    for(const auto& v:{p.position_start,p.position_end,p.target_start,p.target_end})
        for(auto f:v)if(!std::isfinite(f)||std::fabs(f)>1e6f)
            throw std::runtime_error("Battle camera point bounds");
    if((p.position_mode==1&&p.position_duration==0)||(p.target_mode==1&&p.target_duration==0))
        throw std::runtime_error("Battle camera linear duration must be positive");
}
Float4 increment(const Float4& from,const Float4& to,float duration,float timestep) {
    // 0x1e23c0 divides by duration/timestep using VU reciprocal then multiply;
    // it is not the algebraically simplified (to-from)*timestep/duration.
    const float divisor=duration/timestep,reciprocal=1.f/divisor;
    Float4 result;
    for(unsigned j=0;j<4;++j){const float delta=to[j]-from[j];result[j]=delta*reciprocal;}
    return result;
}
}
BattleCameraPreset read_battle_camera_preset(Bytes b) {
    if(b.size()!=0x90)throw std::runtime_error("Battle camera preset extent");
    auto f=[&](unsigned at){return __builtin_bit_cast(float, (word(b,at)));};
    auto v=[&](unsigned at){return Float4{f(at),f(at+4),f(at+8),f(at+12)};};
    BattleCameraPreset p{word(b,0),word(b,0x30),f(4),f(0x34),v(0x10),v(0x20),v(0x40),v(0x50),f(0x64),f(0x70)};
    validate(p);return p;
}
BattleCameraMotion::BattleCameraMotion(const BattleCameraSequence& sequence,float timestep):
    sequence_(sequence),timestep_(timestep),angles_(make_atan_table()) {
    if(!sequence_[0]||!std::isfinite(timestep_)||timestep_<=0)
        throw std::runtime_error("Battle camera sequence/timestep bounds");
    for(const auto& p:sequence_)if(p)validate(*p);
    Rounding rounding;select(*sequence_[0]);
}
void BattleCameraMotion::select(const BattleCameraPreset& p) {
    flags_&=~0x40000000u;
    target_.mode=p.target_mode;target_.remaining=p.target_duration;
    target_.point=p.target_start;target_.endpoint=p.target_end;
    if(p.target_mode==1)target_.auxiliary=increment(target_.point,target_.endpoint,target_.remaining,timestep_);
    flags_&=~0x80000000u;
    position_.mode=p.position_mode;position_.remaining=p.position_duration;
    position_.point=p.position_start;position_.endpoint=p.position_end;
    if(p.position_mode==1)position_phase_=0;
    projection_dirty_=1;projection_duration_=p.projection_duration;projection_value_=p.projection_value;
    aim();
}
void BattleCameraMotion::aim() {
    const float x=target_.point[0]-position_.point[0],y=target_.point[1]-position_.point[1],z=target_.point[2]-position_.point[2];
    const float xx=x*x,zz=z*z;
    position_.auxiliary[0]=camera_angle(-y,std::sqrt(xx+zz),angles_);
    position_.auxiliary[1]=camera_angle(x,z,angles_);
    position_.auxiliary[2]=0;
}
void BattleCameraMotion::tick() {
    advance(nullptr,nullptr);
}
void BattleCameraMotion::tick(Animation& channels,const std::function<void(float)>& projection) {
    if(!projection)throw std::runtime_error("Battle camera requires projection sink");
    advance(&channels,&projection);
}
void BattleCameraMotion::advance(Animation* channels,const std::function<void(float)>* projection) {
    Rounding rounding;
    // 0x14f0d8 runs before motion updates. A null next segment marks readiness
    // and still updates the old modes that tick; exhausting slot2 returns early.
    if(sequence_phase_<3&&(flags_&0xc0000000u)==0xc0000000u) {
        ++sequence_phase_;
        if(sequence_phase_==3)return;
        if(sequence_[sequence_phase_])select(*sequence_[sequence_phase_]);
        else sequence_phase_=3;
    }
    if(channels)position_.auxiliary=channels->value(1);
    if(target_.mode==1)for(unsigned j=0;j<4;++j)target_.point[j]+=target_.auxiliary[j];
    target_.remaining-=timestep_;
    if(target_.remaining<0) {
        target_.remaining=0;
        if(target_.mode==1)target_.point=target_.endpoint;
        target_.mode=7;flags_|=0x40000000u;
    }
    if(position_.mode==1) {
        if(position_phase_==0) {
            position_.step=increment(position_.point,position_.endpoint,position_.remaining,timestep_);
            ++position_phase_;
        }
        if(position_phase_==1) {
            for(unsigned j=0;j<4;++j)position_.point[j]+=position_.step[j];
            position_.remaining-=timestep_;
            if(position_.remaining<0) {
                position_.remaining=0;position_.mode=7;flags_|=0x80000000u;position_phase_=0;
            }
        }
    } else {
        position_.remaining-=timestep_;
        if(position_.remaining<0){position_.remaining=0;position_.mode=7;flags_|=0x80000000u;}
    }
    aim();
    if(projection&&projection_dirty_==1) {
        (*projection)(projection_value_);
        projection_dirty_=2;
    }
    if(channels)channels->set_channel(1,position_.auxiliary);
}
std::optional<float> BattleCameraMotion::take_projection_request() {
    if(projection_dirty_!=1)return std::nullopt;
    projection_dirty_=2;return projection_value_;
}
std::array<std::uint32_t,56> BattleCameraMotion::observation() const {
    std::array<std::uint32_t,56> out{};
    auto bits=[](float f){return __builtin_bit_cast(std::uint32_t, (f));};
    auto lane=[&](unsigned at,const Lane& p) {
        out[at]=p.mode;out[at+1]=bits(p.remaining);
        unsigned off=at+4;
        for(const auto& v:{p.point,p.auxiliary,p.step,p.reserved,p.endpoint})for(auto f:v)out[off++]=bits(f);
    };
    lane(0,position_);lane(24,target_);
    out[48]=bits(projection_duration_);out[49]=bits(projection_value_);out[51]=projection_dirty_;
    out[52]=flags_;out[53]=position_phase_;out[54]=0xffffffffu;out[55]=sequence_phase_;
    return out;
}
}
