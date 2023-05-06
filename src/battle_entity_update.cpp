#include "okage/battle_actor.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
std::uint32_t word(Bytes b,unsigned at) {
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
        (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
std::uint64_t mask(Bytes b,unsigned at) {
    return std::uint64_t(word(b,at))|(std::uint64_t(word(b,at+4))<<32);
}
void mask(std::span<std::uint8_t> b,unsigned at,std::uint64_t value) {
    for(unsigned i=0;i<8;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
std::uint16_t half(Bytes b,unsigned at){return std::uint16_t(b[at]|(unsigned(b[at+1])<<8));}
void half(std::span<std::uint8_t> b,unsigned at,std::uint16_t value) {
    b[at]=std::uint8_t(value);b[at+1]=std::uint8_t(value>>8);
}
class Rounding {
public:
    Rounding() {
        require(std::fegetenv(&saved_)==0,"Battle entity floating environment unavailable");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);throw std::runtime_error("Battle entity rounding unavailable");
        }
    }
    ~Rounding(){std::fesetenv(&saved_);}
private:
    std::fenv_t saved_{};
};
float supported(float value) {
    const auto bits=std::bit_cast<std::uint32_t>(value),exponent=bits&0x7f800000u;
    require(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),
            "Battle entity nonfinite/subnormal arithmetic is unrecovered");
    return value;
}
enum class Operation {Add,Subtract,Multiply};
float single(float left,float right,Operation operation) {
    supported(left);supported(right);
    require(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"Battle entity arithmetic flags unavailable");
    volatile float a=left,b=right,result;
    switch(operation) {
    case Operation::Add:result=a+b;break;
    case Operation::Subtract:result=a-b;break;
    case Operation::Multiply:result=a*b;break;
    }
    require(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)),
            "Battle entity arithmetic overflow/underflow is unrecovered");
    return supported(result);
}
void refresh_pair(BattleState& root,std::span<const BattleEntityBinding> bindings,unsigned i,unsigned j) {
    auto* a=bindings[i].actor;auto* b=bindings[j].actor;
    if(!a||!b)return;
    const float half_b=single(std::bit_cast<float>(word(b->storage,0x4c)),.5f,Operation::Multiply);
    const float half_a=single(std::bit_cast<float>(word(a->storage,0x4c)),.5f,Operation::Multiply);
    const float radius=single(half_a,half_b,Operation::Add);
    const auto pa=a->model->animation().value(1);
    const float radius_squared=single(radius,radius,Operation::Multiply);
    const auto pb=b->model->animation().value(1);
    //188b28: subtract and square, then (X+Y)+Z. No sqrt and no W term.
    Float4 delta{},square{};
    for(unsigned axis=0;axis<3;++axis)delta[axis]=single(pa[axis],pb[axis],Operation::Subtract);
    for(unsigned axis=0;axis<3;++axis)square[axis]=single(delta[axis],delta[axis],Operation::Multiply);
    const float xy=single(square[0],square[1],Operation::Add);
    const float distance=single(xy,square[2],Operation::Add);
    auto ei=root.entity(i),ej=root.entity(j);
    if(distance<=radius_squared) {
        ei[j]=ej[j];ej[i]=ei[i];
        mask(ei,0x50,mask(ei,0x50)|(std::uint64_t(1)<<j));
    } else {ei[j]=0;ej[i]=0;}
}
}
void battle_register_action(BattleState& root,unsigned owner,unsigned target,
                            std::uint32_t registration_flag) {
    auto node=root.entity(owner);
    (void)root.entity(target);
    half(node,0x60,std::uint16_t(owner));half(node,0x62,std::uint16_t(target));
    for(unsigned i=0;i<4;++i)node[0x68+i]=std::uint8_t(registration_flag>>(8*i));
    half(node,0x64,std::uint16_t(half(node,0x64)+1));
    const auto append=[&](unsigned destination) {
        auto e=root.entity(destination);
        const auto count=half(e,0x16c);
        half(e,0x16c,std::uint16_t(count+1)); // Source stores before indexing.
        const auto index=std::bit_cast<std::int16_t>(count);
        require(index>=0&&index<64,"Battle action append would write outside owned link storage");
        require(word(e,0x6c+unsigned(index)*4)==0,"Battle action link contains an unnormalized guest pointer");
        root.action_links[destination][unsigned(index)]=std::uint8_t(owner);
    };
    append(target);append(owner);
}
unsigned battle_action_readiness(const BattleState& root,unsigned owner,Bytes descriptor) {
    (void)root.entity(owner);
    const auto source_head=root.action_links[owner][0];
    if(!source_head)return 2;
    require(descriptor.size()>=16,"Battle action readiness descriptor extent");
    const int count=std::bit_cast<std::int16_t>(half(descriptor,14));
    require(count<=14,"Battle action readiness target extent");
    bool matched=false;
    for(int i=0;i<count;++i) {
        const int target=std::bit_cast<std::int8_t>(descriptor[unsigned(i)]);
        require(target>=0&&target<64,"Battle action readiness signed target extent");
        const auto target_head=root.action_links[unsigned(target)][0];
        if(!target_head)continue;
        if(std::bit_cast<std::int16_t>(half(root.entity(*source_head),0x60))!=int(owner)||
           std::bit_cast<std::int16_t>(half(root.entity(*target_head),0x60))!=int(owner))return 0;
        matched=true;
    }
    return matched?1:3;
}
void battle_clear_action(BattleState& root,unsigned owner) {
    auto node=root.entity(owner);
    half(node,0x60,0xffff);half(node,0x62,0xffff);
    for(unsigned i=0;i<4;++i)node[0x68+i]=0;
}
void battle_compact_actions(BattleState& root,unsigned entity) {
    auto e=root.entity(entity);auto& links=root.action_links[entity];
    if(!links[0])return;
    for(unsigned pass=0;pass<64;++pass)for(unsigned i=0;i<63;++i) {
        if(!links[i])continue;
        const auto node=root.entity(*links[i]);
        if(std::bit_cast<std::int16_t>(half(node,0x60))>=0)continue;
        for(unsigned j=i;j<63;++j)links[j]=links[j+1];
        links[63].reset();half(e,0x16c,std::uint16_t(half(e,0x16c)-1));
    }
}
void battle_refresh_entities(BattleState& root,std::span<const BattleEntityBinding> bindings) {
    const auto count=word(root.bytes,0x50);
    require(count<=battle_entity_count&&bindings.size()==count,"Battle entity refresh count/binding mismatch");
    for(unsigned i=0;i<count;++i) {
        const auto e=root.entity(i);
        require(word(e,0x1f4)==0&&word(e,0x1f8)==0,"Battle entity contains unnormalized guest links");
        for(unsigned offset=0x6c;offset<=0x168;offset+=4)
            require(word(e,offset)==0,"Battle entity action list contains an unnormalized guest pointer");
        for(const auto link:root.action_links[i])
            require(!link||*link<64,"Battle entity action link owner extent");
        if(const auto* actor=bindings[i].actor) {
            require(actor->entity_id==i&&actor->model,"Battle entity typed actor/model binding mismatch");
        }
    }
    Rounding rounding;
    for(unsigned i=0;i<count;++i) {
        auto e=root.entity(i);const auto old=mask(e,0x50);
        mask(e,0x50,0);mask(e,0x58,old);
        for(unsigned j=0;j<i;++j)refresh_pair(root,bindings,i,j);
        for(unsigned j=i+1;j<count;++j)refresh_pair(root,bindings,i,j);
        const auto current=mask(e,0x50);mask(e,0x58,(old^current)&current);
        battle_compact_actions(root,i);
    }
}
}
