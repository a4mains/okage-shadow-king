#include "okage/camera.hpp"
#include "collision_geometry.hpp"
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
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Camera rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
}
AtanTable make_atan_table() {
    Rounding rounding;AtanTable result;
    for(unsigned i=0;i<result.size();++i) {
        float value=std::atan(float(i)*0.000244140625f);
        value=value*4.f;value=value*2048.f;value=value/3.1415927410125732421875f;
        result[i]=std::int16_t(value+0.5f);
    }
    return result;
}
float camera_angle(float y,float x,const AtanTable& table) {
    Rounding rounding;
    if(!std::isfinite(y)||!std::isfinite(x))throw std::runtime_error("Non-finite camera direction");
    const auto ax=std::fabs(x),ay=std::fabs(y);unsigned angle;
    if(ax==ay) {if(ax==0)return 0;angle=0x800;}
    else {
        float ratio=ay<ax ? (ay*4096.f)/ax : (ax*4096.f)/ay;
        if(!std::isfinite(ratio)||ratio<0 || ratio>=4096)throw std::runtime_error("Camera angle table bounds");
        const auto part=table[unsigned(ratio)];angle=ay<ax ? part : 0x1000-part;
    }
    if(y<0)angle=0x4000-angle;
    if(x<0)angle=0x2000-angle;
    return float(angle&0x3fff)*0.00006103515625f;
}
Float4 camera_look_at(const Float4& position,const Float4& target,const AtanTable& angles,const SineTable& sine) {
    Rounding rounding;Float4 d;
    for(unsigned i=0;i<4;++i) {
        if(!std::isfinite(position[i])||!std::isfinite(target[i]))throw std::runtime_error("Non-finite camera point");
        d[i]=position[i]+(-target[i]);
    }
    const auto yaw=camera_angle(-d[0],-d[2],angles);
    const auto index=(std::uint32_t(std::int32_t(-yaw*32768.f))>>1)&0x3fff;
    const float s=sine[index],c=sine[(index+4096)&0x3fff];
    // Original identity then Y rotation: z' = -sin(yaw)*x + cos(yaw)*z.
    float z=(-s)*d[0];z=z+0.f*d[1];z=z+c*d[2];z=z+0.f*d[3];
    return {camera_angle(d[1],-z,angles),yaw,0,1};
}
void CameraBlend::begin(float rate,const Float4& from_position,const Float4& from_target) {
    if(!std::isfinite(rate))throw std::runtime_error("Invalid camera blend rate");
    if(rate!=0) {phase=0;step=rate;position=from_position;target=from_target;}
}
CameraPoints anchored_camera(const Float4& player,const AnchoredCamera& settings,
                             const AtanTable& angles,const SineTable& sine) {
    Rounding rounding;
    for(float v:player)if(!std::isfinite(v))throw std::runtime_error("Non-finite camera player position");
    for(float v:settings.anchor)if(!std::isfinite(v))throw std::runtime_error("Non-finite camera anchor");
    for(float v:{settings.distance_offset,settings.distance_scale,settings.target_height})
        if(!std::isfinite(v))throw std::runtime_error("Non-finite anchored-camera setting");
    CameraPoints result;result.target=player;result.target[1]+=settings.target_height;
    const float dx=result.target[0]-settings.anchor[0],dz=result.target[2]-settings.anchor[2];
    const float distance=std::sqrt(dx*dx+dz*dz);
    const float yaw=camera_angle(dx,dz,angles);
    const auto index=(std::uint32_t(std::int32_t(yaw*32768.f))>>1)&0x3fff;
    const float radius=settings.distance_offset+distance*settings.distance_scale;
    const Float4 offset={sine[index]*radius,0,sine[(index+4096)&0x3fff]*radius,0};
    for(unsigned i=0;i<4;++i)result.position[i]=settings.anchor[i]+offset[i];
    return result;
}
Float4 ExteriorCamera::begin(const Float4& player) {
    Rounding rounding;auto result=player;result[1]-=50;
    turn_rate=.0003f;distance_scale=0.976f;target_height=-6;maximum_height=desired_height;
    height=-2.2f;phase=0;fast_turn=false;preset=0;return result;
}
void ExteriorCamera::configure(float h,float scale,float target) {
    if(!std::isfinite(h)||!std::isfinite(scale)||!std::isfinite(target))throw std::runtime_error("Invalid exterior camera settings");
    height=h;distance_scale=scale;target_height=target;
    desired_height=height;maximum_height=height;saved_scale=distance_scale;phase=0;
}
void ExteriorCamera::select_preset(unsigned value) {
    auto f=[](std::uint32_t value){return __builtin_bit_cast(float, (value));};
    preset=value<=4?value:0;
    if(!preset){target_height=-6;maximum_height=desired_height;saved_scale=distance_scale=f(0x3f79db23);return;}
    if(preset==2)configure(f(0xc232cccd),f(0x3f7fbe66),f(0xc11ccccd));
    else if(preset==3)configure(f(0xc2aa999a),f(0x3f804170),f(0xc151999a));
    else configure(f(0xc1a01449),f(preset==1?0x3f7c2917:0x3f79db23),f(preset==1?0xc11028d4:0xc0a9eb14));
    obstruction_clock=0;
}
CameraPoints ExteriorCamera::tick(const Float4& player,const Float4& facing,const Float4& camera,
                                 unsigned held,unsigned look_x,const CollisionMap& map,
                                 const AtanTable& angles,const SineTable& sine,std::uint32_t disabled_mask) {
    using namespace collision_geometry;Rounding rounding;
    for(const auto& point:{player,facing,camera,previous_player})for(float v:point)
        if(!std::isfinite(v)||std::fabs(v)>1e6f)throw std::runtime_error("Invalid exterior camera transform");
    for(float v:{distance_scale,target_height,height,desired_height,maximum_height,phase,angular_velocity,target_y,target_offset})
        if(!std::isfinite(v)||std::fabs(v)>1e6f)throw std::runtime_error("Invalid exterior camera state");
    if(look_x>255||phase<0||phase>1)throw std::runtime_error("Invalid exterior camera input/phase");
    auto smooth=[](float current,float target) {
        const float delta=target-current;
        if(delta<=-4)return current-.5f;
        if(delta>=4)return current+.5f;
        const float quarter=delta*.25f;const float step=(quarter*quarter)*.5f;
        return delta<0?current-step:current+step;
    };
    turn_rate=fast_turn?.002f:.0003f;
    const auto relative=sub(player,camera);
    float radius=std::sqrt(relative[0]*relative[0]+relative[2]*relative[2]);
    if(radius>75)radius=radius-(radius-75)*.02f;
    else if(radius<65)radius=radius+(65-radius)*.02f;
    radius=radius*distance_scale;
    if(held&4)look_x=0;if(held&8)look_x=255;
    float manual=manual_rotate?(float(look_x)-128.f)*.00005f:0;
    if(double(manual)<.005&&manual>-.005f)manual=0;
    float yaw=(1.f-camera_angle(relative[0],relative[2],angles))+manual;
    const auto movement=sub(player,previous_player);
    const float limit=std::sqrt(dot(movement,movement))*.02f;
    float player_yaw=(1.f-facing[1])+.5f;
    while(player_yaw>=1)player_yaw-=1;
    float difference=player_yaw-yaw;
    if(difference>.5f)difference-=1;if(difference<-.5f)difference+=1;
    angular_velocity=angular_velocity*.8f+difference*turn_rate;
    if(angular_velocity>limit)angular_velocity=limit;
    if(angular_velocity< -limit)angular_velocity=-limit;
    yaw+=angular_velocity;previous_player=player;
    const float scaled=yaw*32768.f;
    if(scaled< -2147483648.f||scaled>=2147483648.f)throw std::runtime_error("Exterior camera angle bounds");
    const auto index=(std::uint32_t(std::int32_t(scaled))>>1)&0x3fff;
    const float vertical=-6.f+height;
    CameraPoints result;result.position=add(player,{radius*sine[index],vertical,(-radius)*sine[(index+4096)&0x3fff],1});
    if(phase!=1) {
        auto delta=sub(result.position,camera);for(auto& v:delta)v*=phase;
        result.position=add(camera,delta);map.camera_move(camera,result.position,disabled_mask);
        phase+=.005f;if(phase>=.99f)phase=1;
    } else map.camera_move(camera,result.position,disabled_mask);
    if(preset==0) {
        const float base=result.position[1]-vertical;
        if(obstruction_clock<0) {
            if(obstruction_clock==INT32_MIN)throw std::runtime_error("Camera obstruction timer overflow");
            --obstruction_clock;height=smooth(height,desired_height);
            const float delta=desired_height-height;
            if(delta<.4f&&delta>-.4f)obstruction_clock=0;
        }
        auto from=player;from[1]+=target_height;
        const float threshold=from[1]-44.7f;
        auto probe=result.position;probe[1]=base+desired_height;
        if(threshold<=desired_height)probe[1]-=1;
        if(map.segment_contact(from,probe,disabled_mask).hit) {
            if(threshold<=desired_height)desired_height-=1;
            obstruction_clock=-1;
        }
        if(obstruction_clock>=0) {
            probe[1]=-5;
            if(map.segment_contact(from,probe,disabled_mask).hit)obstruction_clock=0;
            else {
                if(obstruction_clock==INT32_MAX)throw std::runtime_error("Camera obstruction timer overflow");
                if(++obstruction_clock>=60) {
                    if(desired_height<=maximum_height)desired_height+=.25f;
                    height=smooth(height,desired_height);
                }
            }
        }
    }
    result.target=player;
    if(snap_target){target_reset_clock=10;snap_target=false;target_reset_counter=0;target_y=player[1];}
    else target_y=smooth(target_y,player[1]);
    if(target_height+.15f<target_offset)target_offset-=.1f;
    else if(target_offset<target_height-.15f)target_offset+=.1f;
    result.target[1]=target_y+target_offset;
    const float separation=std::fabs(player[0]-result.position[0])+std::fabs(player[2]-result.position[2]);
    if(separation>=2147483648.f)throw std::runtime_error("Camera distance conversion bounds");
    distance=int(separation);
    // 0x136d00 computes an unused local five-unit offset; only phase changes.
    if(distance<8){phase+=.06f;if(phase>=.99f)phase=1;}
    return result;
}
void CameraBlend::apply(Float4& p,Float4& t) {
    Rounding rounding;
    if(phase==1)return;
    for(unsigned i=0;i<4;++i) {p[i]=position[i]+(p[i]-position[i])*phase;t[i]=target[i]+(t[i]-target[i])*phase;}
    phase=phase+step;if(phase>=0.99f)phase=1;
}
}
