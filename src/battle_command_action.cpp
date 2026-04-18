#include "okage/battle_command_action.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint32_t word(const BattleState& r,unsigned at) {
    std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(r.bytes[at+i])<<(8*i);return v;
}
void put(BattleState& r,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)r.bytes[at+i]=std::uint8_t(value>>(8*i));
}
std::int16_t half(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=2,"Battle command action halfword extent");
    return std::bit_cast<std::int16_t>(std::uint16_t(b[at]|(unsigned(b[at+1])<<8)));
}
//root+254+i*200: the typed actor BODY owner of a registered entity.
BattleActor* actor_at(const BattleState& root,std::span<const BattleEntityBinding> entities,std::int32_t index) {
    const auto count=word(root,0x50);
    check(count<=battle_entity_count&&entities.size()==count,"Battle command action entity ownership mismatch");
    //The source indexes root words without a bound; unregistered or negative
    //indices would read unrelated root storage, so they reject here.
    check(index>=0&&std::uint32_t(index)<count,"Battle command action entity index outside registered entities");
    return entities[unsigned(index)].actor;
}
void select_camera(const BattleState& root,std::span<const BattleEntityBinding> entities,const BattleActor& actor,
                   const BattleCommandActionServices& services) {
    const auto anchored=[&](unsigned row) {
        check(bool(services.camera_anchored),"Battle command action camera151100 service missing");
        //151100's second argument is lw root+81ac, not root+30.
        services.camera_anchored(row,std::bit_cast<std::int32_t>(word(root,0x81ac)));
    };
    const auto indexed=[&](unsigned row) {
        check(bool(services.camera_index),"Battle command action camera1510a0 service missing");
        services.camera_index(row);
    };
    //151ef8: lh BODY+1d8 (TASK218, the 157ed0 action).
    const auto action=half(actor.storage,0x218);
    switch(action) {
    case 0x14:case 0x15:anchored(0x32);return;
    case 0x16:case 0x17:case 0x18:indexed(0x35);return;
    case 0x19:anchored(0x38);return;
    case 0x1a:anchored(0x3b);return;
    case 0x1b:anchored(0x41);return;
    case 0x1c:anchored(0x3e);return;
    case 0x1d:anchored(0x44);return;
    case 0xaa:anchored(0x07);return;
    case 0xcd:case 0xcf:anchored(0x37);return;
    case 0xce:indexed(0x0c);return;
    default:break;
    }
    const auto selected=std::bit_cast<std::int32_t>(word(root,0x81ac));
    if(selected>=0&&selected<4){anchored(0x05);return;}
    const auto* target=actor_at(root,entities,selected);
    check(target!=nullptr,"Battle command action default camera reads an absent selected actor");
    const auto radius=std::bit_cast<float>(std::uint32_t(target->storage[0x4c])|(std::uint32_t(target->storage[0x4d])<<8)|
        (std::uint32_t(target->storage[0x4e])<<16)|(std::uint32_t(target->storage[0x4f])<<24));
    anchored(radius==10.f?0x09:radius==15.f?0x0a:radius==60.f?0x0b:0x08);
}
}
BattleCommandActionResult battle_command_action_update(BattleState& root,std::span<const BattleEntityBinding> entities,
                                                       const BattleCommandActionServices& services) {
    const auto owner=std::bit_cast<std::int32_t>(word(root,0x30)); //lw s0,30(s1) at entry
    auto phase=std::bit_cast<std::int32_t>(word(root,0x24));
    if(phase==0) {
        //151ebc: 151098(root+8068 camera) is an empty source function.
        put(root,0x24,word(root,0x24)+1);phase=1;
    }
    if(phase==1) {
        if(auto* actor=actor_at(root,entities,owner))select_camera(root,entities,*actor,services);
        put(root,0x24,word(root,0x24)+1); //152124/152128 reload, then increment.
    } else if(phase!=2) {
        return BattleCommandActionResult::Running; //151ea4/151eb4: no work.
    }
    //152138: the owner is re-read from the root each time it is used.
    auto* actor=actor_at(root,entities,owner);
    if(!actor)return BattleCommandActionResult::Running;
    auto result=BattleCommandActionResult::Running;
    if(battle_secondary_clock(*actor,services.strike)) {
        if(half(actor->storage,0x218)==0x1c) {
            check(bool(services.camera_index),"Battle command action camera1510a0 service missing");
            services.camera_index(0x131);
        } else {
            check(bool(services.camera_offset),"Battle command action camera151170 service missing");
            services.camera_offset(1);
        }
        check(bool(services.reset_type3),"Battle command action HUD168458 service missing");
        services.reset_type3();
        put(root,0x81a8,1); //1519a0
        check(bool(services.clear_roster_mask),"Battle command action roster mask115a70 service missing");
        services.clear_roster_mask(false); //root+806c party roster
        services.clear_roster_mask(true);  //root+8070 enemy roster
        battle_refresh_entities(root,entities); //151290(root+50)
        put(root,0x24,1);
        put(root,0x34,0);
        result=BattleCommandActionResult::Restored;
    }
    battle_secondary_sound(*actor_at(root,entities,owner),services.strike);  //157448
    battle_secondary_effect(*actor_at(root,entities,owner),root,entities,services.strike); //157518
    return result;
}
}
