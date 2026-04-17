#include "okage/battle_camera.hpp"
#include "okage/animation.hpp"
#include <bit>
#include <cfenv>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace {
void require(bool value) {
    if (!value) throw std::runtime_error("Battle camera assertion failed");
}
template<class F> void rejects(F&& f) {
    bool rejected=false;
    try { f(); } catch (const std::runtime_error&) { rejected=true; }
    require(rejected);
}

okage::BattleCameraPreset fixed(float position_duration=0,float target_duration=0) {
    okage::BattleCameraPreset p;
    p.position_duration=position_duration;
    p.target_duration=target_duration;
    p.position_start={0,0,0,0}; p.position_end={2,0,0,0};
    p.target_start={0,0,0,0}; p.target_end={0,2,0,0};
    p.projection_duration=5; p.projection_value=1.25f;
    return p;
}
okage::BattleCameraPreset linear(float duration) {
    auto p=fixed(duration,duration);
    p.position_mode=1; p.target_mode=1;
    return p;
}
std::vector<std::uint8_t> preset_bytes(const okage::BattleCameraPreset& p) {
    std::vector<std::uint8_t> b(0x90);
    auto put=[&](unsigned at,std::uint32_t x) {
        for (unsigned i=0;i<4;++i) b[at+i]=static_cast<std::uint8_t>(x>>(8*i));
    };
    auto f=[&](unsigned at,float x) { put(at,std::bit_cast<std::uint32_t>(x)); };
    auto v=[&](unsigned at,const okage::Float4& x) {
        for(unsigned i=0;i<4;++i) f(at+4*i,x[i]);
    };
    put(0,p.position_mode); put(0x30,p.target_mode);
    f(4,p.position_duration); f(0x34,p.target_duration);
    v(0x10,p.position_start); v(0x20,p.position_end);
    v(0x40,p.target_start); v(0x50,p.target_end);
    f(0x64,p.projection_duration); f(0x70,p.projection_value);
    return b;
}
}

