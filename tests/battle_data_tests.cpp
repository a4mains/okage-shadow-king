#include "okage/battle_data.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <iterator>
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
int main(int argc,char** argv) {
    try {
        if((argc==3||argc==4)&&std::string(argv[1])=="--camera-artifact") {
            std::ifstream stream(argv[2],std::ios::binary);
            if(!stream)throw std::runtime_error("Cannot read camera artifact");
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
            //Optional exact version: a stale artifact must not pass a newer check.
            if(argc==4&&(bytes.size()<12||std::to_string(bytes[8])!=argv[3]))
                throw std::runtime_error("Camera artifact version differs from the required version");
            const okage::BattleCameraData camera(bytes);
            constexpr std::array<float,5> expected{1059.f,1059.f,744.f,744.f,744.f};
            for(unsigned offset=0;offset<expected.size();++offset) {
                const auto selected=camera.sequence_at_offset(17,offset);
                require(selected[0]&&selected[0]->projection_value==expected[offset]);
            }
            constexpr std::array<unsigned,5> indices{0x28,0x2b,0x2c,0x2d,0x2e};
            constexpr std::array<float,5> victory_projection{1024.f,744.f,744.f,744.f,595.f};
            for(unsigned i=0;i<indices.size();++i) {
                const auto selected=camera.sequence_by_index(indices[i]);
                require(selected[0]&&selected[0]->projection_value==victory_projection[i]);
            }
            if(okage::Bytes(bytes).size()>=12&&bytes[8]>=4) {
                //151e68 rows, read from the pinned ELF table 0x235ea0 (row
                //0x37: one preset 0x22c090, 160 frames, focal 595, linear
                //position/target; Stan actions 0xcd/0xcf anchor it).
                const auto stan=camera.sequence_by_index(0x37);
                require(stan[0]&&!stan[1]&&!stan[2]&&stan[0]->position_mode==1&&stan[0]->target_mode==1&&
                        stan[0]->position_duration==160&&stan[0]->projection_value==595&&
                        stan[0]->position_start==okage::Float4({-5.24f,-6,-13.35f,0})&&
                        stan[0]->position_end==okage::Float4({-39.6f,-22,-24.08f,0})&&
                        stan[0]->target_start==okage::Float4({37.5f,-18,0,0})&&
                        stan[0]->target_end==okage::Float4({37.5f,-16,0,0}));
                for(unsigned row:{0x05u,0x07u,0x08u,0x09u,0x0au,0x0bu,0x0cu,0x32u,0x35u,0x38u,0x3bu,0x3eu,0x41u,0x44u,0x131u})
                    require(bool(camera.sequence_by_index(row)[0]));
                std::cout<<"Owned v4 camera artifact includes the151e68 rows\n";
            }
            if(okage::Bytes(bytes).size()>=12&&bytes[8]==5) {
                //14b1f0/14b348 phase0 1510a0 rows, read from the pinned ELF
                //table0x235ea0: row0x29 -> preset0x22b670, row0x2a -> 0x22b700.
                //Each has one preset, linear position/target, 300 timestep units.
                const auto party=camera.sequence_by_index(0x29),enemy=camera.sequence_by_index(0x2a);
                require(party[0]&&!party[1]&&!party[2]&&party[0]->position_mode==1&&party[0]->target_mode==1&&
                        party[0]->position_duration==300&&party[0]->target_duration==300&&
                        party[0]->projection_duration==300&&party[0]->projection_value==1024&&
                        party[0]->position_start==okage::Float4({3.84f,-26,-66.67f,0})&&
                        party[0]->position_end==okage::Float4({2.2f,-9,-58.54f,0})&&
                        party[0]->target_start==okage::Float4({37.5f,0,0,0})&&
                        party[0]->target_end==okage::Float4({37.5f,-4,-32.2f,0}));
                require(enemy[0]&&!enemy[1]&&!enemy[2]&&enemy[0]->position_mode==1&&enemy[0]->target_mode==1&&
                        enemy[0]->position_duration==300&&enemy[0]->target_duration==300&&
                        enemy[0]->projection_duration==300&&enemy[0]->projection_value==744&&
                        enemy[0]->position_start==okage::Float4({85.41f,-35,-47.92f,0})&&
                        enemy[0]->position_end==okage::Float4({85.41f,-21,-47.92f,0})&&
                        enemy[0]->target_start==okage::Float4({4.2f,-2,10,0})&&
                        enemy[0]->target_end==okage::Float4({4.2f,-2,10,0}));
                std::cout<<"Owned v5 camera artifact includes escape rows0x29/0x2a\n";
            }
            return 0;
        }
        require(argc==1);
        const auto b=fixture();
        okage::BattleData data(b);
        require(!data.has_formation(0) && !data.has_formation(2));
        const auto one=data.formation(1);
        require(one.size()==2 && one[0].unit_id==-32768 && one[0].field_2==-128 && one[0].field_3==127);
        require(std::bit_cast<std::uint32_t>(one[0].x)==0x80000000);
        require(std::bit_cast<std::uint32_t>(one[0].z)==0x7fc12345);
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
        const auto row0=camera_data.sequence_at_offset(17,0),row1=camera_data.sequence_at_offset(17,1);
        require(row0[0]&&row0[0]->projection_value==800&&row1[0]&&row1[0]->projection_value==1024);
        rejects([&]{camera_data.sequence_at_offset(17,2);});
        rejects([&]{camera_data.sequence_at_offset(17,0xffffffffu);});

        constexpr std::size_t camera_header=48,camera_row=3*148;
        std::vector<std::uint8_t> cameras_v2(camera_header+23*5*camera_row);
        std::copy(camera_magic.begin(),camera_magic.end(),cameras_v2.begin());
        put(cameras_v2,8,2);put(cameras_v2,12,23);
        std::copy_n(b.begin()+24,32,cameras_v2.begin()+16);
        const unsigned selected_v2=camera_header+16*5*camera_row;
        std::copy_n(cameras.begin()+selected,2*camera_row,cameras_v2.begin()+selected_v2);
        constexpr std::array<std::uint32_t,3> focal{0x44960000,0x44c80000,0x44fa0000};
        for(unsigned offset=2;offset<=4;++offset) {
            const auto slot=selected_v2+offset*camera_row;
            put(cameras_v2,slot,1);put(cameras_v2,slot+4+0x70,focal[offset-2]);
        }
        okage::BattleCameraData camera_data_v2(cameras_v2);
        for(unsigned offset=2;offset<=4;++offset) {
            const auto sequence=camera_data_v2.sequence_at_offset(17,offset);
            require(sequence[0]&&std::bit_cast<std::uint32_t>(sequence[0]->projection_value)==focal[offset-2]);
        }
        const auto v2_initial=camera_data_v2.sequence(17),v2_overview=camera_data_v2.sequence(17,true);
        require(v2_initial[0]&&v2_initial[0]->projection_value==800);
        require(v2_overview[0]&&v2_overview[0]->projection_value==1024);
        rejects([&]{camera_data_v2.sequence_at_offset(17,5);});
        constexpr std::array<unsigned,5> indexed{0x28,0x2b,0x2c,0x2d,0x2e};
        constexpr std::size_t v3_header=48,v3_row=3*148;
        std::vector<std::uint8_t> cameras_v3(v3_header+(23*5+indexed.size())*v3_row);
        std::copy(camera_magic.begin(),camera_magic.end(),cameras_v3.begin());
        put(cameras_v3,8,3);put(cameras_v3,12,23);
        std::copy_n(b.begin()+24,32,cameras_v3.begin()+16);
        std::copy(cameras_v2.begin()+v3_header,cameras_v2.end(),cameras_v3.begin()+v3_header);
        constexpr std::array<std::uint32_t,5> indexed_focal{
            0x43a00000,0x43b00000,0x43c00000,0x43d00000,0x43e00000};
        for(unsigned i=0;i<indexed.size();++i) {
            const auto slot=v3_header+(23*5+i)*v3_row;
            put(cameras_v3,slot,1);put(cameras_v3,slot+4+0x70,indexed_focal[i]);
        }
        okage::BattleCameraData camera_data_v3(cameras_v3);
        for(unsigned i=0;i<indexed.size();++i) {
            const auto sequence=camera_data_v3.sequence_by_index(indexed[i]);
            require(sequence[0]&&std::bit_cast<std::uint32_t>(sequence[0]->projection_value)==indexed_focal[i]);
        }
        const unsigned stage17_initial=17*8+0x5c;
        const auto by_stage_index=camera_data_v3.sequence_by_index(stage17_initial+2);
        require(by_stage_index[0]&&std::bit_cast<std::uint32_t>(by_stage_index[0]->projection_value)==focal[0]);
        rejects([&]{camera_data_v3.sequence_by_index(0x29);});
        rejects([&]{camera_data_v3.sequence_by_index(0xffffffffu);});
        rejects([&]{camera_data.sequence_by_index(0x28);});
        rejects([&]{camera_data_v2.sequence_by_index(0x2b);});
        auto invalid_v3=cameras_v3;
        const auto indexed_start=v3_header+23*5*v3_row;
        put(invalid_v3,indexed_start,2);
        rejects([&]{okage::BattleCameraData x(invalid_v3);});
        invalid_v3=cameras_v3;
        put(invalid_v3,indexed_start,0);
        rejects([&]{okage::BattleCameraData x(invalid_v3);});
        rejects([&]{okage::BattleCameraData x(okage::Bytes(cameras_v3).first(cameras_v3.size()-1));});
        constexpr std::array<unsigned,16> command_rows{0x05,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x32,0x35,
                                                         0x37,0x38,0x3b,0x3e,0x41,0x44,0x131};
        std::vector<std::uint8_t> cameras_v4(cameras_v3);
        cameras_v4.resize(cameras_v3.size()+command_rows.size()*v3_row);
        put(cameras_v4,8,4);
        for(unsigned i=0;i<command_rows.size();++i) {
            const auto slot=v3_header+(23*5+indexed.size()+i)*v3_row;
            put(cameras_v4,slot,1);put(cameras_v4,slot+4+0x70,0x44000000u+i);
        }
        okage::BattleCameraData camera_data_v4(cameras_v4);
        for(unsigned i=0;i<indexed.size();++i)
            require(std::bit_cast<std::uint32_t>(camera_data_v4.sequence_by_index(indexed[i])[0]->projection_value)==indexed_focal[i]);
        for(unsigned i=0;i<command_rows.size();++i) {
            const auto sequence=camera_data_v4.sequence_by_index(command_rows[i]);
            require(sequence[0]&&!sequence[1]&&std::bit_cast<std::uint32_t>(sequence[0]->projection_value)==0x44000000u+i);
            rejects([&]{camera_data_v3.sequence_by_index(command_rows[i]);});
        }
        rejects([&]{camera_data_v4.sequence_by_index(0x36);});
        rejects([&]{okage::BattleCameraData x(okage::Bytes(cameras_v4).first(cameras_v4.size()-v3_row));});
        auto v3_sized_v4=cameras_v3;put(v3_sized_v4,8,4);
        rejects([&]{okage::BattleCameraData x(v3_sized_v4);});
        auto v4_sized_v5=cameras_v4;put(v4_sized_v5,8,5);rejects([&]{okage::BattleCameraData x(v4_sized_v5);});
        // V5 retains the v4 rows and appends the escape-parent rows0x29/0x2a.
        // V4 (and older) artifacts never expose them.
        rejects([&]{camera_data_v4.sequence_by_index(0x29);});
        rejects([&]{camera_data_v4.sequence_by_index(0x2a);});
        std::vector<std::uint8_t> cameras_v5(cameras_v4);
        cameras_v5.resize(cameras_v4.size()+2*v3_row);
        put(cameras_v5,8,5);
        for(unsigned i=0;i<2;++i) {
            const auto slot=v3_header+(23*5+indexed.size()+command_rows.size()+i)*v3_row;
            put(cameras_v5,slot,1);put(cameras_v5,slot+4+0x70,0x44100000u+i);
        }
        okage::BattleCameraData camera_data_v5(cameras_v5);
        for(unsigned i=0;i<command_rows.size();++i)
            require(std::bit_cast<std::uint32_t>(camera_data_v5.sequence_by_index(command_rows[i])[0]->projection_value)==0x44000000u+i);
        for(unsigned i=0;i<2;++i) {
            const auto sequence=camera_data_v5.sequence_by_index(0x29+i);
            require(sequence[0]&&!sequence[1]&&!sequence[2]&&
                    std::bit_cast<std::uint32_t>(sequence[0]->projection_value)==0x44100000u+i);
        }
        rejects([&]{camera_data_v5.sequence_by_index(0x2f);});
        rejects([&]{okage::BattleCameraData x(okage::Bytes(cameras_v5).first(cameras_v5.size()-v3_row));});
        auto v6=cameras_v5;put(v6,8,6);rejects([&]{okage::BattleCameraData x(v6);});
        rejects([&]{camera_data.sequence(0);});rejects([&]{camera_data.sequence(24);});
        rejects([&]{camera_data.sequence(1);});
        auto invalid=cameras;put(invalid,selected,2);rejects([&]{okage::BattleCameraData x(invalid);});
        invalid=cameras;put(invalid,selected,0);rejects([&]{okage::BattleCameraData x(invalid);});
        invalid=cameras;invalid[16]^=1;rejects([&]{okage::BattleCameraData x(invalid);});
        rejects([&]{okage::BattleCameraData x(okage::Bytes(cameras).first(cameras.size()-1));});
        std::cout << "Battle data tests passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
