#include "okage/battle_data.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
void require(bool v) { if(!v) throw std::runtime_error("Battle data assertion failed"); }
template<class F> void rejects(F f) {
    bool failed=false; try { f(); } catch(const std::runtime_error&) { failed=true; }
    require(failed);
}
void put(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t v) {
    for(unsigned i=0;i<4;++i) b.at(p+i)=std::uint8_t(v>>(8*i));
}
constexpr std::size_t entries=56+256*8+23*64;
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(entries+36);
    const std::string magic="OKBTDAT1";
    std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1); put(b,12,256); put(b,16,23); put(b,20,3);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i) b[24+i]=std::uint8_t(std::stoul(hash.substr(i*2,2),nullptr,16));
    for(unsigned i=0;i<256;++i) put(b,56+i*8+4,0xffffffff);
    put(b,56+1*8+4,2); put(b,56+255*8,2); put(b,56+255*8+4,1);
    for(unsigned i=0;i<23;++i) {
        const auto path="btldata/beta2/field/synthetic_"+std::to_string(i+1)+".xpf";
        std::copy(path.begin(),path.end(),b.begin()+56+256*8+i*64);
    }
    put(b,entries,0x7f808000); put(b,entries+4,0x80000000); put(b,entries+8,0x7fc12345);
    put(b,entries+12,0x00ffffff); put(b,entries+16,0x3fa00000); put(b,entries+20,0xc0200000);
    put(b,entries+24,0xfd0c7fff); put(b,entries+28,0x40400000); put(b,entries+32,0x40800000);
    return b;
}
}
int main() {
    try {
        const auto b=fixture();
        okage::BattleData data(b);
        require(!data.has_formation(0) && !data.has_formation(2));
        const auto one=data.formation(1);
        require(one.size()==2 && one[0].unit_id==-32768 && one[0].field_2==-128 && one[0].field_3==127);
        require(__builtin_bit_cast(std::uint32_t, (one[0].x))==0x80000000);
        require(__builtin_bit_cast(std::uint32_t, (one[0].z))==0x7fc12345);
        require(one[1].unit_id==-1 && one[1].field_2==-1 && one[1].x==1.25f && one[1].z==-2.5f);
        const auto last=data.formation(255);
        require(last.size()==1 && last[0].unit_id==32767 && last[0].field_3==-3 && last[0].x==3);
        require(data.stage_path(1)=="btldata/beta2/field/synthetic_1.xpf");
        require(data.stage_path(23)=="btldata/beta2/field/synthetic_23.xpf");
        rejects([&]{data.formation(0);}); rejects([&]{data.formation(-1);});
        rejects([&]{data.has_formation(256);}); rejects([&]{data.stage_path(0);});
        rejects([&]{data.stage_path(24);});
        // Input ownership: caller bytes can disappear or change after parsing.
        auto temporary=b; okage::BattleData owned(temporary); temporary.clear();
        require(owned.formation(255)[0].z==4);
        for(const auto p:{std::size_t(0),std::size_t(8),std::size_t(12),std::size_t(16),std::size_t(24)}) {
            auto bad=b; bad[p]^=1; rejects([&]{okage::BattleData d(bad);});
        }
        for(const auto extent:{std::size_t(0),std::size_t(55),entries-1,b.size()-1})
            rejects([&]{okage::BattleData d(okage::Bytes(b).first(extent));});
        auto bad=b; bad.push_back(0); rejects([&]{okage::BattleData d(bad);});
        // Holes, overlaps, invalid absent records, unreferenced data, oversized counts.
        for(const auto mutation:std::vector<std::pair<std::size_t,std::uint32_t>>{
            {56,1},{56+8,1},{56+8+4,0},{56+8+4,11},{56+255*8,1},
            {56+255*8+4,2},{20,0xffffffff},{56+255*8+4,0xffffffff}}) {
            bad=b; put(bad,mutation.first,mutation.second);
            rejects([&]{okage::BattleData d(bad);});
        }
        const auto stage=56+256*8;
        bad=b; std::fill_n(bad.begin()+stage,64,'a'); rejects([&]{okage::BattleData d(bad);});
        bad=b; bad[stage+63]='x'; rejects([&]{okage::BattleData d(bad);});
        bad=b; bad[stage+20]='/'; rejects([&]{okage::BattleData d(bad);});
        bad=b; bad[stage+20]='.'; bad[stage+21]='.'; rejects([&]{okage::BattleData d(bad);});
        std::vector<std::uint8_t> cameras(48+23*2*3*148);
        const std::string camera_magic="OKBTCAM1";
        std::copy(camera_magic.begin(),camera_magic.end(),cameras.begin());
        put(cameras,8,1);put(cameras,12,23);
        std::copy_n(b.begin()+24,32,cameras.begin()+16);
        const unsigned selected=48+16*6*148;
        put(cameras,selected,1);put(cameras,selected+4+0x70,0x44480000); // focal800.
        put(cameras,selected+3*148,1);put(cameras,selected+3*148+4+0x70,0x44800000);
        put(cameras,48,1);put(cameras,52,9); // Unsupported other-stage mode.
        okage::BattleCameraData camera_data(cameras);
        const auto initial=camera_data.sequence(17),later=camera_data.sequence(17,true);
        require(initial[0]&&initial[0]->projection_value==800&&!initial[1]&&!initial[2]);
        require(later[0]&&later[0]->projection_value==1024);
        rejects([&]{camera_data.sequence(0);});rejects([&]{camera_data.sequence(24);});
        rejects([&]{camera_data.sequence(1);});
        auto invalid=cameras;put(invalid,selected,2);rejects([&]{okage::BattleCameraData x(invalid);});
        invalid=cameras;put(invalid,selected,0);rejects([&]{okage::BattleCameraData x(invalid);});
        invalid=cameras;invalid[16]^=1;rejects([&]{okage::BattleCameraData x(invalid);});
        rejects([&]{okage::BattleCameraData x(okage::Bytes(cameras).first(cameras.size()-1));});
        std::cout << "Battle data tests passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
