#include "okage/animation.hpp"
#include "okage/pose.hpp"
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
        require(animation.pc()==68 && animation.script_state()==0x10101);
        // Transition setup does not mark output dirty; it is evaluated on tick.
        require(animation.values()[0][0]==1);
        animation.tick(); require(animation.values()[0][0]==0.125f);
        require(animation.script_state()==0x101);
        animation.tick(); require(animation.values()[0][0]==0.25f);
        require(animation.script_state()==1 && animation.pc()==68);
        animation.tick(); require(animation.values()[0][0]==0.25f);
        require(std::fegetround()==FE_TONEAREST);
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
        okage::Model model;
        model.bones.push_back({{1,1,1,0},{0,0,0,0},{0,0,0,0}}); model.hierarchy={{0,-1}};
        std::vector<okage::Float4> channels(6); channels[4]={0,0,0.25f,0}; channels[5]={2,3,4,1};
        const auto pose=okage::animated_pose(model,channels,okage::make_sine_table());
        require(pose[0][0][0]==0 && pose[0][0][1]==1 && pose[0][3][0]==2);
        channels.pop_back(); rejects([&]{okage::animated_pose(model,channels,okage::make_sine_table());});
        std::cout<<"Animation timing, channels, turns, malformed inputs and loop bounds passed\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
