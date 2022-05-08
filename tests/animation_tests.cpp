#include "okage/animation.hpp"
#include "okage/pose.hpp"
#include "okage/actor.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value,unsigned at=__builtin_LINE()) {
    if (!value) throw std::runtime_error("animation test assertion failed at line "+std::to_string(at));
}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes.at(at+i)=value>>(i*8);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(72);
    put(b,0,0x544d58); put(b,4,40); put(b,16,0xffffffff); put(b,24,1);
    put(b,32,0x30);
    put(b,48,0x00200042);
    for (unsigned i=0;i<3;++i) put(b,52+i*4,__builtin_bit_cast(std::uint32_t, (0.25f)));
    put(b,64,0x00100002); put(b,68,0);
    return b;
}
template<class F> void rejects(F action) {
    bool failed=false;
    try { action(); } catch (const std::runtime_error&) { failed=true; }
    require(failed);
}
int main() {
    try {
        std::fesetround(FE_TONEAREST);
        auto data=fixture(); okage::Animation animation(data,1); animation.start(0);
        {
            //Bone consumers must not evaluate a pending world transform.
            okage::ModelFile model;model.models.emplace_back().bones.resize(1);
            okage::FieldActor actor(model,data,{},true,true);
            actor.animation().set_channel(0,{2,3,4,1});
            actor.animation().set_channel(4,{5,6,7,1});
            const auto before=actor.animation().channel_outputs();
            actor.evaluate_bones();
            const auto after=actor.animation().channel_outputs();
            require(after[0]==before[0]&&after[1]==before[1]&&after[3]==before[3]);
            require(after[4]==okage::Float4({5,6,7,1}));
            require(actor.animation().value(0)==okage::Float4({2,3,4,1}));
            require(actor.animation().channel_outputs()[1]==before[1]);
            okage::FieldActor fixed(model);require(!fixed.has_motion());
            const auto initial=fixed.animation().channel_outputs();require(initial.size()==4);
            fixed.tick_channels();fixed.tick_script();require(fixed.animation().channel_outputs()==initial);
            rejects([&]{fixed.select_motion(0);});
            fixed.evaluate();require(fixed.channels()[0]==okage::Float4({0,0,0,1}));
            require(fixed.channels()[2]==okage::Float4({1,1,1,1}));
        }
        require(animation.pc()==68 && animation.script_state()==0x10101);
        // Transition setup does not mark output dirty; it is evaluated on tick.
        require(animation.values()[0][0]==1);
        animation.tick(); require(animation.values()[0][0]==0.125f);
        require(animation.script_state()==0x101);
        animation.tick(); require(animation.values()[0][0]==0.25f);
        require(animation.script_state()==1 && animation.pc()==68);
        animation.tick(); require(animation.values()[0][0]==0.25f);
        require(std::fegetround()==FE_TONEAREST);
        okage::Animation cubic(data,1);cubic.set_channel_mode(0,0x10);cubic.start(0);
        cubic.tick();require(cubic.values()[0][0]==.15625f);
        cubic.tick();require(cubic.values()[0][0]==.25f);
        rejects([&]{cubic.set_channel_mode(0,3);});
        // Changing the mode is deferred until a transition installs its flags.
        okage::Animation deferred(data,1);deferred.start(0);deferred.set_channel_mode(0,0x10);
        deferred.tick();require(deferred.values()[0][0]==.125f);
        okage::Animation pushed(data,1);pushed.start(0);pushed.tick();
        require(pushed.values()[0][0]==.125f && pushed.moving(0,0));
        pushed.write_channel_output(0,{5,6,7,1});
        require(pushed.values()[0]==okage::Float4({5,6,7,1}) && pushed.moving(0,0));
        pushed.tick();require(pushed.values()[0][0]==.25f);
        pushed.tick();pushed.values();pushed.write_channel_output(0,{5,6,7,1});
        pushed.tick();require(pushed.values()[0]==okage::Float4({5,6,7,1}));
        rejects([&]{ animation.start(std::numeric_limits<std::size_t>::max()); });
        for (std::size_t length=0;length<data.size();++length)
            rejects([&]{okage::Animation truncated(okage::Bytes(data.data(),length),1);});
        auto unknown=data; put(unknown,32,0x34);
        rejects([&]{okage::Animation a(unknown,1);a.start(0);});
        auto bad=data; put(bad,32,0x00800030);
        rejects([&]{okage::Animation a(bad,1);a.start(0);});
        auto cycle=data; put(cycle,32,1); put(cycle,36,0xfffffffc);
        rejects([&]{okage::Animation a(cycle,1);a.start(0);});
        auto nan=data; put(nan,36,0x7fc00000);
        rejects([&]{okage::Animation a(nan,1);a.start(0);});
        require(std::fegetround()==FE_TONEAREST);
        auto route=fixture();put(route,32,0x0010002e);put(route,36,__builtin_bit_cast(std::uint32_t, (.75f)));
        put(route,40,0x00400140);put(route,44,__builtin_bit_cast(std::uint32_t, (.125f)));
        put(route,48,0x00100008);put(route,52,0);
        okage::Animation actor(route,1),event(route,1);
        actor.set_channel(0,{0,0,0,1});actor.values();event.set_channel_target(&actor);
        event.select(0);require(actor.values()[0][1]==0);event.tick_script();
        require(event.pc()==48 && actor.moving(0,1));
        for(unsigned i=0;i<4;++i)actor.tick_channels();
        require(actor.values()[0][1]==.125f && actor.moving(0,1));
        actor.tick_channels();require(!actor.moving(0,1));event.tick_script();require(event.script_state()==1);
        require(event.values()[0][1]==1);
        rejects([&]{actor.turn_component(0,1,0,0);});
        rejects([&]{actor.set_component(0,4,0);});
        // Scalar linear movement must not take the shortest circular path.
        // This operation shares the queued-target behavior of vector op0x33.
        put(route,40,0x00400131);
        okage::Animation scalar(route,1);scalar.start(0);scalar.tick_channels();
        require(scalar.values()[0][1]==.59375f);
        for(unsigned i=0;i<3;++i)scalar.tick_channels();
        require(scalar.values()[0][1]==.125f);
        okage::Model model;
        model.bones.push_back({{1,1,1,0},{0,0,0,0},{0,0,0,0}}); model.hierarchy={{0,-1}};
        std::vector<okage::Float4> channels(6); channels[4]={0,0,0.25f,0}; channels[5]={2,3,4,1};
        const auto pose=okage::animated_pose(model,channels,okage::make_sine_table());
        require(pose[0][0][0]==0 && pose[0][0][1]==1 && pose[0][3][0]==2);
        channels.pop_back(); rejects([&]{okage::animated_pose(model,channels,okage::make_sine_table());});
        std::cout<<"Animation timing, channels, turns, malformed inputs and loop bounds passed\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
