#include "okage/actor.hpp"
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
void require(bool value,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("Actor world consumption assertion at line "+std::to_string(at.line()));
}
void put(std::vector<std::uint8_t>& bytes,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=std::uint8_t(value>>(8*i));
}

template<class F>void rejects(F action) {
    try{action();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected shared actor texture rejection");
}
okage::ModelFile textured_model() {
    okage::ModelFile model;model.textures={"base0","base1"};
    auto& part=model.models.emplace_back();part.bones.resize(1);
    auto& block=part.blocks.emplace_back();
    for(const std::int16_t index:{0,1,0,-1})block.textures.push_back({index,-1,1,1,{0,0,1,1}});
    return model;
}
okage::TextureAnimation texture_animation() {return {{0,1},{"first","second"}};}
std::vector<std::uint8_t> texture_motion() {
    // Five eight-byte entries: END; group0 variants1/2/0; group1 variant1.
    std::vector<std::uint8_t> motion(72);
    put(motion,0,0x544d58);put(motion,4,40);put(motion,16,0xffffffff);put(motion,24,1);
    put(motion,40,0x134);put(motion,48,0x234);put(motion,56,0x34);put(motion,64,0x100134);
    return motion;
}
void shared_texture_callbacks() {
    const auto model=textured_model();const auto animation=texture_animation();const auto motion=texture_motion();
    auto bindings=std::make_shared<okage::TextureBindings>(model,animation);
    okage::FieldActor first(model,motion,animation,true,true,bindings);
    require(&first.textures()==bindings.get());
    first.select_motion(1);first.tick_script();
    require(first.textures().images()==std::vector<std::int32_t>({2,1,2,-1}));
    okage::FieldActor second(model,motion,animation,true,true,bindings);
    require(&second.textures()==&first.textures());
    require(second.textures().images()==std::vector<std::int32_t>({2,1,2,-1}));
    // 1b6fb8 restores all before selecting the next script. This is visible
    // to the other instance even before the new script reaches opcode34.
    second.select_motion(2);
    require(first.textures().images()==std::vector<std::int32_t>({0,1,0,-1}));
    second.tick_script();
    require(first.textures().images()==std::vector<std::int32_t>({3,1,3,-1}));
    first.animation().start(4);
    require(second.textures().images()==std::vector<std::int32_t>({3,2,3,-1}));
    first.animation().start(3); // Variant0 restores only the registered group.
    require(second.textures().images()==std::vector<std::int32_t>({0,2,0,-1}));
    auto initial=motion;put(initial,32,0x234);
    okage::FieldActor third(model,initial,animation,true,true,bindings);
    require(first.textures().images()==std::vector<std::int32_t>({3,2,3,-1}));
    require(third.textures().images()==second.textures().images());
    first.select_motion(0);
    require(third.textures().images()==std::vector<std::int32_t>({0,1,0,-1}));
    // Script bounds rejection occurs after the source restore-all mutation.
    bindings->select(0,2);rejects([&]{second.select_motion(100);});
    require(first.textures().images()==std::vector<std::int32_t>({0,1,0,-1}));
    require(second.motion()==2);
}
void private_texture_callbacks() {
    const auto model=textured_model();const auto animation=texture_animation();const auto motion=texture_motion();
    okage::FieldActor first(model,motion,animation),second(model,motion,animation);
    require(&first.textures()!=&second.textures());
    first.animation().start(2);
    require(first.textures().images()==std::vector<std::int32_t>({3,1,3,-1}));
    require(second.textures().images()==std::vector<std::int32_t>({0,1,0,-1}));
    second.select_motion(1);second.tick_script();
    require(first.textures().images()==std::vector<std::int32_t>({3,1,3,-1}));
    require(second.textures().images()==std::vector<std::int32_t>({2,1,2,-1}));
}
void shared_texture_validation_and_lifetime() {
    const auto model=textured_model();const auto animation=texture_animation();const auto motion=texture_motion();
    auto bindings=std::make_shared<okage::TextureBindings>(model,animation);bindings->select(0,2);
    const auto selected=bindings->images();
    auto different=model;different.textures[0]="wrong";
    rejects([&]{okage::FieldActor actor(different,motion,animation,true,true,bindings);});
    different=model;different.models[0].blocks[0].textures[1].texture=0;
    rejects([&]{okage::FieldActor actor(different,motion,animation,true,true,bindings);});
    different=model;different.models[0].blocks[0].textures.pop_back();
    rejects([&]{okage::FieldActor actor(different,motion,animation,true,true,bindings);});
    auto wrong=animation;wrong.groups={1,0};
    rejects([&]{okage::FieldActor actor(model,motion,wrong,true,true,bindings);});
    wrong=animation;wrong.variants[1]="wrong";
    rejects([&]{okage::FieldActor actor(model,motion,wrong,true,true,bindings);});
    require(bindings->images()==selected); // Validation copies/restores privately.
    auto invalid=motion;put(invalid,32,0x100234);put(invalid,36,0x700034);
    rejects([&]{okage::FieldActor actor(model,invalid,animation,true,true,bindings);});
    require(bindings->images()==std::vector<std::int32_t>({3,3,3,-1}));
    std::weak_ptr<okage::TextureBindings> weak=bindings;
    auto owner=std::make_unique<okage::FieldActor>(model,motion,animation,true,true,bindings);
    bindings.reset();require(!weak.expired());
    owner->animation().start(3);require(owner->textures().images()==std::vector<std::int32_t>({0,3,0,-1}));
    owner.reset();require(weak.expired()); // Callback captures no shared cycle.
    auto fixed=std::make_shared<okage::TextureBindings>(model,okage::TextureAnimation{});
    okage::FieldActor static_actor(model,fixed);require(&static_actor.textures()==fixed.get()&&!static_actor.has_motion());
    rejects([&]{okage::FieldActor static_actor(model,std::make_shared<okage::TextureBindings>(model,animation));});
}
}
int main(){try {
    shared_texture_callbacks();private_texture_callbacks();shared_texture_validation_and_lifetime();
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
    std::cout<<"Actor shared texture callbacks and deferred world consumption passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
