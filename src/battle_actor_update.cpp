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
void initial_status_and_motion(BattleActor& actor,const BattleState& root) {
    const auto index=half(actor.storage,0x130);
    check(index>=0&&unsigned(index)<battle_entity_count,"Battle actor entity index outside native root");
    check(half(root.bytes,0x1d2+unsigned(index)*0x200)==0,
          "Unrecovered battle actor queued status/action");
    const float remaining=scalar(actor.storage,0x12c);
    if(remaining>=0.0f) {
        const float next=single(remaining,scalar(actor.storage,0x2c),Operation::Subtract);
        scalar(actor.storage,0x12c,next);
        check(next>=0.0f,"Unrecovered battle actor motion expiry/effect cleanup");
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
    check(half(actor.storage,0x40)!=1,"Unrecovered enemy battle attack callback155558/157188");
    check(half(recipient.storage,0x48)==0,"Unrecovered battle target-category callback155558");
    put(actor.storage,0x24,0);
    actor.saved_first_callback=actor.first_callback; //TASK28 is a normalized pointer lane.
    put(actor.storage,0x28,0);
    actor.first_callback=BattleActor::FirstCallback::ApproachAttack;
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
    case 5:select_attack_callback(actor,entities);break;
    default:throw std::runtime_error("Unrecovered battle actor motion/action state");
    }
    auto& animation=model(actor).animation();
    animation.set_channel(0,vector(actor.storage,0x90));
    animation.set_channel(1,vector(actor.storage,0x60));
    animation.set_channel(2,vector(actor.storage,0xc0));
}
void battle_party_first_update(BattleActor& actor,const BattleState& root,std::span<const BattleEntityBinding> entities) {
    check(!actor.enemy&&!actor.dead_callback,"Unrecovered party first callback identity");
    TowardZero rounding;
    if(half(actor.storage,0x42)!=3)initial_status_and_motion(actor,root);
    battle_actor_motion_update(actor,entities);
}
void battle_actor_first_update(BattleActor& actor,BattleState& root,std::span<const BattleEntityBinding> entities,
                               const BattleStrikeServices& services,const BattleHitServices* hits) {
    TowardZero rounding;
    switch(actor.first_callback) {
    case BattleActor::FirstCallback::Roster:
        if(actor.enemy)battle_enemy_first_update(actor,root,entities,hits);else battle_party_first_update(actor,root,entities);
        break;
    case BattleActor::FirstCallback::ApproachAttack:approach_attack_update(actor,root,entities,services);break;
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
