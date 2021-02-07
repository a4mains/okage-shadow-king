#include "okage/field.hpp"
#include <algorithm>
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
OpeningField::OpeningField(Bytes bytes,std::function<void(int)> start,std::function<bool(int)> wait,std::span<const ActorBinding> actors):
    bytes_(bytes.begin(),bytes.end()),script_(bytes,3),start_dialogue_(std::move(start)),wait_dialogue_(std::move(wait)) {
    actors_.assign(actors.begin(),actors.end());
    for(std::size_t i=0;i<actors_.size();++i) {
        const auto& binding=actors_[i];
        if(binding.alias>=0 || binding.alias < -2048 || binding.alias==-2 || !binding.actor)throw std::runtime_error("Invalid actor binding");
        for(std::size_t j=0;j<i;++j)if(actors_[j].alias==binding.alias || actors_[j].actor==binding.actor)throw std::runtime_error("Duplicate actor binding");
        binding.actor->select_motion(1); // Original field actor construction 0x138220.
    }
    if(!start_dialogue_ || !wait_dialogue_)throw std::runtime_error("Field requires dialogue services");
    if(!actors_.empty() && !actor(-8))throw std::runtime_error("Opening actor diagnostic requires MAM alias -8");
    if(actor(-1) && !actor(-9))throw std::runtime_error("Opening player diagnostic requires IMO alias -9");
    // This diagnostic entry is tied to the documented USA HOME1C script.
    if(word(0xb88)!=0xffe00007 || word(0xb8c)!=0x30 || word(0xbb8)!=0x20000a)
        throw std::runtime_error("Unsupported opening event layout");
    script_.set_service_callback([this](auto op,auto a,auto b,auto pc){return service(op,a,b,pc);});
    if(actor(-8)) {
        if(word(0xb6c)!=0xff800007 || word(0xb70)!=0x400030 || word(0xb80)!=0x10002e)
            throw std::runtime_error("Unsupported opening actor setup layout");
        if(actor(-9) && word(0xb68)!=0xff71003b)throw std::runtime_error("Unsupported opening visibility layout");
        if(actor(-1) && word(0xb64)!=0xfff1003b)throw std::runtime_error("Unsupported opening player visibility layout");
        script_.start_at(actor(-1) ? 0xb64 : (actor(-9) ? 0xb68 : 0xb6c));
        for(const auto& binding:actors_)binding.actor->evaluate();
    } else script_.start_at(0xb88);
    evaluate();
}
FieldActor* OpeningField::actor(int alias) const {
    for(const auto& binding:actors_)if(binding.alias==alias)return binding.actor;
    return nullptr;
}
void OpeningField::load_effects(Bytes bytes) {
    if(!effect_definitions_.empty())throw std::runtime_error("Field effects already loaded");
    effect_definitions_=read_efd(bytes);
}
void OpeningField::load_exploration_camera(Bytes stg) {
    if(!actor(-1)||exploration_camera_)throw std::runtime_error("Exploration camera requires a player and one configuration");
    exploration_camera_=field_anchored_camera(stg);
}
Animation::ServiceResult OpeningField::service(unsigned op,unsigned a,std::int32_t b,std::uint32_t pc) {
    auto pending=[&] {pending_opcode=op;pending_actor=b;pending_animation=a;return Animation::ServiceResult{pc,true};};
    switch(op) {
        case 7:
            if(b==-2)script_.set_channel_target(nullptr);
            else if(auto* target=actor(b))script_.set_channel_target(&target->animation());
            else return pending();
            group_=b;return {pc+4};
        case 0x4b:
            if(b==0) {
                // 0x130670 resets the automatic rate, begins a .01 blend and
                // selects mode 0. Rotation retains the last evaluated channel.
                blend.begin(0.01f,script_.values().at(0),requested_target_);
                camera_mode_=0;return {pc+4};
            }
            if(b!=1)throw std::runtime_error("Field camera target mode not yet implemented");
            requested_target_=point(pc+4);camera_mode_=2;return {pc+16};
        case 0x52: {
            Rounding rounding;const float rate=b ? 1.f/float(b) : 0.f;
            blend.begin(rate,script_.values().at(0),requested_target_);return {pc+4};
        }
        case 0x4c:
            if(a || b)throw std::runtime_error("Field dialogue mode not yet implemented");
            start_dialogue_(__builtin_bit_cast(std::int32_t, (word(pc+4))));return {pc+8};
        case 10:return wait_dialogue_(b) ? Animation::ServiceResult{pc+4} : Animation::ServiceResult{pc,true};
        case 0x36:
            if(auto* target=actor(b);target && a<0x800) {target->select_motion(a);return {pc+4};}
            return pending();
        case 0x3b:
            if(auto* target=actor(b);target && (a&0x100)) {target->visible=(a&255)!=0;return {pc+4};}
            return pending();
        case 0x45:
            if(audio_service && audio_service(a,b)){pending_opcode=0;return {pc+4};}
            return pending();
        case 0x68:
            if(sound_cue && sound_cue(a,b,__builtin_bit_cast(float, (word(pc+4))),word(pc+8))){pending_opcode=0;return {pc+12};}
            return pending();
        case 0x47: {
            const auto selector=word(pc+4),index=selector&0xffff,bank=selector>>16;
            if(bank>4 || (bank==4 && index==6))throw std::runtime_error("Unrecovered field variable storage or controller side effect");
            if(!set_variable)return pending();
            set_variable(index,bank,b);pending_opcode=0;return {pc+8};
        }
        case 0x5e: {
            const auto definition=std::find_if(effect_definitions_.begin(),effect_definitions_.end(),[](const auto& d){return d.id==30;});
            if(definition==effect_definitions_.end())throw std::runtime_error("Effect 0x5e requires owned TES.EFD and effect textures");
            if(!actor(b))throw std::runtime_error("Unresolved effect attachment actor");
            unsigned handle=0;while(handle<effects_.size()&&effects_[handle])++handle;
            if(handle==effects_.size())throw std::runtime_error("Field effect capacity exhausted");
            effects_[handle]=std::make_unique<AttachedEffect>(*definition,int(a),b);
            effect_ring_[effect_ring_index_]=handle;effect_ring_index_=(effect_ring_index_+1)&15;
            return {pc+4};
        }
        case 0x5c: {
            if(a<0x100)throw std::runtime_error("Effect replacement template is unrecovered");
            const auto handle=(b&0x100)?unsigned(b&255):effect_ring_[(effect_ring_index_-unsigned(b+1))&15];
            if(handle>=effects_.size())throw std::runtime_error("Invalid effect removal handle");
            if(effects_[handle])effects_[handle]->animation.request_removal();
            return {pc+4};
        }
        case 0x5a:
        case 0x5b: {
            auto* moving=actor(group_);auto* facing=op==0x5a ? actor(b) : nullptr;
            if(!moving || (op==0x5a && !facing))return pending();
            Rounding rounding;
            const auto destination=facing ? facing->animation().values().at(1) : point(pc+4);
            const auto position=moving->animation().values().at(1);
            const float angle=camera_angle(-(destination[0]+(-position[0])),-(destination[2]+(-position[2])),angles_);
            const auto duration=op==0x5a ? int(a) : b;
            if(duration==0)moving->animation().set_component(0,1,angle);
            else moving->animation().turn_component(0,1,angle,duration);
            return {pc+(op==0x5a ? 4u : 16u)};
        }
        default:throw std::runtime_error("Unrecovered field service "+std::to_string(op));
    }
}
void OpeningField::evaluate() {
    position=script_.values().at(0);target=requested_target_;blend.apply(position,target);
    if(camera_mode_==2) {
        rotation=camera_look_at(position,target,angles_,sine_);
        // Original look-at 0x1b9440 writes the result back into channel 1.
        script_.set_channel(1,rotation);
    } else {
        rotation=script_.values().at(1);
        requested_target_=target;
    }
}
void OpeningField::tick() {
    // Field manager 0x133a98 runs before the event pass. Case 5 detects the
    // ended script and selects the room camera; case 0 updates it next frame.
    if(exploration_camera_&&!event_running()) {
        if(exploration_handoff_) {
            const auto camera=anchored_camera(actor(-1)->animation().values().at(1),*exploration_camera_,angles_,sine_);
            script_.set_channel(0,camera.position);requested_target_=camera.target;camera_mode_=2;
        } else exploration_handoff_=true;
    }
    script_.tick_script();
    for(const auto& binding:actors_)binding.actor->tick_script();
    script_.tick_channels();evaluate();
    // Actor groups have flag1 (not global-advance flag2). Original 0x1b9be0
    // calls 0x1b70a0 only for visible instances; hidden motion scripts still run.
    for(const auto& binding:actors_)if(binding.actor->visible){binding.actor->tick_channels();binding.actor->evaluate();}
    for(auto& effect:effects_)if(effect) {
        auto& flags=effect->animation.flags;
        if(flags&0x200){effect.reset();continue;}
        if(flags&0x100){flags|=0x200;continue;}
        Rounding rounding;
        effect->position=actor(effect->actor_alias)->animation().values().at(1);
        effect->position[1]-=8.5f;
        effect->animation.tick({-rotation[0],-rotation[1],-rotation[2],0});
    }
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
AnchoredCamera field_anchored_camera(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const std::size_t begin=get(stg,16),count=get(stg,20),end=get(stg,24);
    if(begin<32||begin%4||!count||count>30||end>stg.size()||begin+count*28>end||
       (end-begin-count*28)%28||(end-begin-count*28)/28>30)
        throw std::runtime_error("Invalid STG camera tables");
    auto number=[&](std::size_t at) {
        const auto v=__builtin_bit_cast(float, (get(stg,at)));
        if(!std::isfinite(v))throw std::runtime_error("Non-finite STG camera setting");
        return v;
    };
    AnchoredCamera result;result.anchor={number(begin+4),number(begin+8),number(begin+12),1};
    for(auto at=begin+count*28;at<end;at+=28) {
        // 0x138a50 selects the first settings record with signed ID zero.
        if((get(stg,at)>>16)!=0)continue;
        if(number(at+16)!=1)throw std::runtime_error("Unrecovered STG exploration camera mode");
        result.distance_offset=number(at+4);result.distance_scale=number(at+8);
        result.target_height=number(at+12);return result;
    }
    throw std::runtime_error("STG anchored camera has no default settings");
}
}
