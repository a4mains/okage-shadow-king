#include "okage/battle_enemy_action.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
std::uint32_t word(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=4,"Battle enemy action word extent");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
std::int16_t narrow(std::uint32_t x){return std::bit_cast<std::int16_t>(std::uint16_t(x));}
std::int16_t half(Bytes b,unsigned at) {
    check(at<=b.size()&&b.size()-at>=2,"Battle enemy action halfword extent");
    return narrow(b[at]|(unsigned(b[at+1])<<8));
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t v,unsigned size=4) {
    check(at<=b.size()&&b.size()-at>=size,"Battle enemy action store extent");
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(v>>(i*8));
}
int status(const BattleActor& actor) {
    check(!actor.stat_bindings[1].empty(),"Battle enemy action status binding missing");
    return std::bit_cast<std::int8_t>(actor.stat_bindings[1][0]);
}
std::int32_t draw(const std::function<std::uint32_t()>& random) {
    check(bool(random),"Battle enemy action random owner missing");return std::bit_cast<std::int32_t>(random());
}
int count_entities(const BattleState& root) {
    const auto count=std::bit_cast<std::int32_t>(word(root.bytes,0x50));
    check(count<=64,"Battle enemy action entity extent");return count;
}
std::int16_t choose(BattleActor& actor,const BattleState& root,const BattleEnemyCommands& commands,
                    const std::function<std::uint32_t()>& random) {
    auto& b=actor.storage;
    const int mode=half(b,0x188);
    switch(mode) {
    case 3:put(b,0x18c,std::uint32_t((draw(random)>>5)%5+1),2);break;
    case 4:put(b,0x18c,std::uint32_t(half(b,0x18c))+1,2);break;
    case 5:put(b,0x18c,std::uint32_t(half(b,0x18c))-1,2);break;
    case 6:case 7: {
        int alive=0;const int count=count_entities(root);
        for(int i=4;i<count;++i)if(!(word(root.entity(unsigned(i)),0x1fc)&0x80000000u))++alive;
        if(mode==7)put(b,0x18c,std::uint32_t(alive-1),2);
        else if(alive==1)put(b,0x18c,6,2);
        break;
    }
    case 8:case 9: {
        const auto hp=word(actor.stat_bindings[0],0);
        const int denominator=half(b,0x164);
        check(denominator!=0,"Battle15c3c8 HP ratio zero divisor");
        const auto numerator=std::bit_cast<std::int32_t>(hp*7u-1u);
        //Widen division to avoid host INT_MIN/-1 UB; low32 reproduces EE DIV.
        const auto quotient=std::uint32_t(std::int64_t(numerator)/denominator);
        put(b,0x18c,mode==8?quotient:6u-quotient,2);break;
    }
    default:break;
    }
    if(half(b,0x18c)>6)put(b,0x18c,6,2);
    if(half(b,0x18c)<0)put(b,0x18c,0,2);
    const auto first=draw(random),second=draw(random);
    put(b,0x18e,std::uint32_t(half(b,0x18c)+((first>>5)&3)+((second>>5)&3)-3),2);
    if(half(b,0x18e)>6)put(b,0x18e,6,2);
    if(half(b,0x18e)<0)put(b,0x18e,0,2);
    return commands.command(half(b,0x18a),half(b,0x18e));
}
std::int16_t adjust(BattleActor& actor,BattleState& root,const GameDataTables& tables,std::int16_t action) {
    const auto& row=tables.action(action);
    const auto hp_cost=std::bit_cast<std::int32_t>(row.words[1]);
    if(hp_cost>0) {
        if(status(actor)==5&&half(actor.stat_bindings[2],0)>=2)return 200;
        std::int16_t cost=narrow(std::uint32_t(hp_cost));
        if(hp_cost>=100) {
            const auto product=std::uint32_t(half(actor.storage,0x164))*(std::uint32_t(hp_cost)-100u);
            cost=narrow(std::uint32_t(std::bit_cast<std::int32_t>(product)/100));
            if(cost<1)cost=1;
        }
        const auto remaining=word(actor.stat_bindings[0],0)-std::uint32_t(cost);
        if(std::bit_cast<std::int32_t>(remaining)<=0)return 200;
        put(actor.stat_bindings[0],0,remaining);return action;
    }
    if(status(actor)==5&&half(actor.stat_bindings[2],0)>0)return 200;
    const auto cost=std::bit_cast<std::int32_t>(row.words[2]);
    if(cost<=0)return action;
    const auto remaining=word(root.bytes,0x8078)-std::uint32_t(cost);
    if(std::bit_cast<std::int32_t>(remaining)<0)return 200;
    put(root.bytes,0x8078,remaining);return action;
}
unsigned target(std::int16_t group,const BattleState& root,std::span<const BattleEntityBinding> bindings,
                const std::function<std::uint32_t()>& random) {
    const int count=count_entities(root);
    check(count<0||bindings.size()==unsigned(count),"Battle enemy target binding extent");
    std::array<unsigned,16> candidates{};unsigned used=0;
    for(int i=0;i<count;++i) {
        const auto* actor=bindings[unsigned(i)].actor;
        if(!actor||half(actor->storage,0x46)!=group||status(*actor)==6)continue;
        check(used<candidates.size(),"Battle157de8 candidate capacity");candidates[used++]=unsigned(i);
    }
    if(!used)return 3;
    const auto selected=(draw(random)>>5)%int(used);
    check(selected>=0,"Battle157de8 negative target index");return candidates[unsigned(selected)];
}
}
void battle_decode_enemy_command(BattleActor& actor,BattleState& root,std::span<const BattleEntityBinding> bindings,
    const GameDataTables& tables,const BattleEnemyCommands& commands,const std::function<std::uint32_t()>& random) {
    auto& b=actor.storage;
    for(int i=9;i>=0;--i)b[0x132+unsigned(i)]=0xff;
    put(b,0x140,0,2);
    put(b,0x142,choose(actor,root,commands,random),2);
    put(b,0x142,adjust(actor,root,tables,half(b,0x142)),2);
    const auto type=narrow(tables.action(half(b,0x142)).words[10]);
    switch(type) {
    case 0:
        put(b,0x144,11,2);
        if(half(b,0x190)!=0) {
            put(b,0x146,std::uint32_t(half(b,0x190))-1u,2);
            if(half(b,0x146)>=3)put(b,0x146,2,2);return;
        }
        put(b,0x146,target(11,root,bindings,random),2);return;
    case 3:put(b,0x144,half(b,0x46),2);put(b,0x146,target(half(b,0x46),root,bindings,random),2);return;
    case 5:case 6:put(b,0x144,0xffff,2);put(b,0x146,std::uint32_t(half(b,0x42))+4u,2);return;
    default:put(b,0x146,0xffff,2);put(b,0x144,type==1||type==2?11:type==4?10:12,2);return;
    }
}
}
