#include "okage/battle_reward.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool v,const char* what,unsigned at=__builtin_LINE()) {
    if(!v)throw std::runtime_error(std::string(what)+" at line "+std::to_string(at));
}
template<class F> void rejects(F f) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected reward scope rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t v,unsigned size=4) {
    for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(v>>(8*i));
}
unsigned half(okage::Bytes b,unsigned at){return unsigned(b[at])|(unsigned(b[at+1])<<8);}
std::unique_ptr<okage::FieldActor> prop() {
    okage::ModelFile model;model.models.emplace_back();
    auto result=std::make_unique<okage::FieldActor>(std::move(model));result->visible=false;return result;
}
struct Fixture {
    okage::BattleActor enemy;
    std::array<okage::BattleActor,3> party;
    std::array<std::array<std::uint8_t,16>,3> party_record{};
    std::array<std::uint8_t,3> secondary{},primary{9,9,9};
    std::array<std::uint8_t,0x6c> record{},other_record{};
    std::array<std::uint8_t,0x40> reward{};
    std::array<okage::BattleEntityBinding,5> entities{};
    unsigned draws=0,sample=0;
    std::function<std::uint32_t()> random;
    Fixture() {
        enemy.enemy=true;enemy.entity_id=4; // Deliberately differs from live TASK130.
        put(enemy.storage,0x130,3,2);put(enemy.storage,0x178,0xffff,2);
        enemy.box=prop();enemy.bag=prop();enemy.record=other_record;
        put(other_record,0x60,99);put(record,0x60,7);put(record,0x5c,8);
        entities[3]={&enemy,record,1};entities[4]={&enemy,other_record,1};
        for(unsigned i=0;i<3;++i) {
            put(party[i].storage,0x42,i,2);put(party[i].storage,0x44,9,2);
            party[i].stat_bindings[3]=std::span(secondary).subspan(i,1);
            party[i].stat_bindings[1]=std::span(primary).subspan(i,1);
            entities[i]={&party[i],party_record[i],0};
        }
        random=[&]{++draws;return sample;};
    }
    void run(){okage::battle_enemy_reward(enemy,entities,reward,random);}
    void currency(int value,unsigned chance=0) {
        put(enemy.storage,0x178,0,2);put(record,0x58,chance);put(record,0x54,std::uint32_t(value));
    }
};
void item_lists() {
    Fixture f;put(f.reward,4,7,2);put(f.reward,6,0xffff,2);put(f.reward,12,7,2);put(f.reward,14,4,2);
    const auto storage=f.enemy.storage;f.run();
    require(f.draws==0&&half(f.reward,6)==0&&half(f.reward,14)==5,"All duplicate counts update with16-bit wrap");
    require(half(f.reward,8)==0,"Duplicate match suppresses insertion");
    require(f.enemy.box->visible&&!f.enemy.bag->visible&&f.enemy.storage==storage,"Typed box reveal, callback fields unchanged");
    require(half(f.reward,0x26)==57&&half(f.reward,0x2a)==57&&half(f.reward,0x2e)==57,"Experience follows item update");
    Fixture empty;put(empty.reward,4,2,2);put(empty.reward,8,3,2);empty.run();
    require(half(empty.reward,12)==7&&half(empty.reward,14)==1&&half(empty.reward,16)==0,"First empty insertion only");
    Fixture zero;put(zero.record,0x60,0);zero.run();
    for(unsigned i=0;i<8;++i)require(half(zero.reward,4+4*i)==0&&half(zero.reward,6+4*i)==1,"Item0 matches every empty ID");
    Fixture full;for(unsigned i=0;i<8;++i){put(full.reward,4+4*i,20+i,2);put(full.reward,6+4*i,4+i,2);}
    const auto before=full.reward;full.run();
    for(unsigned i=4;i<36;++i)require(full.reward[i]==before[i],"Full unmatched list remains intact");
    require(full.enemy.box->visible&&half(full.reward,0x26)==57,"Full list still reveals box and awards experience");
    Fixture signed_id;put(signed_id.record,0x60,0xffffffff);put(signed_id.reward,4,0xffff,2);put(signed_id.reward,6,5,2);signed_id.run();
    require(half(signed_id.reward,6)==6&&half(signed_id.reward,8)==0,"Signed16 ID matches full signed32 negative item");
    Fixture wide;put(wide.record,0x60,65535);put(wide.reward,4,0xffff,2);put(wide.reward,6,5,2);wide.run();
    require(half(wide.reward,6)==5&&half(wide.reward,8)==0xffff&&half(wide.reward,10)==1,"Compare full32 before low16 insertion");
}
void currency_and_random() {
    for(unsigned count=0;count<=3;++count) {
        Fixture f;f.currency(80);for(unsigned i=0;i<count;++i)f.secondary[i]=9;
        f.run();require(f.draws==1&&half(f.reward,2)==(80u>>count),"Divisor counts secondary9, not primary9");
        require(f.enemy.bag->visible&&!f.enemy.box->visible,"Currency reveals bag");
    }
    for(int amount:{0,-1,std::numeric_limits<int>::min()}) {
        Fixture f;f.currency(amount);f.run();require(half(f.reward,2)==(amount<0?1u:0u),"Negative quotient replaces1, zero retained");
    }
    Fixture truncated;truncated.currency(-1);truncated.secondary[0]=9;truncated.run();
    require(half(truncated.reward,2)==0,"Division truncates negative source toward zero before minimum rule");
    for(unsigned old:{59999u,60000u,65535u}) {
        Fixture f;f.currency(1);put(f.reward,2,old,2);f.run();require(half(f.reward,2)==60000,"Currency signed32 cap before low16 store");
    }
    Fixture wrap;wrap.currency(std::numeric_limits<int>::max());put(wrap.reward,2,1,2);wrap.run();
    require(half(wrap.reward,2)==0,"Wrapped negative currency sum avoids positive cap");
    Fixture negative_rng;negative_rng.currency(80);negative_rng.sample=0xffffffe0u;negative_rng.run();
    require(negative_rng.draws==1&&half(negative_rng.reward,4)==8&&negative_rng.enemy.box->visible,"Signed RNG shift/remainder may be negative");
    Fixture boundary;boundary.currency(80,1);boundary.sample=32;boundary.run();
    require(boundary.enemy.bag->visible,"Roll equality fails strict chance comparison");
    Fixture live;live.currency(80);put(live.other_record,0x58,1);put(live.other_record,0x5c,12);
    live.random=[&]{++live.draws;put(live.enemy.storage,0x130,4,2);return 0u;};live.run();
    require(live.draws==1&&half(live.reward,4)==12,"Random precedes live owner/chance read");
}
void experience_edges() {
    struct Case {int value,party_level;unsigned old,expected;};
    const Case cases[]={{160,16,0,11},{160,15,0,11},{160,6,0,931},{160,5,0,1357},
        {-17,5,0,2},{-1,6,0,3},{-16,5,0,0},{-1600,0,0,65437},
        {0,0,65535,56},{0,0,32760,32817},{0,0,1356,1357}};
    for(const auto& c:cases) {
        Fixture f;f.entities[1].record={};f.entities[2].record={};
        put(f.record,0x50,std::uint32_t(c.value));put(f.party_record[0],4,std::uint32_t(c.party_level),2);
        put(f.reward,0x26,c.old,2);f.run();
        require(half(f.reward,0x26)==c.expected,"Source experience level difference, truncation and narrow-before-cap");
        require(half(f.reward,0x2a)==0&&half(f.reward,0x2e)==0,"Null records skip experience");
    }
    Fixture ordinal;ordinal.entities[1].record={};ordinal.entities[2].record={};put(ordinal.party[0].storage,0x42,2,2);ordinal.run();
    require(half(ordinal.reward,0x26)==0&&half(ordinal.reward,0x2e)==57,"Destination uses BODY ordinal not entity index/source character");
    Fixture aliases;for(auto& a:aliases.party)put(a.storage,0x42,1,2);aliases.run();
    require(half(aliases.reward,0x2a)==171,"Duplicate ordinal owners accumulate in source order");
    Fixture secondary_without_record;secondary_without_record.currency(80);secondary_without_record.entities[0].record={};secondary_without_record.secondary[0]=9;
    secondary_without_record.run();require(half(secondary_without_record.reward,2)==40,"Divisor actor-presence gate differs from experience record gate");
}
void failure_prefixes() {
    Fixture rng;rng.currency(80);rng.random=[]()->unsigned{throw std::runtime_error("random");};const auto old=rng.reward;
    rejects([&]{rng.run();});require(rng.reward==old&&!rng.enemy.bag->visible&&!rng.enemy.box->visible,"RNG failure precedes rewards");
    Fixture box;box.enemy.box.reset();rejects([&]{box.run();});
    require(half(box.reward,4)==7&&half(box.reward,6)==1&&half(box.reward,0x26)==0,"Item write survives missing box before experience");
    Fixture bag;bag.currency(80);bag.enemy.bag.reset();rejects([&]{bag.run();});
    require(half(bag.reward,2)==80&&half(bag.reward,0x26)==0,"Currency survives missing bag before experience");
    Fixture party;party.entities[1].actor=nullptr;rejects([&]{party.run();});
    require(party.enemy.box->visible&&half(party.reward,0x26)==57&&half(party.reward,0x2a)==0,"Earlier party award survives later missing BODY");
    Fixture binding;binding.currency(80);binding.party[2].stat_bindings[3]={};rejects([&]{binding.run();});
    require(binding.draws==1&&half(binding.reward,2)==0&&!binding.enemy.bag->visible,"Divisor failure after draw before currency write");
    Fixture extent;auto short_reward=std::span(extent.reward).first(6);
    rejects([&]{okage::battle_enemy_reward(extent.enemy,extent.entities,short_reward,extent.random);});
    require(!extent.enemy.box->visible,"Item scan validates reached owner before reveal");
    Fixture partial;partial.entities[1].record={};partial.entities[2].record={};put(partial.party[0].storage,0x42,7,2);
    rejects([&]{partial.run();});require(partial.enemy.box->visible&&half(partial.reward,4)==7,"Experience extent fails after item and reveal");
}
}
int main() {
    try {item_lists();currency_and_random();experience_edges();failure_prefixes();std::cout<<"battle reward tests passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
