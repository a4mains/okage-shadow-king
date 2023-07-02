#include "okage/battle_actor.hpp"
#include "okage/battle_hit.hpp"
#include "okage/camera.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void check(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=4,"Battle actor update word extent");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
std::int16_t half(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=2,"Battle actor update halfword extent");
    return std::bit_cast<std::int16_t>(std::uint16_t(b[at]|(unsigned(b[at+1])<<8)));
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned size=4) {
    check(at<=b.size()&&b.size()-at>=size,"Battle actor update write extent");
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
float supported(float value) {
    const auto bits=std::bit_cast<std::uint32_t>(value),exponent=bits&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),
          "Battle actor update nonfinite/subnormal arithmetic is unsupported");
    return value;
}
float scalar(Bytes b,unsigned at) {return supported(std::bit_cast<float>(word(b,at)));}
void scalar(std::span<std::uint8_t> b,unsigned at,float value) {put(b,at,std::bit_cast<std::uint32_t>(value));}
class TowardZero {
public:
    TowardZero() {
        check(std::fegetenv(&saved_)==0,"Cannot save battle actor floating environment");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);throw std::runtime_error("Cannot set battle actor toward-zero rounding");
        }
    }
    ~TowardZero(){std::fesetenv(&saved_);}
    TowardZero(const TowardZero&)=delete;
    TowardZero& operator=(const TowardZero&)=delete;