int main() {
    try {
        {
            okage::BattleCameraSequence seq{}; seq[0]=linear(2);
            okage::BattleCameraMotion m(seq,1);
            m.tick(); m.tick();
            require(m.position()[0]==2 && m.target()[1]==2);
            require(m.observation()[0]==1 && m.observation()[24]==1);
            require(!m.ready());
            m.tick();
            require(m.position()[0]==3); // authored endpoint is x=2
            require(m.target()[1]==2);   // target snaps rather than overshooting
            require(m.observation()[0]==7 && m.observation()[24]==7);
            require((m.observation()[52]&0xc0000000u)==0xc0000000u);
            require(!m.ready());
        }

        {
            okage::BattleCameraSequence seq{}; seq[0]=fixed(); seq[1]=linear(2);
            okage::BattleCameraMotion m(seq,1);
            auto request=m.take_projection_request();
            require(request && *request==1.25f);
            require(!m.take_projection_request());
            m.tick();
            require(!m.ready() && m.observation()[55]==0);
            m.tick();
            require(!m.ready() && m.observation()[55]==1);
            require(m.position()[0]>0 && m.position()[0]<2);
            request=m.take_projection_request();
            require(request && *request==1.25f);
            require(!m.take_projection_request());
        }

        {
            okage::BattleCameraSequence seq{}; seq[0]=fixed(0,0);
            okage::BattleCameraMotion m(seq,1);
            require(m.take_projection_request().has_value());
            m.tick();
            require((m.observation()[52]&0xc0000000u)==0xc0000000u);
            const auto before=m.position();
            m.tick();
            require(m.ready() && m.observation()[55]==3);
            require(m.position()==before);
            require(!m.take_projection_request());
        }

        {
            okage::BattleCameraSequence seq{};
            seq[0]=fixed(); seq[1]=fixed(); seq[2]=linear(1);
            okage::BattleCameraMotion m(seq,1);
            m.tick(); // slot 0 done
            m.tick(); // select slot 1
            require(m.observation()[55]==1);
            m.tick(); // slot 1 done
            m.tick(); // select slot 2
            require(m.observation()[55]==2);
            m.tick(); m.tick(); // slot 2 completes on second update
            require((m.observation()[52]&0xc0000000u)==0xc0000000u);
            const auto p=m.position(); const auto t=m.target();
            const auto before=m.observation();
            m.tick();
            const auto after=m.observation();
            require(m.ready() && after[55]==3);
            require(m.position()==p && m.target()==t);
            for(unsigned i=0;i<55;++i) require(after[i]==before[i]);
        }

        {
            okage::BattleCameraSequence seq{};seq[0]=linear(2);
            okage::BattleCameraMotion motion(seq,.5f);okage::Animation channels(3);
            channels.set_channel(1,{11,12,13,37});
            motion.tick(channels,[](float){});
            const auto before=motion.observation();
            require(before[53]==1 && before[51]==2);
            const auto position=channels.value(0),rotation=channels.value(1);
            okage::BattleCameraSequence next{};next[0]=fixed(3,4);
            next[0]->position_start={10,20,30,0};
            next[0]->target_start={13,24,35,7};next[0]->projection_value=9;
            motion.restart_sequence(next);
            const auto after=motion.observation();
            okage::BattleCameraMotion fresh(next,.5f);
            require(after!=fresh.observation());
            require(after[53]==1 && after[55]==0 && after[51]==1);
            require(after[11]==before[11]); //aim W is not initialized by14fc50.
            for(unsigned i=12;i<20;++i)require(after[i]==before[i]);
            for(unsigned i=32;i<44;++i)require(after[i]==before[i]);
            require(motion.position()==next[0]->position_start && motion.target()==next[0]->target_start);
            require(channels.value(0)==position && channels.value(1)==rotation);
            unsigned requests=0;
            motion.tick(channels,[&](float value){++requests;require(value==9);require(channels.value(1)==rotation);});
            require(requests==1 && channels.value(0)==position); //fixed callback never publishes position.
            require(motion.observation()[1]==std::bit_cast<std::uint32_t>(2.5f)); //preserved timestep.
            require(channels.value(1)==motion.rotation() && motion.rotation()[3]==37);
            motion.tick(channels,[&](float){++requests;});require(requests==1);
        }
        {
            okage::BattleCameraSequence seq{};seq[0]=linear(1);
            okage::BattleCameraMotion motion(seq,1);
            motion.tick();motion.tick();motion.tick();require(motion.ready());
            okage::BattleCameraSequence next{};next[0]=linear(2);next[1]=fixed();
            next[0]->position_start={20,0,0,0};next[0]->position_end={24,0,0,0};
            motion.restart_sequence(next);
            require(!motion.ready() && motion.observation()[55]==0 && motion.observation()[53]==0);
            require((motion.observation()[52]&0xc0000000u)==0);
            require(motion.take_projection_request()==std::optional<float>(1.25f));
            motion.tick();require(motion.position()[0]==22);
            motion.tick();motion.tick();motion.tick();
            require(!motion.ready() && motion.observation()[55]==1);
        }
        //Validate the complete replacement before changing the live owner.
        {
            okage::BattleCameraSequence seq{};seq[0]=linear(2);
            okage::BattleCameraMotion motion(seq,1);motion.tick();
            const auto before=motion.observation();
            rejects([&]{motion.restart_sequence({});});require(motion.observation()==before);
            auto invalid=seq;invalid[2]=fixed();invalid[2]->target_mode=2;
            rejects([&]{motion.restart_sequence(invalid);});require(motion.observation()==before);
            motion.tick();require(motion.position()[0]==2);
        }

        {
            okage::BattleCameraSequence seq{};seq[0]=linear(2);
            okage::BattleCameraMotion m(seq,1);okage::Animation channels(3);
            channels.set_channel(1,{0,0,0,37});
            m.tick(channels,[](float){});
            const auto before=m.observation();
            const auto old_channels=channels.channel_outputs();
            const std::vector<okage::Float4> saved(old_channels.begin(),old_channels.end());
            m.select_command_camera(0,{0,0,10,1});
            const auto now=m.observation();
            require(now[0]==6&&now[24]==0&&now[53]==0&&now[55]==before[55]);
            require(m.position()==okage::Float4({13.5f,-8,10,1}));
            require(m.target()==okage::Float4({0,-8,0,0}));
            require(now[20]==std::bit_cast<std::uint32_t>(73.5f));
            require(now[21]==std::bit_cast<std::uint32_t>(-44.f));
            require(now[22]==std::bit_cast<std::uint32_t>(-10.f)&&now[23]==0x3d4ccccd);
            for(unsigned i:{12,13,14,15,16,17,18,19,32,33,34,35,36,37,38,39,40,41,42,43})require(now[i]==before[i]);
            require(now[51]==0&&now[48]==0&&now[49]==0x44800000);
            require(!m.take_projection_request());
            require(std::equal(saved.begin(),saved.end(),channels.channel_outputs().begin()));
            unsigned calls=0;m.tick(channels,[&](float){++calls;});
            require(calls==0&&m.position()[0]==16.5f); //delta60*.05, not60/90.
            require(m.position()[2]==9.f&&m.position()[3]<1&&m.position()[3]>.95f);
            require(m.rotation()[3]==37&&channels.value(0)==m.position());
            const auto first=m.position();m.tick();
            require(m.position()[0]-first[0]<3.f&&m.position()[0]>first[0]);
            require(m.observation()[55]==before[55]);
        }
        {
            okage::BattleCameraSequence seq{};seq[0]=fixed();
            okage::BattleCameraMotion m(seq,1);
            //A command selection must cancel even a never-published request.
            m.select_command_camera(1,{0,0,10,1});require(!m.take_projection_request());
            require(m.observation()[22]==std::bit_cast<std::uint32_t>(10.f));
            for(unsigned i=0;i<90;++i)m.tick();
            require(m.observation()[0]==6&&m.observation()[24]==0);
            require(m.observation()[1]==0&&m.observation()[25]==0);
            m.tick();
            require(m.observation()[0]==7&&m.observation()[24]==7);
            require(m.position()[0]<73.5f&&m.position()[0]>70.f);
            require(m.observation()[53]==0);
            m.tick();require(m.ready());
            const auto old_phase=m.observation()[55];
            m.select_command_camera(2,{20,0,-2,0});
            require(m.observation()[55]==old_phase&&m.ready());
            m.tick();require(m.position()[0]==36.5f&&m.position()[2]==-2.f);
        }
        {
            okage::BattleCameraSequence seq{};seq[0]=fixed();
            okage::BattleCameraMotion m(seq,2);m.select_command_camera(0,{0,0,10,1});m.tick();
            require(m.position()[0]==19.5f); //end.W*timestep used before lane product.
            require(m.observation()[1]==std::bit_cast<std::uint32_t>(88.f));
            const auto before=m.observation();
            rejects([&]{m.select_command_camera(3,{0,0,0,1});});require(m.observation()==before);
            rejects([&]{m.select_command_camera(0,{0,std::numeric_limits<float>::infinity(),0,1});});require(m.observation()==before);
        }

        {
            okage::BattleCameraSequence seq{};seq[0]=linear(2);
            okage::BattleCameraMotion motion(seq,1);
            okage::Animation channels(3);
            channels.set_channel(0,{7,8,9,10});channels.set_channel(1,{11,12,13,37});
            channels.set_channel(2,{14,15,16,17});
            unsigned projections=0;
            motion.tick(channels,[&](float value){
                ++projections;require(value==1.25f);
                require(channels.value(1)==okage::Float4({11,12,13,37}));
                require(channels.value(0)==motion.position());
                require(motion.observation()[51]==1);
            });
            require(projections==1&&motion.observation()[51]==2);
            require(motion.rotation()[3]==37);
            require(channels.value(1)==motion.rotation());
            require(channels.value(0)==motion.position());
            require(channels.value(2)==okage::Float4({14,15,16,17}));
        }
        {
            okage::BattleCameraSequence seq{};seq[0]=linear(2);
            okage::BattleCameraMotion motion(seq,1);okage::Animation channels(3);
            channels.set_channel(1,{11,12,13,37});
            rejects([&]{motion.tick(channels,[](float){throw std::runtime_error("test sink");});});
            require(motion.observation()[51]==1);
            require(channels.value(0)==motion.position());
            require(channels.value(1)==okage::Float4({11,12,13,37}));
        }
        {
            okage::BattleCameraSequence seq{};seq[0]=fixed();
            okage::BattleCameraMotion motion(seq,1);okage::Animation channels(3);
            channels.set_channel(0,{7,8,9,10});
            motion.tick(channels,[](float){});
            //The fixed position callback has no1b4220 publication.
            require(channels.value(0)==okage::Float4({7,8,9,10}));
        }
        {
            auto b=preset_bytes(fixed());
            require(okage::read_battle_camera_preset(b).position_mode==0);
            b.pop_back(); rejects([&]{(void)okage::read_battle_camera_preset(b);});
            auto p=fixed(); p.position_mode=2;
            rejects([&]{okage::BattleCameraSequence s{};s[0]=p;okage::BattleCameraMotion m(s,1);});
            p=fixed(); p.position_end[3]=1;
            rejects([&]{okage::BattleCameraSequence s{};s[0]=p;okage::BattleCameraMotion m(s,1);});
            p=linear(0);
            rejects([&]{okage::BattleCameraSequence s{};s[0]=p;okage::BattleCameraMotion m(s,1);});
            p=fixed(); p.projection_value=std::numeric_limits<float>::infinity();
            rejects([&]{okage::BattleCameraSequence s{};s[0]=p;okage::BattleCameraMotion m(s,1);});
            p=fixed(); p.target_start[2]=std::numeric_limits<float>::quiet_NaN();
            rejects([&]{okage::BattleCameraSequence s{};s[0]=p;okage::BattleCameraMotion m(s,1);});
            p=fixed();
            rejects([&]{okage::BattleCameraSequence s{};s[0]=p;okage::BattleCameraMotion m(s,0);});
            rejects([&]{okage::BattleCameraSequence s{};okage::BattleCameraMotion m(s,1);});
        }
        {
            okage::BattleCameraSequence seq{};seq[0]=fixed();
            okage::BattleCameraMotion m(seq,1);
            require(m.observation()[54]==0xffffffffu);
            okage::BattleCameraSequence next{};next[0]=linear(4);next[1]=fixed(1,1);
            next[0]->position_start={1,2,3,0};next[0]->position_end={5,2,3,0};
            next[0]->target_start={0,0,0,0};next[0]->target_end={4,8,0,0};
            next[1]->position_start={0,0,0,0};next[1]->position_end={0,0,0,0};
            next[1]->target_start={1,1,1,1};
            okage::Float4 anchor{10,20,30,1};
            std::vector<int> calls;
            const auto resolve=[&](int entity){calls.push_back(entity);return anchor;};
            m.restart_sequence(next,6,resolve);
            auto o=m.observation();
            require(calls==std::vector<int>{6,6}&&o[54]==6&&o[55]==0&&o[0]==1&&o[24]==1&&o[53]==0);
            require(m.position()==okage::Float4({11,22,33,1})&&m.target()==okage::Float4({10,20,30,1}));
            require(o[20]==std::bit_cast<std::uint32_t>(15.f)&&o[23]==0); //anchored endpoint W zeroed
            require(o[44]==std::bit_cast<std::uint32_t>(14.f)&&o[47]==std::bit_cast<std::uint32_t>(1.f));
            require(o[32]==std::bit_cast<std::uint32_t>(1.f)&&o[33]==std::bit_cast<std::uint32_t>(2.f)&&o[35]==0);
            m.tick();require(m.target()==okage::Float4({11,22,30,1}));
            require(m.position()[0]==12&&m.position()[3]==.75f); //W walks from the anchor's 1 to 0.
            for(unsigned i=0;i<4;++i)m.tick();
            require(calls.size()==2);
            anchor={-1,-2,-3,0};
            m.tick();
            require(calls==std::vector<int>{6,6,6,6}&&m.observation()[55]==1&&m.observation()[54]==6);
            require(m.target()==okage::Float4({0,-1,-2,1})&&m.position()==okage::Float4({-1,-2,-3,0}));
            //A fixed anchored position (1502c0) keeps its W sum.
            okage::BattleCameraSequence fixed_row{};fixed_row[0]=fixed();fixed_row[0]->position_end={0,0,0,0};
            anchor={1,1,1,1};m.restart_sequence(fixed_row,2,resolve);
            require(m.observation()[0]==0&&m.observation()[23]==std::bit_cast<std::uint32_t>(1.f)&&m.observation()[54]==2);
            //Unanchored151170/1510a0 restarts and command cameras store -1.
            m.restart_sequence(seq);require(m.observation()[54]==0xffffffffu);
            m.restart_sequence(fixed_row,2,resolve);m.select_command_camera(0,{0,0,0,1});
            require(m.observation()[54]==0xffffffffu);
            //Rejections leave the live owner unchanged.
            const auto before=m.observation();
            rejects([&]{m.restart_sequence(next,-1,resolve);});
            rejects([&]{m.restart_sequence(next,-2,resolve);});
            rejects([&]{m.restart_sequence(next,3,{});});
            rejects([&]{m.restart_sequence(next,3,[](int)->okage::Float4{throw std::runtime_error("absent body");});});
            rejects([&]{m.restart_sequence(next,3,[](int){return okage::Float4{0,std::numeric_limits<float>::infinity(),0,1};});});
            require(m.observation()==before);
        }
        //Owned row0x37 (ELF 0x22c090) anchored like Stan's 0xcd/0xcf: VU adds
        //under toward-zero rounding, and the linear target step per frame.
        {
            okage::BattleCameraPreset row;
            row.position_mode=1;row.target_mode=1;row.position_duration=160;row.target_duration=160;
            row.position_start={-5.24f,-6,-13.35f,0};row.position_end={-39.6f,-22,-24.08f,0};
            row.target_start={37.5f,-18,0,0};row.target_end={37.5f,-16,0,0};
            row.projection_duration=160;row.projection_value=595;
            okage::BattleCameraSequence seq{};seq[0]=fixed();
            okage::BattleCameraMotion m(seq,1);
            okage::BattleCameraSequence stan{};stan[0]=row;
            m.restart_sequence(stan,3,[](int){return okage::Float4{-12.5f,0,7.25f,1};});
            std::fenv_t saved;std::fegetenv(&saved);std::fesetround(FE_TOWARDZERO);
            volatile float a=-5.24f,b=-12.5f,c=-13.35f,d=7.25f;const float x=a+b,z=c+d;
            volatile float e=-16.f,f=-18.f,divisor=160.f;const float step=(e-f)*(1.f/divisor);
            std::fesetenv(&saved);
            require(m.position()==okage::Float4({x,-6,z,1})&&m.target()==okage::Float4({25,-18,7.25f,1}));
            require(m.observation()[9+24]==std::bit_cast<std::uint32_t>(step)&&m.take_projection_request()==595.f);
        }
        std::cout<<"Battle camera timing, sequence, projection and guards passed\n";
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
