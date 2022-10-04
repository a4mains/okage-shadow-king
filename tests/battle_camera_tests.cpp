#include "okage/battle_camera.hpp"
#include "okage/animation.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

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
    auto f=[&](unsigned at,float x) { put(at,__builtin_bit_cast(std::uint32_t, (x))); };
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
            okage::BattleCameraMotion motion(seq,1);
            okage::Animation channels(3);
            channels.set_channel(0,{7,8,9,10});channels.set_channel(1,{11,12,13,37});
            channels.set_channel(2,{14,15,16,17});
            motion.tick(channels);
            require(motion.rotation()[3]==37);
            require(channels.value(1)==motion.rotation());
            require(channels.value(0)==okage::Float4({7,8,9,10}));
            require(channels.value(2)==okage::Float4({14,15,16,17}));
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
        std::cout<<"Battle camera timing, sequence, projection and guards passed\n";
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