private:
    std::fenv_t saved_{};
};
enum class Operation {Add,Subtract,Multiply,Divide};
float single(float left,float right,Operation operation) {
    supported(left);supported(right);
    if(operation==Operation::Divide)check(right!=0,"Battle actor zero divisor is unsupported");
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"Cannot clear battle actor arithmetic flags");
    volatile float a=left,b=right,result;
    switch(operation) {
    case Operation::Add:result=a+b;break;
    case Operation::Subtract:result=a-b;break;
    case Operation::Multiply:result=a*b;break;
    case Operation::Divide:result=a/b;break;
    }
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)),
          "Battle actor floating overflow/underflow is unsupported");
    return supported(result);
}
Float4 vector(Bytes b,unsigned at) {
    Float4 value{};for(unsigned i=0;i<4;++i)value[i]=std::bit_cast<float>(word(b,at+i*4));
    return value;
}
void vector(std::span<std::uint8_t> b,unsigned at,const Float4& value) {
    for(unsigned i=0;i<4;++i)scalar(b,at+i*4,value[i]);
}
BattleActor& bound_actor(std::span<const BattleEntityBinding> entities,int index) {
    check(index>=0&&unsigned(index)<entities.size()&&entities[index].actor,
          "Battle action requires a live target actor binding");
    return *entities[index].actor;
}
std::int8_t status(const BattleActor& actor,unsigned binding) {
    check(!actor.stat_bindings[binding].empty(),"Battle actor live status binding is missing");
    return std::bit_cast<std::int8_t>(actor.stat_bindings[binding][0]);
}
FieldActor& model(BattleActor& actor) {
    check(bool(actor.model),"Battle actor model owner is missing");return *actor.model;
}
void step_model(BattleActor& actor) {
    //1547b8 takes B=C+40. B+d8 points to C+2c; B+dc is C+11c.
    const float timestep=scalar(actor.storage,0x2c);
    if(timestep>1.0f) {
        put(actor.storage,0x11c,0);
        auto& instance=model(actor);
        //1bab08(model,2): visibility gates BOTH channel advancement and bones.
        for(unsigned i=0;i<2;++i)if(instance.visible) {
            instance.tick_channels();
            if(instance.has_motion())instance.evaluate_bones(); // Original flag200.
        }
    } else {
        const float accumulated=single(scalar(actor.storage,0x11c),timestep,Operation::Add);
        scalar(actor.storage,0x11c,accumulated);
        if(accumulated>=1.0f) {
            put(actor.storage,0x11c,0);
            model(actor).tick_channels();
        }
    }
}
void party_status_and_motion(BattleActor& actor,BattleState& root,const BattleHitServices* services) {
    const auto entity=[&]() {
        const int index=half(actor.storage,0x130);
        check(index>=0&&unsigned(index)<battle_entity_count,"Battle party entity outside native root");
        return root.entity(unsigned(index));
    };
    const auto slot=[](Bytes e) {
        const int cursor=half(e,0x172);
        check(cursor>=0&&cursor<16,"Battle party event queue cursor outside storage");
        return 0x174+unsigned(cursor)*8;
    };
    if(half(entity(),0x172)!=0) {
        const auto category=half(entity(),slot(entity()));
        const auto amount=half(entity(),slot(entity())+2);
        check(services!=nullptr,"Battle party queued hit services missing");
        const int result=battle_apply_hit(actor,category,amount,*services);
        if(result==6) {
            //153b88..153bb8: retain graph/status writes before callback installation.
            const int index=half(actor.storage,0x130);
            check(index>=0&&unsigned(index)<battle_entity_count,"Battle party post-hit entity outside native root");
            battle_clear_action(root,unsigned(index));
            check(!actor.stat_bindings[1].empty(),"Battle party primary binding missing");
            actor.stat_bindings[1][0]=6;
            put(actor.stat_bindings[2],0,0,2);
            //153550(TASK+14c) clears the three live secondary bindings in order.
            put(actor.stat_bindings[3],0,0,1);put(actor.stat_bindings[4],0,0,2);
            put(actor.stat_bindings[5],0,0,2);
            put(actor.storage,0x21c,99);put(actor.storage,0x30,0);
            throw std::runtime_error("Unrecovered party lethal callback154120 installation");
        }
        if(result==3||result==4) {
            put(actor.storage,0x142,200,2);put(actor.storage,0x21c,99);
            put(actor.storage,0x194,0);put(actor.storage,0x19c,0);
        } else if(result==10) {
            if(status(actor,3)==4) {
                const auto next=std::uint16_t(std::uint16_t(half(actor.stat_bindings[4],0))+1);
                put(actor.stat_bindings[4],0,next,2);
                if(std::bit_cast<std::int16_t>(next)>=1000)put(actor.stat_bindings[4],0,999,2);
                const auto remaining=std::uint16_t(std::uint16_t(half(actor.record,0x1c))-
                                                  std::uint16_t(half(actor.stat_bindings[4],0)));
                put(actor.storage,0x168,remaining,2);
                if(std::bit_cast<std::int16_t>(remaining)<=0)put(actor.storage,0x168,0,2);
            }
            put(actor.storage,0x3bc,std::uint16_t(half(actor.storage,0x3bc))+1,2);
            //1596e0 reloads the live entity and queue cursor after hit services.
            const auto e=entity();
            put(actor.storage,0x148,half(e,slot(e)+4),2);
        }
        //159508 reloads the cursor at each store; the fourth halfword survives.
        auto e=entity();
        put(e,slot(e),0xffff,2);put(e,slot(e)+2,0,2);put(e,slot(e)+4,0,2);
        const auto next=std::uint16_t(half(e,0x172)-1);
        put(e,0x172,next,2);
        if(std::bit_cast<std::int16_t>(next)<0)put(e,0x172,0,2);
    }
    const float remaining=scalar(actor.storage,0x12c);
    if(remaining>=0.0f) {
        const float next=single(remaining,scalar(actor.storage,0x2c),Operation::Subtract);
        scalar(actor.storage,0x12c,next);
        if(next<0.0f) {
            check(word(actor.storage,0x344)==0xffffffffu,"Unrecovered party motion expiry effect stop101648");
            unsigned motion=14;
            if(word(actor.storage,0x184)!=1) {
                const auto primary=status(actor,1);motion=primary>=2&&primary<=5?5u:1u;
            }
            put(actor.storage,0x128,motion);model(actor).select_motion(motion);
            scalar(actor.storage,0x12c,-1.f);
        }
    }
}
void enemy_status_and_motion(BattleActor& actor,BattleState& root,const BattleHitServices* services) {
    int index=half(actor.storage,0x130);
    check(index>=0&&unsigned(index)<battle_entity_count,"Battle enemy entity outside native root");
    auto entity=root.entity(unsigned(index));
    const auto slot=[&]() {
        const int cursor=half(entity,0x172);
        check(cursor>=0&&cursor<16,"Battle enemy event queue cursor outside storage");
        return 0x174+unsigned(cursor)*8;
    };
    if(half(entity,0x172)!=0) {
        const auto category=half(entity,slot()),amount=half(entity,slot()+2);
        check(services!=nullptr,"Battle enemy queued hit services missing");
        const int result=battle_apply_hit(actor,category,amount,*services);
        index=half(actor.storage,0x130);
        check(index>=0&&unsigned(index)<battle_entity_count,"Battle enemy post-hit entity outside native root");
        entity=root.entity(unsigned(index));
        if(result==6||result==9) {
            if(result==9)put(actor.storage,0x178,0xffff,2);
            battle_clear_action(root,unsigned(index));
            put(entity,0x1fc,word(entity,0x1fc)|0x80000000u);
            check(!actor.stat_bindings[1].empty(),"Battle enemy primary binding missing");
            actor.stat_bindings[1][0]=6;
            put(actor.storage,0x21c,0xffffffff);put(actor.storage,0x30,0);
            const auto previous=actor.first_callback;
            actor.first_callback=BattleActor::FirstCallback::EnemyDeath;put(actor.storage,0,0);
            actor.saved_first_callback=previous;put(actor.storage,0x28,0);
            actor.dead_callback=true;put(actor.storage,0x24,0);
            //159590 clears only slots cursor..0, preserving each fourth halfword.
            for(int cursor=half(entity,0x172);cursor>=0;--cursor) {
                check(cursor<16,"Battle enemy reset queue cursor outside storage");
                const auto at=0x174+unsigned(cursor)*8;
                put(entity,at,0xffff,2);put(entity,at+2,0,2);put(entity,at+4,0,2);
            }
            put(entity,0x172,0,2);return;
        }
        if(result==3||result==4) {
            put(actor.storage,0x194,0);put(actor.storage,0x21c,99);put(actor.storage,0x19c,0);
        }
        //159508 reloads the mutable cursor at each source store.
        put(entity,slot(),0xffff,2);put(entity,slot()+2,0,2);put(entity,slot()+4,0,2);
        const auto next=std::uint16_t(half(entity,0x172)-1);
        put(entity,0x172,next,2);
        if(std::bit_cast<std::int16_t>(next)<0)put(entity,0x172,0,2);
    }
    const float remaining=scalar(actor.storage,0x12c);
    if(remaining>=0) {
        const float next=single(remaining,scalar(actor.storage,0x2c),Operation::Subtract);
        scalar(actor.storage,0x12c,next);
        if(next<0) {
            const auto primary=status(actor,1);
            const auto motion=primary>1&&primary<6?5u:1u;
            put(actor.storage,0x128,motion);model(actor).select_motion(motion);
            scalar(actor.storage,0x12c,-1.f);
        }
    }
}
void primary_effect(BattleActor& actor) {
    //1548e0: status1 or outside1..5 has no work unless its effect is active.
    const auto primary=status(actor,1);
    check(!(primary>1&&primary<6)&&word(actor.storage,0x310)!=1,
          "Unrecovered battle actor primary status effect");
}
void select_attack_callback(BattleActor& actor,std::span<const BattleEntityBinding> entities) {
    const int target=std::bit_cast<std::int8_t>(actor.storage[0x132]);
    auto& recipient=bound_actor(entities,target);
    check(bool(actor.action_motion),"Battle attack has no motion descriptor owner");
    check(word(actor.action_motion->bytes,4)==0,"Unrecovered battle ranged callback1567c8");
    const bool enemy_kind=half(actor.storage,0x40)==1;
    check(!enemy_kind||half(actor.storage,0x44)!=207,"Unrecovered special enemy callback157188");
    const bool ballistic=enemy_kind||half(recipient.storage,0x48)!=0;
    put(actor.storage,0x24,0);
    actor.saved_first_callback=actor.first_callback; //TASK28 is a normalized pointer lane.
    put(actor.storage,0x28,0);
    actor.first_callback=ballistic?BattleActor::FirstCallback::BallisticAttack:BattleActor::FirstCallback::ApproachAttack;
    put(actor.storage,0,0);
    put(actor.storage,0x30,std::uint32_t(std::int32_t(half(actor.storage,0x130))));
}
struct ApproachDirection {Float4 difference;float distance;};
ApproachDirection approach_direction(const Float4& origin,const Float4& target) {
    ApproachDirection result{};Float4 squares{};
    for(unsigned i=0;i<4;++i) {
        result.difference[i]=single(origin[i],target[i],Operation::Subtract);
        squares[i]=single(result.difference[i],result.difference[i],Operation::Multiply);
    }
    volatile float sum=single(squares[0],squares[2],Operation::Add);
    result.distance=supported(std::sqrt(sum));
    return result;
}
void approach_angles(BattleActor& actor,unsigned at,const ApproachDirection& direction) {
    static const AtanTable angles=make_atan_table();
    scalar(actor.storage,at,camera_angle(-direction.difference[1],direction.distance,angles));
    scalar(actor.storage,at+4,camera_angle(direction.difference[0],direction.difference[2],angles));
    put(actor.storage,at+8,0); //W survives.
}
bool approach_target(BattleActor& actor,std::span<const BattleEntityBinding> entities) {
    auto& animation=model(actor).animation();
    if(word(actor.storage,0x300)!=1) {
        auto& target=bound_actor(entities,std::bit_cast<std::int8_t>(actor.storage[0x132]));
        vector(actor.storage,0xf0,model(target).animation().value(1));
        const auto origin=vector(actor.storage,0x60);
        const auto direction=approach_direction(origin,vector(actor.storage,0xf0));
        approach_angles(actor,0x90,direction);
        const float distance=single(direction.distance,12.f,Operation::Subtract); //No minimum clamp.
        vector(actor.storage,0xf0,origin);
        static const SineTable sine=make_sine_table();
        const auto angle=static_cast<std::uint32_t>(single(scalar(actor.storage,0x94),32768.f,Operation::Multiply));
        const auto index=(angle>>1)&0x3fff;
        scalar(actor.storage,0xf0,single(origin[0],single(sine[index],distance,Operation::Multiply),Operation::Subtract));
        scalar(actor.storage,0xf8,single(origin[2],single(sine[(index+4096)&0x3fff],distance,Operation::Multiply),Operation::Subtract));
        const auto duration=single(48.f,scalar(actor.storage,0x2c),Operation::Divide);
        check(double(duration)>=double(INT32_MIN)&&double(duration)<=double(INT32_MAX),
              "Battle approach duration outside signed32 scope");
        animation.move_channel(1,vector(actor.storage,0xf0),static_cast<std::int32_t>(duration));
        put(actor.storage,0x128,8);
        model(actor).select_motion(8);
        scalar(actor.storage,0x12c,48.f);
        put(actor.storage,0x300,word(actor.storage,0x300)+1);
        return false;
    }
    const bool done=!animation.moving(1,0);
    if(done)put(actor.storage,0x300,0);
    vector(actor.storage,0x100,animation.value(0));
    approach_angles(actor,0x100,approach_direction(vector(actor.storage,0x60),vector(actor.storage,0xf0)));
    animation.set_channel(0,vector(actor.storage,0x100));
    return done;
}
const BattleActionDescriptor& strike_descriptor(const BattleActor& actor) {
    check(bool(actor.action_motion),"Battle strike has no motion descriptor");
    return *actor.action_motion;
}
bool schedule_ready(const BattleActor& actor,unsigned counter,unsigned limit,unsigned time) {
    const auto index=std::bit_cast<std::int32_t>(word(actor.storage,counter));
    if(index>=int(limit))return false;
    if(!(scalar(actor.storage,time)<=scalar(actor.storage,0x1f8)))return false;
    check(index>=0,"Battle strike negative schedule index is unsupported");
    return true;
}
void advance_schedule(BattleActor& actor,unsigned counter,unsigned limit,unsigned time,unsigned base,unsigned stride) {
    const auto next=word(actor.storage,counter)+1;
    put(actor.storage,counter,next);
    const auto index=std::bit_cast<std::int32_t>(next);
    if(index<int(limit)) {
        check(index>=0,"Battle strike wrapped negative schedule index");
        const auto value=strike_descriptor(actor).half(base+unsigned(index)*stride);
        scalar(actor.storage,time,float(value));
        if(value<0)put(actor.storage,counter,limit);
    }
}
bool target_active(const BattleState& root,int target) {
    check(target>=0&&unsigned(target)<word(root.bytes,0x50)&&unsigned(target)<battle_entity_count,
          "Battle strike target outside registered entities");
    return std::bit_cast<std::int32_t>(word(root.bytes,0x25c+unsigned(target)*0x200))>=0;
}
int scheduled_target(const BattleActor& actor,unsigned index) {
    check(index<14,"Battle strike target list exceeds embedded descriptor");
    return std::bit_cast<std::int8_t>(actor.storage[0x132+index]);
}
void strike_damage(BattleActor& actor,const BattleState& root,const BattleStrikeServices& services) {
    if(!schedule_ready(actor,0x1e4,4,0x1e0))return;
    for(unsigned i=0;int(i)<half(actor.storage,0x140);++i) {
        const int target=scheduled_target(actor,i);
        if(!target_active(root,target))continue;
        const auto& descriptor=strike_descriptor(actor);
        check(word(descriptor.bytes,4)==0,"Unrecovered ranged strike156920");
        check(bool(services.damage_event),"Battle strike damage158ca8 service missing");
        const auto index=std::bit_cast<std::int32_t>(word(actor.storage,0x1e4));
        check(index>=0&&index<4,"Battle strike damage schedule index");
        services.damage_event(descriptor.half(0xa+unsigned(index)*4),half(actor.storage,0x130),target);
    }
    advance_schedule(actor,0x1e4,4,0x1e0,8,4);
}
void strike_sound(BattleActor& actor,const BattleStrikeServices& services) {
    if(!schedule_ready(actor,0x1ec,8,0x1e8))return;
    check(bool(services.sound),"Battle strike sound12df80 service missing");
    services.sound(strike_descriptor(actor).half(0x18+word(actor.storage,0x1ec)*4));
    advance_schedule(actor,0x1ec,8,0x1e8,0x1a,4);
}
void strike_effect(BattleActor& actor,const BattleState& root,std::span<const BattleEntityBinding> entities,
                   const BattleStrikeServices& services) {
    if(!schedule_ready(actor,0x1f4,4,0x1f0))return;
    const auto entry=[&]() {
        const auto index=std::bit_cast<std::int32_t>(word(actor.storage,0x1f4));
        check(index>=0&&index<4,"Battle strike effect schedule index");
        return Bytes(strike_descriptor(actor).bytes).subspan(0x38+unsigned(index)*20,20);
    };
    const auto create=[&](int target,bool absolute) {
        const auto data=entry();
        Float4 point=absolute?Float4{0,0,0,1}:model(bound_actor(entities,target)).animation().value(1);
        for(unsigned i=0;i<3;++i)point[i]=absolute?scalar(data,8+i*4):single(point[i],scalar(data,8+i*4),Operation::Add);
        check(bool(services.positioned_effect),"Battle strike effect1012f8 service missing");
        const unsigned handle=services.positioned_effect(half(data,0),point);
        put(actor.storage,0x30c,handle);
    };
    const auto type=word(entry(),4);
    switch(type) {
    case 0:create(0,true);break;
    case 1:create(half(actor.storage,0x130),false);break;
    case 5:
        check(bool(services.sound),"Battle strike auxiliary sound12df80 service missing");
        services.sound(39);
        [[fallthrough]];
    case 2:
        for(unsigned i=0;int(i)<half(actor.storage,0x140);++i) {
            const auto target=scheduled_target(actor,i);
            if(target_active(root,target))create(target,false);
        }
        break;
    case 3:
        create(half(actor.storage,0x130),false);
        throw std::runtime_error("Unrecovered strike effect scale101758");
    case 4:
        for(unsigned i=0;int(i)<half(actor.storage,0x140);++i) {
            const auto target=scheduled_target(actor,i);
            if(target_active(root,target)) {
                create(target,false);
                throw std::runtime_error("Unrecovered strike effect scale101758");
            }
        }
        break;
    default:break;
    }
    advance_schedule(actor,0x1f4,4,0x1f0,0x3a,20);
}
bool strike_update(BattleActor& actor,const BattleState& root,std::span<const BattleEntityBinding> entities,
                   const BattleStrikeServices& services) {
    if(word(actor.storage,0x300)!=1) {
        const auto selected=word(actor.storage,0x21c)==5||half(actor.storage,0x1fc)==206?3u:7u;
        const float duration=scalar(strike_descriptor(actor).bytes,0);
        put(actor.storage,0x128,selected);model(actor).select_motion(selected);
        scalar(actor.storage,0x12c,duration);put(actor.storage,0x300,word(actor.storage,0x300)+1);
    }
    const auto remaining=single(scalar(actor.storage,0x12c),scalar(actor.storage,0x2c),Operation::Subtract);
    scalar(actor.storage,0x12c,remaining);
    if(remaining<0) {put(actor.storage,0x300,0);return true;}
    const auto elapsed=single(scalar(actor.storage,0x1f8),scalar(actor.storage,0x2c),Operation::Add);
    scalar(actor.storage,0x1f8,elapsed);
    if(scalar(strike_descriptor(actor).bytes,0)<elapsed) {put(actor.storage,0x300,0);return true;}
    strike_damage(actor,root,services);strike_sound(actor,services);strike_effect(actor,root,entities,services);
    return false;
}
bool return_to_baseline(BattleActor& actor) {
    auto& animation=model(actor).animation();
    const auto duration=[&]() {
        const float value=single(24.f,scalar(actor.storage,0x2c),Operation::Divide);
        check(double(value)>=double(INT32_MIN)&&double(value)<=double(INT32_MAX),"Battle return duration outside signed32 scope");
        return static_cast<std::int32_t>(value);
    };
    if(word(actor.storage,0x300)!=1) {
        animation.move_channel(1,vector(actor.storage,0x60),duration());
        vector(actor.storage,0x90,{0,.25f,0,0});
        animation.move_channel(0,vector(actor.storage,0x90),duration());
        put(actor.storage,0x128,8);model(actor).select_motion(8);
        scalar(actor.storage,0x12c,24.f);put(actor.storage,0x300,word(actor.storage,0x300)+1);
        return false;
    }
    if(animation.moving(1,0))return false;
    const auto primary=status(actor,1);const auto motion=primary>1&&primary<6?5u:1u;
    put(actor.storage,0x128,motion);model(actor).select_motion(motion);scalar(actor.storage,0x12c,-1.f);
    put(actor.storage,0x21c,99);
    for(int i=9;i>=0;--i)actor.storage[0x132+unsigned(i)]=0xff;
    for(unsigned at:{0x146u,0x140u,0x142u,0x144u})put(actor.storage,at,0,2);
    put(actor.storage,0x300,0);return true;
}
//155fb0: reciprocal then vector multiplication, not component division.
void ballistic_velocity(BattleActor& actor,const Float4& displacement,float gravity,float duration) {
    scalar(actor.storage,0x84,gravity);put(actor.storage,0x80,0);put(actor.storage,0x88,0);
    const float reciprocal=single(1.f,duration,Operation::Divide);
    Float4 velocity{};
    for(unsigned i=0;i<4;++i)velocity[i]=single(displacement[i],reciprocal,Operation::Multiply);
    vector(actor.storage,0x70,velocity);
    const float dt=scalar(actor.storage,0x2c);
    float adjustment=single(duration,dt,Operation::Subtract);
    adjustment=single(adjustment,gravity,Operation::Multiply);
    adjustment=single(adjustment,.5f,Operation::Multiply);
    scalar(actor.storage,0x74,single(velocity[1],adjustment,Operation::Subtract));
    velocity=vector(actor.storage,0x70);
    for(auto& value:velocity)value=single(value,dt,Operation::Multiply);
    vector(actor.storage,0x70,velocity);
    auto acceleration=vector(actor.storage,0x80);
    const float dt2=single(dt,dt,Operation::Multiply);
    for(auto& value:acceleration)value=single(value,dt2,Operation::Multiply);
    vector(actor.storage,0x80,acceleration); //Source preserves/scales preexisting W.
}
int motion_duration(BattleActor& actor,float duration) {
    const float frames=single(duration,scalar(actor.storage,0x2c),Operation::Divide);
    check(double(frames)>=double(INT32_MIN)&&double(frames)<=double(INT32_MAX),
          "Battle ballistic duration outside signed32 scope");
    return static_cast<std::int32_t>(frames);
}
void select_timed_motion(BattleActor& actor,unsigned motion,float duration) {
    put(actor.storage,0x128,motion);model(actor).select_motion(motion);scalar(actor.storage,0x12c,duration);
}
void add_ballistic_step(BattleActor& actor) {
    auto position=model(actor).animation().value(1);vector(actor.storage,0xf0,position);
    auto velocity=vector(actor.storage,0x70);const auto acceleration=vector(actor.storage,0x80);
    for(unsigned i=0;i<4;++i)position[i]=single(position[i],velocity[i],Operation::Add);
    vector(actor.storage,0xf0,position);
    for(unsigned i=0;i<4;++i)velocity[i]=single(velocity[i],acceleration[i],Operation::Add);
    vector(actor.storage,0x70,velocity);
}
void ballistic_target(BattleActor& actor,const BattleState& root,
                      std::span<const BattleEntityBinding> entities,const BattleStrikeServices& services) {
    const int id=half(actor.storage,0x130);
    check(id>=0&&id<64,"Battle ballistic owner outside root");
    const bool diagonal=(root.bytes[0x60+unsigned(id)*0x201]&1)!=0;
    const int target=std::bit_cast<std::int8_t>(actor.storage[0x132]);
    float radius;
    if(diagonal) {
        approach_angles(actor,0x100,approach_direction(vector(actor.storage,0x60),vector(actor.storage,0xf0)));
        radius=scalar(bound_actor(entities,target).storage,0x4c);
    } else {
        const int group=half(root.entity(unsigned(id)),0x66);
        if(group<0)return;
        check(group<3,"Battle ballistic group angle outside authored table");
        approach_angles(actor,0x100,approach_direction(vector(actor.storage,0x60),vector(actor.storage,0xf0)));
        //Pinned ELF1fcf60 three authored group angles.
        constexpr std::array<std::uint32_t,3> angles{0xbe800000,0xbdcccccd,0xbecccccd};
        float yaw=std::bit_cast<float>(angles[unsigned(group)]);
        if(target==1)yaw=single(yaw,.125f,Operation::Add);
        else if(target==2)yaw=single(yaw,.125f,Operation::Subtract);
        scalar(actor.storage,0x104,yaw);
        model(actor).animation().move_channel(0,vector(actor.storage,0x100),motion_duration(actor,76.f));
        radius=scalar(actor.storage,0x4c);
    }
    const float distance=single(single(radius,.5f,Operation::Multiply),7.5f,Operation::Add);
    static const SineTable sine=make_sine_table();
    const float scaled=single(scalar(actor.storage,0x104),32768.f,Operation::Multiply);
    check(double(scaled)>=double(INT32_MIN)&&double(scaled)<=double(INT32_MAX),"Battle ballistic angle outside signed32 scope");
    const auto angle=std::uint32_t(static_cast<std::int32_t>(scaled));
    const unsigned index=(angle>>1)&0x3fff;
    Float4 offset{single(distance,sine[index],Operation::Multiply),0,
                  single(distance,sine[(index+4096)&0x3fff],Operation::Multiply),0};
    check(bool(services.approach_offset_w),"Unrecovered155c10 stack offset W provenance");
    offset[3]=supported(services.approach_offset_w(actor));
    auto position=vector(actor.storage,0xf0);
    for(unsigned i=0;i<4;++i)position[i]=single(position[i],offset[i],Operation::Add);
    vector(actor.storage,0xf0,position);
}
bool ballistic_approach(BattleActor& actor,const BattleState& root,
                        std::span<const BattleEntityBinding> entities,const BattleStrikeServices& services) {
    //Two16-byte rows243e50..243e70; next bytes are the diagnostic format string.
    constexpr std::array<Float4,2> timing{{{120,96,76,20},{180,120,60,60}}};
    const auto times=[&]() -> const Float4& {
        const int row=half(actor.storage,0x4a);
        check(row>=0&&row<2,"Battle ballistic timing row outside authored table");
        return timing[unsigned(row)];
    };
    bool done=false;
    auto phase=word(actor.storage,0x300);
    if(phase==0) {
        select_timed_motion(actor,2,times()[0]);put(actor.storage,0x300,++phase);
    }
    if(phase==1) {
        if(scalar(actor.storage,0x12c)<=times()[1]) {
            vector(actor.storage,0xf0,model(bound_actor(entities,std::bit_cast<std::int8_t>(actor.storage[0x132]))).animation().value(1));
            ballistic_target(actor,root,entities,services);
            Float4 displacement{};const auto target=vector(actor.storage,0xf0),origin=vector(actor.storage,0x60);
            for(unsigned i=0;i<4;++i)displacement[i]=single(target[i],origin[i],Operation::Subtract);
            ballistic_velocity(actor,displacement,std::bit_cast<float>(0x3cf5c28fu),times()[2]);
            put(actor.storage,0x300,2);
        }
    } else if(phase==2) {
        add_ballistic_step(actor);
        if(scalar(actor.storage,0x12c)<=times()[3]) {put(actor.storage,0xf4,0);put(actor.storage,0x300,3);}
        model(actor).animation().set_channel(1,vector(actor.storage,0xf0));
    } else if(phase==3&&scalar(actor.storage,0x12c)<=0) {put(actor.storage,0x300,0);done=true;}
    scalar(actor.storage,0x12c,single(scalar(actor.storage,0x12c),scalar(actor.storage,0x2c),Operation::Subtract));
    return done;
}
bool ballistic_return(BattleActor& actor) {
    const auto phase=word(actor.storage,0x300);
    if(phase==0) {
        approach_angles(actor,0x100,approach_direction(vector(actor.storage,0xf0),vector(actor.storage,0x60)));
        //156130 uses baseline rotation+50, not the freshly written+c0.
        model(actor).animation().move_channel(0,vector(actor.storage,0x90),motion_duration(actor,40.f));
        Float4 displacement{};const auto origin=vector(actor.storage,0x60),position=vector(actor.storage,0xf0);
        for(unsigned i=0;i<4;++i)displacement[i]=single(origin[i],position[i],Operation::Subtract);
        ballistic_velocity(actor,displacement,std::bit_cast<float>(0x3d4ccccdu),40.f);
        select_timed_motion(actor,2,40.f);put(actor.storage,0x300,1);return false;
    }
    if(phase!=1)return false;
    add_ballistic_step(actor);
    const float remaining=single(scalar(actor.storage,0x12c),scalar(actor.storage,0x2c),Operation::Subtract);
    scalar(actor.storage,0x12c,remaining);
    if(remaining<=0) {
        put(actor.storage,0x300,0);const auto primary=status(actor,1);
        select_timed_motion(actor,primary>1&&primary<6?5u:1u,-1.f);
        put(actor.storage,0x21c,99);
        for(int i=9;i>=0;--i)actor.storage[0x132+unsigned(i)]=0xff;
        for(unsigned at:{0x146u,0x140u,0x142u,0x144u})put(actor.storage,at,0,2);
        model(actor).animation().set_channel(1,vector(actor.storage,0x60));return true;
    }
    model(actor).animation().set_channel(1,vector(actor.storage,0xf0));return false;
}
void ballistic_attack_update(BattleActor& task,const BattleState& root,std::span<const BattleEntityBinding> entities,
                             const BattleStrikeServices& services) {
    auto& actor=bound_actor(entities,std::bit_cast<std::int32_t>(word(task.storage,0x30)));
    put(task.storage,0x20,1);
    switch(word(task.storage,0x24)) {
    case 0:put(actor.storage,0x300,0);put(task.storage,0x24,1);[[fallthrough]];
    case 1:
        if(ballistic_approach(actor,root,entities,services)) {put(actor.storage,0x300,0);put(task.storage,0x24,word(task.storage,0x24)+1);}
        break;
    case 2:
        if(strike_update(actor,root,entities,services)) {put(actor.storage,0x300,0);put(task.storage,0x24,word(task.storage,0x24)+1);}
        break;
    case 3:
        if(ballistic_return(actor)) {
            put(task.storage,0x34,0);
            check(task.saved_first_callback.has_value(),"Battle ballistic return lacks saved callback");
            task.first_callback=*task.saved_first_callback;put(task.storage,0,0);put(task.storage,0x24,0);
        }
        break;
    default:break;
    }
    step_model(actor);
}
void enemy_death_update(BattleActor& actor,const BattleStrikeServices& services,const BattleHitServices* hits) {
    put(actor.storage,0x20,3);
    switch(word(actor.storage,0x24)) {
    case 0: {
        auto& instance=model(actor);
        instance.render_flags|=0x100000;instance.shadow_mode_override=0xffff;
        scalar(actor.storage,0xe8,.25f);scalar(actor.storage,0xec,1.f);
        scalar(actor.storage,0xe0,.25f);scalar(actor.storage,0xe4,.25f);
        instance.animation().set_channel(3,vector(actor.storage,0xe0));
        scalar(actor.storage,0xec,0.f);
        const float duration=single(60.f,scalar(actor.storage,0x2c),Operation::Divide);
        check(double(duration)>=double(INT32_MIN)&&double(duration)<=double(INT32_MAX),
              "Battle death duration outside signed32 scope");
        instance.animation().move_channel(3,vector(actor.storage,0xe0),static_cast<std::int32_t>(duration));
        put(actor.storage,0x24,word(actor.storage,0x24)+1);
        break;
    }
    case 1:
        if(!model(actor).animation().moving(3,0)) {
            check(hits&&bool(hits->attached_effect),"Battle death attached effect service missing");
            hits->attached_effect(20,actor);
            check(bool(services.sound),"Battle death sound service missing");services.sound(66);
            model(actor).visible=false;
            put(actor.storage,0x24,word(actor.storage,0x24)+1);scalar(actor.storage,0x3c4,30.f);
        }
        break;
    case 2: {
        const float remaining=single(scalar(actor.storage,0x3c4),scalar(actor.storage,0x2c),Operation::Subtract);
        scalar(actor.storage,0x3c4,remaining);
        if(remaining<0) {
            check(bool(services.enemy_reward),"Battle death reward service missing");
            services.enemy_reward(actor);
            check(bool(services.sound),"Battle death reward sound service missing");
            services.sound(65);put(actor.storage,0x24,99);
        }
        break;
    }
    default:break;
    }
    step_model(actor);
}
void approach_attack_update(BattleActor& task,const BattleState& root,std::span<const BattleEntityBinding> entities,
                            const BattleStrikeServices& services) {
    auto& actor=bound_actor(entities,std::bit_cast<std::int32_t>(word(task.storage,0x30)));
    put(task.storage,0x20,1);
    switch(word(task.storage,0x24)) {
    case 0:
        put(actor.storage,0x300,0);put(task.storage,0x20,1);put(task.storage,0x24,1);
        [[fallthrough]];
    case 1:
        if(approach_target(actor,entities)) {
            put(actor.storage,0x300,0);put(task.storage,0x24,word(task.storage,0x24)+1);
        }
        break;
    case 2:
        if(strike_update(actor,root,entities,services)) {
            put(actor.storage,0x300,0);put(task.storage,0x24,word(task.storage,0x24)+1);
        }
        break;
    case 3:
        if(return_to_baseline(actor)) {
            put(task.storage,0x34,0);
            check(task.saved_first_callback.has_value(),"Battle return lacks saved first callback");
            task.first_callback=*task.saved_first_callback;put(task.storage,0,0);
            put(task.storage,0x24,0);
        }
        break;
    default:break; //156300/156318 go directly to model step.
    }
    step_model(actor);
}
}
void battle_actor_motion_update(BattleActor& actor,std::span<const BattleEntityBinding> entities) {
    TowardZero rounding;
    put(actor.storage,0x20,0);
    switch(word(actor.storage,0x21c)) { //B+1dc, not C+1dc.
    case 0:case 2:case 99:case 0xffffffffu:
        break;
    case 1: {
        const float step=single(scalar(actor.storage,0x2c),scalar(actor.storage,0x1a4),Operation::Multiply);
        const float next=single(scalar(actor.storage,0x194),step,Operation::Add);
        const float maximum=scalar(actor.storage,0x198);
        scalar(actor.storage,0x194,next);
        if(maximum<=next)scalar(actor.storage,0x194,maximum);
        break;
    }
    case 3:case 4: {
        const float current=scalar(actor.storage,0x19c);
        const float timestep=scalar(actor.storage,0x2c);
        const float maximum=scalar(actor.storage,0x1a0);
        const float next=single(current,timestep,Operation::Add);
        scalar(actor.storage,0x19c,next);
        if(maximum<=next)scalar(actor.storage,0x19c,maximum);
        step_model(actor);
        break;
    }
    case 5:case 6:select_attack_callback(actor,entities);break;
    default:throw std::runtime_error("Unrecovered battle actor motion/action state");
    }
    auto& animation=model(actor).animation();
    animation.set_channel(0,vector(actor.storage,0x90));
    animation.set_channel(1,vector(actor.storage,0x60));
    animation.set_channel(2,vector(actor.storage,0xc0));
}
void battle_party_first_update(BattleActor& actor,BattleState& root,std::span<const BattleEntityBinding> entities,
                               const BattleHitServices* hits) {
    check(!actor.enemy&&!actor.dead_callback,"Unrecovered party first callback identity");
    TowardZero rounding;
    if(half(actor.storage,0x42)!=3)party_status_and_motion(actor,root,hits);
    battle_actor_motion_update(actor,entities);
}
void battle_actor_first_update(BattleActor& actor,BattleState& root,std::span<const BattleEntityBinding> entities,
                               const BattleStrikeServices& services,const BattleHitServices* hits) {
    TowardZero rounding;
    switch(actor.first_callback) {
    case BattleActor::FirstCallback::Roster:
        if(actor.enemy)battle_enemy_first_update(actor,root,entities,hits);else battle_party_first_update(actor,root,entities,hits);
        break;
    case BattleActor::FirstCallback::ApproachAttack:approach_attack_update(actor,root,entities,services);break;
    case BattleActor::FirstCallback::BallisticAttack:ballistic_attack_update(actor,root,entities,services);break;
    case BattleActor::FirstCallback::EnemyDeath:enemy_death_update(actor,services,hits);break;
    }
}
void battle_actor_second_suffix(BattleActor& actor) {
    TowardZero rounding;
    if(word(actor.storage,0x20)==0)step_model(actor);
    primary_effect(actor);
    //154a38 has a different status pointer and a separate effect owner.
    const auto secondary=status(actor,3);
    check(!(secondary>0&&secondary<10)&&word(actor.storage,0x320)!=1,
          "Unrecovered battle actor secondary status effect");
    check(word(actor.storage,0x17c)!=1&&word(actor.storage,0x330)!=1,
          "Unrecovered battle actor auxiliary status effect");
}
void battle_party_second_update(BattleActor& actor,const BattleState& root) {
    check(!actor.enemy,"Party second callback received enemy actor");
    TowardZero rounding;
    const float ratio=single(scalar(actor.storage,0x194),scalar(actor.storage,0x198),Operation::Divide);
    const float percent=single(ratio,100.0f,Operation::Multiply);
    check(double(percent)>=double(std::numeric_limits<std::int32_t>::min())&&
          double(percent)<=double(std::numeric_limits<std::int32_t>::max()),
          "Battle party charge conversion outside signed32 scope");
    const auto charge=static_cast<std::int32_t>(percent); //CVT.W.S, toward zero.
    check(word(root.bytes,0x81b4)!=3,"Unrecovered battle party transition to callback153dc0");
    // No clamp: only preserve an already-full charge if the new value is full.
    if(half(actor.record,0x26)<100||charge<100)put(actor.record,0x26,std::uint32_t(charge),2);
    battle_actor_second_suffix(actor);
}
void battle_enemy_first_update(BattleActor& actor,BattleState& root,std::span<const BattleEntityBinding> entities,
                               const BattleHitServices* hits) {
    check(actor.enemy&&!actor.dead_callback,"Unrecovered enemy first callback identity");
    TowardZero rounding;
    put(actor.storage,0x20,0);
    enemy_status_and_motion(actor,root,hits);
    battle_actor_motion_update(actor,entities); //154b38 writes C+20 a second time.
}
void battle_enemy_second_update(BattleActor& actor,const BattleState& root) {
    check(actor.enemy,"Enemy second callback received party actor");
    TowardZero rounding;
    check(word(root.bytes,0x81b4)!=4,"Unrecovered battle enemy transition to callback15c1a8");
    if(word(actor.storage,0x20)==0)step_model(actor);
    primary_effect(actor);
}
}
