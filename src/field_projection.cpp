#include "okage/field_projection.hpp"
#include <bit>
#include <cmath>
#include <cfenv>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Projection STG bounds");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
        (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
}
void FieldProjection::reset_scene() {
    if(phase_!=Phase::Idle)throw std::runtime_error("Scene reset during projection submission");
    current_.depth_min=101;current_.depth_max=16777115;current_.focal=1024;
    current_.depth_near=current_.clip_near=3;current_.depth_far=current_.clip_far=1024;
    current_.fog_near=4096;current_.fog_far=16384;
    field_far_=1024;field_fog_near_=4096;field_fog_far_=16384;
    frame_={current_,current_,current_,current_,current_,current_};loaded_=false;
}
void FieldProjection::load_stage(Bytes bytes) {
    if(phase_!=Phase::Idle)throw std::runtime_error("Projection stage changed during submission");
    if(word(bytes,0)!=0x475453)throw std::runtime_error("Invalid projection STG");
    const auto begin=word(bytes,8),count=word(bytes,12);
    if(begin<32||count>4096||std::uint64_t(begin)+std::uint64_t(count)*20>bytes.size())
        throw std::runtime_error("Projection STG settings bounds");
    auto next=current_;
    //0x1b8028 ->0x1b7f98/0x1b7fc0 reset shared and field planes/fog.
    next.depth_min=101;next.depth_max=16777115;next.focal=1024;
    next.depth_near=next.clip_near=3;next.depth_far=next.clip_far=1024;
    next.fog_near=4096;next.fog_far=16384;
    bool projection=false;
    for(unsigned i=0;i<count;++i) {
        const auto at=begin+i*20,kind=word(bytes,at);
        if(kind!=0x43&&kind!=0x44)continue;
        Float4 v;
        for(unsigned j=0;j<4;++j) {
            v[j]=__builtin_bit_cast(float, (word(bytes,at+4+j*4)));
            if(!std::isfinite(v[j]))throw std::runtime_error("Nonfinite projection STG value");
        }
        //0x1bb088 applies records in order, including repeated records.
        if(kind==0x43){next.fog_near=v[0];next.fog_far=v[1];}
        else {
            projection=true;next.scale_x=v[0];next.scale_y=v[1];next.focal=v[2];
            if(v[3]>=10){next.depth_near=next.clip_near=3;next.depth_far=next.clip_far=v[3];}
        }
    }
    if(!projection)throw std::runtime_error("Field projection requires explicit STG scale/focal");
    (void)shadow_screen_parameters(next);
    current_=next;field_far_=next.clip_far;
    field_fog_near_=next.fog_near;field_fog_far_=next.fog_far;
    //0x101260 configures effect far/fog, preserving the shared current near.
    synchronize_stage_effects();
}
void FieldProjection::apply_stage_setting(unsigned kind,const Float4& value,float depth_min,float depth_max) {
    if(phase_!=Phase::Idle)throw std::runtime_error("Projection stage changed during submission");
    for(float f:value)if(!std::isfinite(f))throw std::runtime_error("Nonfinite projection STG value");
    auto next=current_;
    if(kind==0x43){next.fog_near=value[0];next.fog_far=value[1];}
    else if(kind==0x44) {
        next.scale_x=value[0];next.scale_y=value[1];next.focal=value[2];
        if(value[3]>=10) {
            next.depth_near=next.clip_near=3;next.depth_far=next.clip_far=value[3];
            next.depth_min=depth_min;next.depth_max=depth_max;
        }
    } else throw std::runtime_error("Unsupported projection stage setter");
    (void)shadow_screen_parameters(next);
    current_=next;
    if(kind==0x43){field_fog_near_=value[0];field_fog_far_=value[1];}
    else if(value[3]>=10)field_far_=value[3];
}
void FieldProjection::synchronize_stage_effects() {
    if(phase_!=Phase::Idle)throw std::runtime_error("Effect projection sync during submission");
    effect_far_=field_far_;effect_fog_near_=field_fog_near_;effect_fog_far_=field_fog_far_;
    loaded_=true;frame_={current_,current_,current_,current_,current_,current_};
}
void FieldProjection::submit_empty_resource_frame(bool effect_pass,float depth_min,float depth_max) {
    if(phase_!=Phase::Idle)throw std::runtime_error("Resource projection lifecycle order");
    current_.depth_near=current_.clip_near=3;current_.depth_far=current_.clip_far=2048;
    current_.depth_min=depth_min;current_.depth_max=depth_max;
    current_.fog_near=4096;current_.fog_far=16384;
    if(effect_pass) {
        phase_=Phase::Field;
        begin_effects();begin_deferred();end_effects();
    }
}
void FieldProjection::begin_field() {
    if(!loaded_||phase_!=Phase::Idle)throw std::runtime_error("Field projection lifecycle order");
    //0x1b9288 copies the previous descriptor before ordinary settings change.
    frame_.shadow=current_;
    current_.depth_near=3;current_.depth_far=2048;
    current_.clip_near=3;current_.clip_far=field_far_;
    current_.fog_near=field_fog_near_;current_.fog_far=field_fog_far_;
    frame_.ordinary=current_;phase_=Phase::Field;++frames_;
}
void FieldProjection::begin_field(float depth_min,float depth_max) {
    if(!std::isfinite(depth_min)||!std::isfinite(depth_max)||depth_min<0||depth_max<depth_min||depth_max>=16777216.f)
        throw std::runtime_error("Invalid resource depth cache range");
    begin_field();
    current_.depth_min=depth_min;current_.depth_max=depth_max;
    frame_.ordinary=current_;
}
void FieldProjection::begin_effects() {
    if(phase_!=Phase::Field)throw std::runtime_error("Effect projection lifecycle order");
    //0x1b9be0 epilogue, then0x102218. Depth planes survive the effect passes.
    current_.depth_near=current_.clip_near=3;current_.depth_far=current_.clip_far=2048;
    current_.fog_near=4096;current_.fog_far=16384;frame_.field_end=current_;
    current_.half_width=1024;current_.half_height=512;
    current_.clip_near=effect_near_;current_.clip_far=effect_far_;
    current_.fog_near=effect_fog_near_;current_.fog_far=effect_fog_far_;
    frame_.effects=current_;phase_=Phase::Effects;
}
void FieldProjection::select_effect_near(float near) {
    if(phase_!=Phase::Effects&&phase_!=Phase::Deferred)
        throw std::runtime_error("Effect near outside submission");
    if(!std::isfinite(near)||near<=0||near>=effect_far_)
        throw std::runtime_error("Invalid effect clipping plane");
    if(near!=effect_near_) {
        current_.clip_near=effect_near_=near;current_.clip_far=effect_far_;
    }
}
void FieldProjection::begin_deferred() {
    if(phase_!=Phase::Effects)throw std::runtime_error("Deferred projection lifecycle order");
    current_.half_width=320;current_.half_height=112; //0x102218 epilogue.
    current_.fog_near=0;current_.fog_far=0; //0x102cf8, even with an empty queue.
    frame_.deferred=current_;phase_=Phase::Deferred;
}
void FieldProjection::end_effects() {
    if(phase_!=Phase::Deferred)throw std::runtime_error("Projection frame lifecycle order");
    frame_.ending=current_;phase_=Phase::Idle;
}
Float4 field_clip_projection(const ShadowScreenSettings& s) {
    (void)shadow_screen_parameters(s);
    const int old=std::fegetround();
    if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Projection rounding unavailable");
    const float range=s.clip_far-s.clip_near;
    const Float4 result{(s.focal*s.scale_x)/320.f,(s.focal*s.scale_y)/112.f,
        (s.clip_far+s.clip_near)/range,((s.clip_far*-2.f)*s.clip_near)/range};
    std::fesetround(old);return result;
}
std::array<float,15> field_projection_words(const ShadowScreenSettings& s) {
    return {s.center_x,s.center_y,s.half_width,s.half_height,s.depth_near,s.depth_far,
        s.clip_near,s.clip_far,s.depth_min,s.depth_max,s.fog_near,s.fog_far,s.scale_x,s.scale_y,s.focal};
}
}
