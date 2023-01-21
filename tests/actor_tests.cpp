#include "okage/actor.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,unsigned at=__builtin_LINE()) {
    if(!value)throw std::runtime_error("Actor world consumption assertion at line "+std::to_string(at));
}
void put(std::vector<std::uint8_t>& bytes,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=std::uint8_t(value>>(8*i));
}
}
int main(){try {
    std::vector<std::uint8_t> motion(56);
    put(motion,0,0x544d58);put(motion,4,24);put(motion,16,0xffffffff);
    put(motion,24,1);put(motion,32,48);put(motion,48,0x00a00002);
    okage::ModelFile model;model.models.emplace_back().bones.resize(1);
    okage::FieldActor actor(model,motion,{},true,true);
    auto& animation=actor.animation();
    animation.set_channel(3,{10,20,30,40});animation.set_channel(4,{5,6,7,8});actor.evaluate();
    const auto prior=actor.channels();
    animation.set_channel(3,{90,91,92,93});animation.set_channel(4,{50,51,52,53});
    animation.set_channel(0,{0,0,0,1});animation.set_channel(1,{2,3,4,1});animation.set_channel(2,{2,3,4,1});
    const auto pc=animation.pc(),script=animation.script_state();
    actor.evaluate_world();
    require(animation.pc()==pc&&animation.script_state()==script);
    require(actor.channels()[0]==okage::Float4({0,0,0,1}));
    require(actor.channels()[1]==okage::Float4({2,3,4,1}));
    require(actor.channels()[2]==okage::Float4({2,3,4,1}));
    for(unsigned i=3;i<prior.size();++i) {
        require(actor.channels()[i]==prior[i]);require(animation.channel_outputs()[i]==prior[i]);
    }
    const auto sine=okage::make_sine_table();const auto world=actor.world(sine);
    require(world[3]==okage::Float4({2,3,4,1}));require(world[0][0]==2&&world[1][1]==3&&world[2][2]==4);
    require(animation.value(3)==okage::Float4({90,91,92,93}));
    require(animation.value(4)==okage::Float4({50,51,52,53}));
    actor.evaluate_world();require(actor.channels()[3]==prior[3]&&actor.channels()[4]==prior[4]);
    animation.set_channel(0,{0,0,0,1});animation.value(0);
    animation.turn_component(0,0,.25f,4);animation.tick_channels();
    actor.evaluate_world();require(actor.channels()[0][0]==.0625f);
    actor.evaluate_world();require(actor.channels()[0][0]==.0625f); // No second step.
    require(animation.moving(0,0)&&animation.pc()==pc&&animation.script_state()==script);
    animation.tick_channels();actor.evaluate_world();require(actor.channels()[0][0]==.125f);
    // Static constructor's deferred identity is consumed without motion data.
    okage::FieldActor fixed(model);fixed.evaluate_world();
    require(fixed.channels()[0]==okage::Float4({0,0,0,1}));
    require(fixed.channels()[2]==okage::Float4({1,1,1,1}));
    std::cout<<"Actor world consumption: deferred world, preserved bone/color, no script/channel step passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
