#include "okage/field.hpp"
#include <bit>
#include <cmath>
#include <cfenv>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
std::uint32_t get(Bytes b,std::size_t at) {
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("Field asset word bounds");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|(std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Field rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
}
std::uint32_t OpeningField::word(std::size_t at) const{return get(bytes_,at);}
Float4 OpeningField::point(std::size_t at) const {
    Float4 p{0,0,0,1};for(unsigned i=0;i<3;++i){p[i]=__builtin_bit_cast(float, (word(at+4*i)));if(!std::isfinite(p[i]))throw std::runtime_error("Invalid field point");}return p;
}
OpeningField::OpeningField(Bytes bytes,std::function<void(int)> start,std::function<bool(int)> wait,FieldActor* woman):
    bytes_(bytes.begin(),bytes.end()),script_(bytes,3),start_dialogue_(std::move(start)),wait_dialogue_(std::move(wait)) {
    woman_=woman;
    if(!start_dialogue_ || !wait_dialogue_)throw std::runtime_error("Field requires dialogue services");
    // This diagnostic entry is tied to the documented USA HOME1C script.
    if(word(0xb88)!=0xffe00007 || word(0xb8c)!=0x30 || word(0xbb8)!=0x20000a)
        throw std::runtime_error("Unsupported opening event layout");
    script_.set_service_callback([this](auto op,auto a,auto b,auto pc){return service(op,a,b,pc);});
    if(woman_) {
        if(word(0xb6c)!=0xff800007 || word(0xb70)!=0x400030 || word(0xb80)!=0x10002e)
            throw std::runtime_error("Unsupported opening actor setup layout");
        woman_->select_motion(1); // Original field actor construction 0x138220.
        script_.start_at(0xb6c);woman_->evaluate();
    } else script_.start_at(0xb88);
    evaluate();
}
Animation::ServiceResult OpeningField::service(unsigned op,unsigned a,std::int32_t b,std::uint32_t pc) {
    switch(op) {
        case 7:
            if(b==-2)script_.set_channel_target(nullptr);
            else if(b==-8 && woman_)script_.set_channel_target(&woman_->animation());
            else throw std::runtime_error("Field actor channel group not yet implemented");
            group_=b;return {pc+4};
        case 0x4b:
            if(b!=1)throw std::runtime_error("Field camera target mode not yet implemented");
            requested_target_=point(pc+4);return {pc+16};
        case 0x52: {
            Rounding rounding;const float rate=b ? 1.f/float(b) : 0.f;
            blend.begin(rate,script_.values().at(0),requested_target_);return {pc+4};
        }
        case 0x4c:
            if(a || b)throw std::runtime_error("Field dialogue mode not yet implemented");
            start_dialogue_(__builtin_bit_cast(std::int32_t, (word(pc+4))));return {pc+8};
        case 10:return wait_dialogue_(b) ? Animation::ServiceResult{pc+4} : Animation::ServiceResult{pc,true};
        case 0x36:
            if(b==-8 && woman_ && a<0x800) {woman_->select_motion(a);return {pc+4};}
            [[fallthrough]];
        case 0x3b:
            pending_opcode=op;pending_actor=b;pending_animation=a;
            return {pc,true};
        case 0x5b: {
            if(group_!=-8 || !woman_)throw std::runtime_error("Unsupported actor facing group");
            Rounding rounding;const auto destination=point(pc+4),position=woman_->animation().values().at(1);
            const float angle=camera_angle(-(destination[0]+(-position[0])),-(destination[2]+(-position[2])),angles_);
            if(b==0)woman_->animation().set_component(0,1,angle);
            else woman_->animation().turn_component(0,1,angle,b);
            return {pc+16};
        }
        default:throw std::runtime_error("Unrecovered field service "+std::to_string(op));
    }
}
void OpeningField::evaluate() {
    position=script_.values().at(0);target=requested_target_;blend.apply(position,target);
    rotation=camera_look_at(position,target,angles_,sine_);
}
void OpeningField::tick() {
    script_.tick_script();
    if(woman_)woman_->tick_script();
    script_.tick_channels();evaluate();
    if(woman_){woman_->tick_channels();woman_->evaluate();}
}
Matrix4 OpeningField::view() const {
    Float4 p=position,r=rotation;for(unsigned i=0;i<3;++i){p[i]=-p[i];r[i]=-r[i];}p[3]=1;r[3]=0;
    return camera_matrix(p,r,{1,1,1,0},sine_);
}
Float4 field_projection(Bytes stg) {
    Rounding rounding;
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const auto begin=get(stg,8),count=get(stg,12);
    if(begin<32 || count>4096 || std::uint64_t(begin)+std::uint64_t(count)*20>stg.size())throw std::runtime_error("STG settings bounds");
    Float4 settings{};bool found=false;
    for(unsigned i=0;i<count;++i)if(get(stg,begin+i*20)==0x44) {
        if(found)throw std::runtime_error("Multiple STG projection records");found=true;
        for(unsigned j=0;j<4;++j){settings[j]=__builtin_bit_cast(float, (get(stg,begin+i*20+4+j*4)));if(!std::isfinite(settings[j]))throw std::runtime_error("Non-finite STG projection");}
    }
    if(!found || settings[0]<=0 || settings[1]<=0 || settings[2]<=0 || settings[3]>=10)throw std::runtime_error("Unsupported STG projection");
    return {(settings[2]*settings[0])/320.f,(settings[2]*settings[1])/112.f,
        (1024.f+3.f)/(1024.f-3.f),((1024.f*-2.f)*3.f)/(1024.f-3.f)};
}
}
