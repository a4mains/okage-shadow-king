#include "okage/battle_ui_roster.hpp"
#include "okage/battle_actor.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected explicit roster scope rejection");
}
void put(std::span<std::uint8_t> b,std::size_t at,std::uint32_t value,unsigned size=4) {
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
void child(okage::BattleActor& actor,int group,int entity) {
    put(actor.storage,0x46,std::uint32_t(group),2);put(actor.storage,0x130,std::uint32_t(entity),2);
}
void available_party() {
    std::array<okage::BattleActor,4> actors;
    std::array<okage::BattleEntityBinding,4> entities{};
    for(unsigned i=0;i<4;++i) {
        entities[i].actor=&actors[i];entities[i].kind=7; // Source has no kind test.
        put(actors[i].storage,0x21c,2);put(actors[i].storage,0x220,2,2);
        // Contradict the task-relative decoy lanes. Registered source actors
        // point 0x40 bytes beyond the task header; availability reads that body.
        put(actors[i].storage,0x1dc,99);put(actors[i].storage,0x1e0,0xfffe,2);
    }
    require(okage::battle_ui_available_party(entities)==3,"Only first three real bindings are queried");
    require(okage::battle_ui_ready_party(entities)==std::array<std::int32_t,4>{0,1,2,-1},"Ready IDs retain source order and fourth sentinel");
    entities[1].actor=nullptr;
    require(okage::battle_ui_ready_party(entities)==std::array<std::int32_t,4>{0,2,-1,-1},"Ready IDs compact absent actors without inventing ordinals");
    entities[1].actor=nullptr;put(actors[2].storage,0x220,0xfffe,2);
    require(okage::battle_ui_available_party(entities)==1,"Missing child and signed -2 status unavailable");
    put(actors[0].storage,0x21c,0x10002);
    put(actors[0].storage,0x1dc,2);put(actors[0].storage,0x1e0,2,2);
    require(okage::battle_ui_available_party(entities)==0,"Full word state must equal2");
    put(actors[0].storage,0x21c,2);put(actors[0].storage,0x220,0xffff0002);
    require(okage::battle_ui_available_party(entities)==1,"Availability reads only body signed halfword1e0, task220");
    put(actors[0].storage,0x220,0xfffe,2);
    require(okage::battle_ui_available_party(entities)==0,"Available-looking task decoys cannot substitute for unavailable body state");
    rejects([&]{okage::battle_ui_available_party(std::span(entities).first(2));});
}
void enemy_counts() {
    okage::BattleState root;std::array<okage::BattleActor,4> actors;
    child(actors[0],-128,3);child(actors[1],-128,4);child(actors[2],-1,5);child(actors[3],4,-1);
    const std::array<const okage::BattleActor*,4> enemies{&actors[0],&actors[1],&actors[2],&actors[3]};
    put(root.entity(4),0x1fc,0x80000000);
    actors[0].entity_id=63;actors[0].dead_callback=true;
    require(okage::battle_ui_enemy_group_count(root,enemies,-128)==1,"Actual root flags exclude matching enemy");
    require(okage::battle_ui_enemy_group_count(root,enemies,0x1234ff80)==1,"Group argument narrows to signed16");
    require(okage::battle_ui_enemy_group_count(root,enemies,-1)==1,"Negative group is a valid matching signed value");
    require(okage::battle_ui_enemy_group_count(root,enemies,7)==0,"Unmatched invalid child index is not read");
    rejects([&]{okage::battle_ui_enemy_group_count(root,enemies,4);});
    child(actors[3],4,64);rejects([&]{okage::battle_ui_enemy_group_count(root,enemies,4);});
    const std::array<const okage::BattleActor*,2> repeated{&actors[0],&actors[0]};
    require(okage::battle_ui_enemy_group_count(root,repeated,-128)==2,"Source enemy list is not deduplicated");
    const std::array<const okage::BattleActor*,1> missing{nullptr};
    rejects([&]{okage::battle_ui_enemy_group_count(root,missing,7);});
    require(okage::battle_ui_enemy_group_count(root,{},7)==0,"Empty enemy manager list");
    put(root.entity(3),0x1fc,0x80000001);
    require(okage::battle_ui_enemy_group_count(root,enemies,-128)==0,"Live root changes are read on each query");
}
void grouping_and_tails() {
    okage::BattleState root;std::array<okage::BattleActor,2> actors;
    child(actors[0],-128,3);child(actors[1],-1,4);
    const std::array<const okage::BattleActor*,2> enemies{&actors[0],&actors[1]};
    const std::array<okage::BattleFormationEntry,7> entries{{
        {-3,-1,0,0,0},{-32768,-128,0,0,0},{88,-128,0,0,0},
        {12,4,0,0,0},{13,4,0,0,0},{14,-128,0,0,0},{15,-1,0,0,0}}};
    okage::BattleUiFormationSummary summary;
    summary.unit_ids.fill(77);summary.counts.fill(88);summary.groups.fill(99);
    summary.refresh_entries(entries,root,enemies,3);
    require(summary.count==3&&summary.unit_ids[0]==-32768&&summary.unit_ids[1]==14&&summary.unit_ids[2]==15,
            "Signed IDs, initial -1 sentinel, adjacent skip and nonadjacent duplicate group");
    require(summary.groups[0]==-128&&summary.groups[1]==-128&&summary.groups[2]==-1&&summary.counts[0]==1,
            "Signed grouping and real enemy count");
    require(summary.unit_ids[3]==77&&summary.counts[7]==88&&summary.groups[7]==99,"Unused tails persist");
    put(root.entity(3),0x1fc,0x80000000);
    summary.refresh_entries(entries,root,enemies,3);
    require(summary.count==1&&summary.unit_ids[0]==15&&summary.unit_ids[1]==14,"Refresh resets count only, preserving old tail");
    summary.refresh_entries({},root,enemies,3);
    require(summary.count==0&&summary.unit_ids[0]==15,"Empty formation only clears count");
    // Demo mode uses source literal8, never queries the enemy manager.
    const std::array<const okage::BattleActor*,1> missing{nullptr};
    summary.refresh_entries(entries,root,missing,4);
    require(summary.count==4&&summary.unit_ids[1]==12&&summary.counts[3]==8,"Mode4 adds zero-live groups with count8");
    std::array<okage::BattleFormationEntry,9> too_many{};
    for(unsigned i=0;i<too_many.size();++i){too_many[i].unit_id=std::int16_t(100+i);too_many[i].field_2=std::int8_t(i);}
    rejects([&]{summary.refresh_entries(too_many,root,{},4);});
    require(summary.count==8&&summary.unit_ids[7]==107,"Summary capacity rejects ninth store after prior source writes");
}
okage::BattleData data_fixture() {
    constexpr std::size_t base=56+256*8+23*64;
    std::vector<std::uint8_t> bytes(base+36);
    const std::string magic="OKBTDAT1";
    std::copy(magic.begin(),magic.end(),bytes.begin());
    put(bytes,8,1);put(bytes,12,256);put(bytes,16,23);put(bytes,20,3);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)bytes[24+i]=std::uint8_t(std::stoul(hash.substr(i*2,2),nullptr,16));
    for(unsigned i=0;i<256;++i)put(bytes,56+i*8+4,0xffffffff);
    for(unsigned i=0;i<3;++i) {
        const unsigned index=i==2?255:i;
        put(bytes,56+index*8,i);put(bytes,60+index*8,1);
        put(bytes,base+i*12,(i+2)*0x10000+(i+1)*11);
    }
    for(unsigned i=0;i<23;++i) {
        const std::string path="btldata/beta2/field/synthetic.xpf";
        std::copy(path.begin(),path.end(),bytes.begin()+56+256*8+i*64);
    }
    return okage::BattleData(bytes);
}
void formation_selection() {
    const auto data=data_fixture();okage::BattleState root;okage::BattleActor actor;
    child(actor,2,3);const std::array<const okage::BattleActor*,1> enemies{&actor};
    okage::BattleUiFormationSummary summary;
    put(root.bytes,0x3c,0xffffffff);summary.refresh(root,data,enemies,3);
    require(summary.count==1&&summary.unit_ids[0]==11,"Signed negative root formation clamps to0");
    put(root.bytes,0x3c,0xffffffff);summary.refresh(root,data,{},4);
    require(summary.count==1&&summary.unit_ids[0]==22&&summary.counts[0]==8,"Mode4 selects formation1 instead of root");
    put(root.bytes,0x3c,2);summary.refresh(root,data,enemies,3);
    require(summary.count==0&&summary.unit_ids[0]==22,"Absent owned table row is source empty, not reader error");
    child(actor,4,3);put(root.bytes,0x3c,255);summary.refresh(root,data,enemies,3);
    require(summary.count==1&&summary.unit_ids[0]==33,"Last owned table entry");
    for(auto index:{256u,10000u,10001u,0x7fffffffu}) {
        put(root.bytes,0x3c,index);rejects([&]{summary.refresh(root,data,enemies,3);});
        require(summary.count==0&&summary.unit_ids[0]==33,"Unimported table access rejects after source count reset");
    }
}
}
int main(){try {
    available_party();enemy_counts();grouping_and_tails();formation_selection();
    std::cout<<"Battle UI roster source-contract tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
