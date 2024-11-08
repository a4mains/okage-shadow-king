#include "okage/animation.hpp"
#include "okage/pose.hpp"
#include "okage/actor.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <memory>
#include <source_location>
#include <stdexcept>

void require(bool value,const std::source_location at=std::source_location::current()) {
    if (!value) throw std::runtime_error("animation test assertion failed at line "+std::to_string(at.line()));
}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes.at(at+i)=value>>(i*8);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(72);
    put(b,0,0x544d58); put(b,4,40); put(b,16,0xffffffff); put(b,24,1);
    put(b,32,0x30);
    put(b,48,0x00200042);
    for (unsigned i=0;i<3;++i) put(b,52+i*4,std::bit_cast<std::uint32_t>(0.25f));
    put(b,64,0x00100002); put(b,68,0);
    return b;
}
std::vector<std::uint8_t> sampled_fixture(bool source_wide_header,bool destination_wide,
                                          unsigned destination_channel=0) {
    constexpr std::size_t samples=120;
    std::vector<std::uint8_t> b(samples+24*4);
    put(b,0,0x544d58);put(b,4,88);put(b,8,24*4);put(b,12,0);
    put(b,16,0xffffffff);put(b,24,source_wide_header?1:0);
    put(b,32,0x00f00027|(destination_channel<<10));
    put(b,36,12);
    put(b,40,0x00100002);
    put(b,48,0);put(b,52,0);put(b,56,4);
    put(b,60,std::bit_cast<std::uint32_t>(1.f));
    put(b,64,8);put(b,68,std::bit_cast<std::uint32_t>(1.f));
    put(b,72,12);put(b,76,std::bit_cast<std::uint32_t>(.5f));
    put(b,80,destination_wide?0x10000000:0x00010000);
    put(b,84,4);
    put(b,88,16);put(b,92,std::bit_cast<std::uint32_t>(.5f));
    put(b,96,20);put(b,100,std::bit_cast<std::uint32_t>(.5f));
    put(b,104,destination_wide?0x20000000:0x00020000); // end marker
    for(unsigned i=0;i<24;++i)
        put(b,samples+i*4,std::bit_cast<std::uint32_t>(i<12?float(i%4):float(i)));
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
        {
            okage::Animation movement(1);
            movement.set_channel(0,{0,0,0,1});movement.value(0);
            movement.move_channel(0,{8,16,24,5},4);
            movement.tick_channels();
            require(movement.value(0)==okage::Float4({2,4,6,2}));
            movement.move_channel(0,{16,32,48,9},2); //Keep active segment; queue next.
            for(unsigned n=0;n<3;++n)movement.tick_channels();
            require(movement.value(0)==okage::Float4({8,16,24,5}));
            movement.tick_channels();require(movement.value(0)==okage::Float4({12,24,36,7}));
            movement.tick_channels();require(movement.value(0)==okage::Float4({16,32,48,9}));
            for(unsigned lane=0;lane<4;++lane)require(!movement.moving(0,lane));
            rejects([&]{movement.move_channel(0,{0,0,0,1},0);});
            movement.set_channel_mode(0,0x10);movement.set_channel(0,{0,0,0,1});
            movement.move_channel(0,{8,16,24,5},2);movement.tick_channels();
            require(movement.value(0)==okage::Float4({5,10,15,3.5f}));
        }
        {
            auto pool=std::make_shared<okage::AnimationChannelPool>();
            require(pool->free_channels()==okage::AnimationChannelPool::capacity);
            okage::Animation camera(3),model(2);
            camera.bind_channels(pool);model.bind_channels(pool);
            require(camera.channel_base()==0&&camera.channel_count()==3);
            require(model.channel_base()==3&&model.channel_count()==2);
            require(pool->free_channels()==okage::AnimationChannelPool::capacity-5);

            okage::Animation moving(1);moving.set_channel_mode(0,0x10);
            moving.set_channel(0,{0,0,0,1});moving.value(0);
            moving.move_channel(0,{8,4,2,1},4);moving.tick_channels();
            const auto pending=moving.value(0);
            moving.bind_channels(pool);
            require(moving.channel_base()==5&&moving.value(0)==pending);
            require(moving.moving(0,0));
            moving.tick_channels();require(moving.value(0)[0]==5.f);

            auto script=fixture();put(script,32,0x00d0002e);
            put(script,36,std::bit_cast<std::uint32_t>(.75f));
            put(script,40,0x00100002);put(script,44,0);
            okage::Animation event(script,1);event.set_channel_target(&camera);event.start(0);
            require(camera.channel_outputs().size()==3);
            require(model.value(0)[1]==.75f);
            require(model.channel_outputs()[0][1]==.75f);
            okage::Animation standalone(3),bad_event(script,1);
            bad_event.set_channel_target(&standalone);
            rejects([&]{bad_event.start(0);});

            auto track_pool=std::make_shared<okage::AnimationChannelPool>();
            okage::Animation narrow_destination(1);
            auto wide_source=std::make_unique<okage::Animation>(
                sampled_fixture(true,false),1);
            wide_source->set_channel_target(&narrow_destination);
            wide_source->start(0);
            auto moved_source=std::make_unique<okage::Animation>(std::move(*wide_source));
            wide_source->tick();wide_source->tick();
            require(wide_source->script_state()==1&&wide_source->pc()==0);
            wide_source.reset();
            narrow_destination.bind_channels(track_pool);
            moved_source.reset();
            for(unsigned i=0;i<3;++i)narrow_destination.tick_channels();
            require(narrow_destination.value(0)[0]==6.f);
            for(unsigned i=0;i<2;++i)narrow_destination.tick_channels();
            require(narrow_destination.value(0)[0]==14.f);

            auto wide_destination_bytes=fixture();put(wide_destination_bytes,24,1);
            okage::Animation wide_destination(wide_destination_bytes,1);
            auto narrow_source=std::make_unique<okage::Animation>(
                sampled_fixture(false,true),1);
            narrow_source->set_channel_target(&wide_destination);
            narrow_source->start(0);
            narrow_source.reset();
            for(unsigned i=0;i<3;++i)wide_destination.tick_channels();
            require(wide_destination.value(0)[0]==6.f);
            for(unsigned i=0;i<2;++i)wide_destination.tick_channels();
            require(wide_destination.value(0)[0]==14.f);

            auto alias_pool=std::make_shared<okage::AnimationChannelPool>();
            okage::Animation prefix(1),logical_target(1);
            prefix.bind_channels(alias_pool);logical_target.bind_channels(alias_pool);
            auto old_owner=std::make_unique<okage::Animation>(fixture(),1);
            old_owner->bind_channels(alias_pool);
            require(prefix.channel_base()==0&&logical_target.channel_base()==1&&
                    old_owner->channel_base()==2);
            old_owner->release_channel(0);
            auto reused_bytes=fixture();put(reused_bytes,24,1);
            okage::Animation reused_wide(reused_bytes,1);
            reused_wide.bind_channels(alias_pool);
            require(reused_wide.channel_base()==2);
            old_owner.reset();
            require(alias_pool->free_channels()==okage::AnimationChannelPool::capacity-2);

            auto alias_source=std::make_unique<okage::Animation>(
                sampled_fixture(false,true,1),1);
            alias_source->set_channel_target(&logical_target);
            alias_source->start(0);
            alias_source.reset();
            for(unsigned i=0;i<3;++i)reused_wide.tick_channels();
            require(reused_wide.value(0)[0]==6.f);
            require(logical_target.channel_outputs()[0]==okage::Float4({1,1,1,1}));
            for(unsigned i=0;i<2;++i)reused_wide.tick_channels();
            require(reused_wide.value(0)[0]==14.f);
        }
        {
            auto pool=std::make_shared<okage::AnimationChannelPool>();
            auto old=std::make_unique<okage::Animation>(2);old->bind_channels(pool);
            auto neighbor=std::make_unique<okage::Animation>(2);neighbor->bind_channels(pool);
            require(old->channel_base()==0&&neighbor->channel_base()==2);
            old->release_channel(1);
            auto reused=std::make_unique<okage::Animation>(1);reused->bind_channels(pool);
            require(reused->channel_base()==1);
            old.reset();
            require(pool->free_channels()==okage::AnimationChannelPool::capacity-2);
            auto first_fit=std::make_unique<okage::Animation>(1);first_fit->bind_channels(pool);
            require(first_fit->channel_base()==0);
            auto moved=std::make_unique<okage::Animation>(std::move(*neighbor));
            neighbor.reset();
            require(moved->channel_base()==2&&moved->channel_count()==2);
            reused.reset();first_fit.reset();moved.reset();
            require(pool->free_channels()==okage::AnimationChannelPool::capacity);
        }
        {
            auto pool=std::make_shared<okage::AnimationChannelPool>();
            okage::Animation full(okage::AnimationChannelPool::capacity);full.bind_channels(pool);
            require(full.channel_base()==0&&pool->free_channels()==0);
            rejects([&]{okage::Animation extra(1);extra.bind_channels(pool);});
        }
        {
            auto pool=std::make_shared<okage::AnimationChannelPool>();
            okage::Animation prefix(okage::AnimationChannelPool::capacity-1),last(1);
            prefix.bind_channels(pool);last.bind_channels(pool);
            require(prefix.channel_base()==0&&last.channel_base()==okage::AnimationChannelPool::capacity-1);
            auto at_end=fixture();put(at_end,32,0x0000002e);
            put(at_end,36,std::bit_cast<std::uint32_t>(.625f));
            put(at_end,40,0x00100002);put(at_end,44,0);
            okage::Animation valid(at_end,1);valid.set_channel_target(&last);valid.start(0);
            require(last.value(0)[0]==.625f);
            auto past_end=at_end;put(past_end,32,0x0040002e);
            okage::Animation invalid(past_end,1);invalid.set_channel_target(&last);
            rejects([&]{invalid.start(0);});
        }
        {
            auto pool=std::make_shared<okage::AnimationChannelPool>();
            okage::Animation destination(1),source(2);
            destination.bind_channels(pool);source.bind_channels(pool);
            require(destination.channel_base()==0&&source.channel_base()==1);
            destination=std::move(source);
            require(destination.channel_base()==1&&destination.channel_count()==2);
            require(pool->free_channels()==okage::AnimationChannelPool::capacity-2);
            require(source.channel_count()==0);
        }
        {
            auto two_channel=fixture();
            okage::Animation released(two_channel,2);
            released.start(0);
            released.tick();
            released.set_channel(1,{2,3,4,1});
            released.value(1);
            released.move_channel(1,{8,9,10,1},2);
            require(released.moving(1,0));
            const auto stored=released.value(1);
            require(released.moving(0,0));
            const auto pc=released.pc();
            const auto script_state=released.script_state();
            released.release_channel(1);
            require(released.channel_outputs()[1]==stored);
            require(released.value(1)==stored);
            require(released.pc()==pc && released.script_state()==script_state);
            released.tick_channels();
            require(released.moving(1,0));
            require(released.value(1)==stored);
            require(released.moving(0,0));
            released.tick_script();
            require(released.pc()!=pc || released.script_state()!=script_state);
            bool invalid_index=false;
            try { released.release_channel(2); }
            catch (const std::out_of_range&) { invalid_index=true; }
            require(invalid_index);
        }
        auto data=fixture(); okage::Animation animation(data,1); animation.start(0);
        {
            //0x25 consumes a mode word (not an opcode2 wait), indexes by B,
            //ignores A and routes through the currently selected group.
            auto script=fixture();put(script,32,0x004abc25);put(script,36,0x12340010);
            put(script,40,0x00200431);put(script,44,std::bit_cast<std::uint32_t>(8.f));
            put(script,48,0x00100002);put(script,52,0);
            okage::Animation owner(script,2),target(2);
            target.set_channel(1,{0,0,0,1});target.values();owner.set_channel_target(&target);
            owner.set_service_callback([](auto,auto,auto,auto)->okage::Animation::ServiceResult {
                throw std::runtime_error("Channel mode must not dispatch a field service");
            });
            owner.start(0);require(owner.pc()==52 && owner.script_state()==0x10101);
            target.tick_channels();require(target.value(1)[0]==5.f);
            require(owner.value(1)[0]==1.f && target.value(0)[0]==1.f);
            auto linear=script;put(linear,36,2);
            okage::Animation own(linear,2);own.set_channel(1,{0,0,0,1});own.start(0);
            own.tick_channels();require(own.value(1)[0]==4.f);
            auto invalid=script;put(invalid,36,3);
            rejects([&]{okage::Animation bad(invalid,2);bad.start(0);});
            auto short_mode=std::vector<std::uint8_t>(script.begin(),script.begin()+36);
            put(short_mode,4,4);put(short_mode,32,0x25);
            rejects([&]{okage::Animation bad(short_mode,2);bad.start(0);});
        }
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
        auto route=fixture();put(route,32,0x0010002e);put(route,36,std::bit_cast<std::uint32_t>(.75f));
        put(route,40,0x00400140);put(route,44,std::bit_cast<std::uint32_t>(.125f));
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
