#include "okage/field_shadows.hpp"
#include <algorithm>
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
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Field shadow rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
std::uint32_t word(Bytes b,std::size_t p) {
    if(p>b.size()||b.size()-p<4)throw std::runtime_error("Shadow STG bounds");
    return std::uint32_t(b[p])|(std::uint32_t(b[p+1])<<8)|(std::uint32_t(b[p+2])<<16)|(std::uint32_t(b[p+3])<<24);
}
}
unsigned field_depth_bucket(const Matrix4& view,const Float4& position,const ShadowScreenSettings& settings,unsigned depth_shift) {
    if(depth_shift>31)throw std::runtime_error("Unsupported sort depth shift");
    Rounding rounding;
    Float4 p{};
    for(unsigned row=0;row<4;++row) {
        float f=view[0][row]*position[0];f=f+view[1][row]*position[1];f=f+view[2][row]*position[2];
        p[row]=f+view[3][row];
    }
    p[2]=std::max(p[2],settings.clip_near);p[3]=1;
    const Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    const auto cache=shadow_vertex_cache(std::span(&p,1),{identity,identity},shadow_screen_parameters(settings));
    const float z=cache.screen[0][2]*16;
    if(!std::isfinite(z)||z<0||z>=2147483648.f)throw std::runtime_error("Field shadow sort depth overflow");
    return std::min(512u,static_cast<std::uint32_t>(z)>>depth_shift);
}
unsigned field_ordinary_bucket(std::uint32_t flags,unsigned depth,bool special) {
    if(depth>512)throw std::runtime_error("Invalid field sort depth");
    if(special)return 1031; //0x138220 ->0x1bb920(mode4), clears live depth-sort bit.
    unsigned base=3;
    if(!(flags&0x80)){
        base=5;
        if(flags&0x40000)base=513;
        if(flags&0x80000)base=4;
        else if(flags&0x20000)base=3;
    }
    if((flags>>5)&3)base=521;
    return base+((flags&0x100080)?depth:0);
}
FieldShadowSettings field_shadow_settings(Bytes stage) {
    if(word(stage,0)!=0x475453)throw std::runtime_error("Invalid shadow STG");
    const auto start=word(stage,8),count=word(stage,12);
    if(start<32||count>4096||std::uint64_t(start)+std::uint64_t(count)*20>stage.size())
        throw std::runtime_error("Shadow STG records extent");
    Rounding rounding;FieldShadowSettings result;
    for(unsigned i=0;i<count;++i) {
        const auto at=start+i*20,kind=word(stage,at);
        if(kind<0x4a||kind>0x4c)continue;
        Float4 value;
        for(unsigned c=0;c<4;++c) {
            value[c]=__builtin_bit_cast(float, (word(stage,at+4+c*4)));
            if(!std::isfinite(value[c]))throw std::runtime_error("Nonfinite shadow STG value");
        }
        if(kind==0x4a){result.direction=value;result.direction[3]=0;}
        else if(kind==0x4b)for(unsigned c=0;c<3;++c) {
            const float v=value[c]*255;
            if(v<0||v>=256)throw std::runtime_error("Unsupported shadow tint range");
            result.tint[c]=static_cast<std::uint32_t>(v);
        }
        else {
            if(value[0]<0||value[0]>=2147483648.f)throw std::runtime_error("Unsupported shadow threshold");
            result.threshold=static_cast<std::uint32_t>(value[0]);
        }
    }
    return result;
}
FieldShadowActor make_field_shadow_actor(int alias,std::string_view name,const FieldActor& actor,
        const ModelFile* companion,bool height) {
    const auto flags=alias==-3?0xa0:alias==-1?0xc0:(name=="wkumo"||name=="rkumo"||name=="bkumo")?0x40060:0xe0;
    return make_instance_shadow_actor(alias,actor,companion,flags,height);
}
FieldShadowActor make_instance_shadow_actor(int alias,const FieldActor& actor,
        const ModelFile* companion,std::uint32_t flags,bool height) {
    FieldShadowActor result;result.alias=alias;result.actor=&actor;result.flags=flags;
    auto classification=(result.flags>>5)&3;
    if(!companion&&classification!=1){result.flags&=~0x60u;classification=0;}
    if(classification<2)return result;
    if(actor.model().models.size()!=1||companion->models.size()!=1)throw std::runtime_error("Field shadow requires one main/companion model");
    result.selector=(classification-2)|(result.flags&0x400000?2:0)|(height?4:0);
    result.companion=bind_shadow_model(actor.model().models[0],companion->models[0]);
    for(const auto& block:result.companion->blocks)result.bounds.push_back(model_block_bounds(block,true));
    return result;
}
FieldShadowFrame field_shadow_frame(std::span<const FieldShadowActor> actors,const Matrix4& view,
        const FieldProjectionFrame& screen,const FieldShadowSettings& settings,float factor,const SineTable& sine,unsigned depth_shift) {
    const ShadowProjectionContext context(settings.direction,view);
    auto projection=context.base();
    const auto params=shadow_screen_parameters(screen.shadow);
    FieldShadowFrame frame;
    //0x1c96c0 inserts at the head: each selector precedes its geometry and
    // actor pairs execute in reverse construction order within shadow bucket517.
    for(auto it=actors.rbegin();it!=actors.rend();++it) {
        const auto& input=*it;
        if(!input.actor||!input.actor->visible||!input.companion||!input.selector)continue;
        const auto sort=field_depth_bucket(view,input.actor->channels().at(1),screen.ordinary,depth_shift);
        if(sort<=settings.threshold)continue;
        const auto selector=input.actor->shadow_mode_override.value_or(*input.selector);
        //0x1ba244/0x1ba248 loads the selector as signed16 and skips both
        // geometry and selector packets when negative (transition uses0xffff).
        if(std::int16_t(selector)<0)continue;
        const auto next=context.project(selector,input.actor->channels().at(1)[1],factor,settings.tint);
        projection.color=next.color;
        if(next.has_matrices)projection.matrices=next.matrices;
        if(next.direction)projection.direction=next.direction;
        const auto world=input.actor->world(sine);
        const auto view_world=compose_pose(view,std::span(&world,1));
        const auto local=animated_pose(*input.companion,input.actor->channels(),sine,input.actor->animation().scale_channels());
        const auto pose=compose_pose(view_world[0],local);
        const auto selected=visible_model_blocks(input.bounds,pose,screen.ordinary);
        const auto draws=shadow_draw_blocks(*input.companion,pose,projection,params,context.view_direction,selected);
        FieldShadowSubmission submission{input.alias,selector,sort,static_cast<unsigned>(selected.size()),static_cast<unsigned>(draws.size()),0};
        for(const auto& draw:draws)if(draw.prism.emitted){frame.draws.push_back({draw.prism,projection.color});++submission.emitted;}
        frame.submissions.push_back(submission);
    }
    return frame;
}
}
