#include "okage/battle_actor.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value,const char* message,unsigned at=__builtin_LINE()) {
    if(!value)throw std::runtime_error(std::string(message)+" at line "+std::to_string(at));
}
template<class F> void rejects(F action) {
    try {action();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected explicit unsupported scope rejection");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint64_t value,unsigned count=4) {
    for(unsigned i=0;i<count;++i)b[at+i]=std::uint8_t(value>>(8*i));
}
std::uint64_t mask(okage::Bytes b,unsigned at) {
    std::uint64_t value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t(b[at+i])<<(8*i);return value;
}
struct Fixture {
    okage::BattleState root;
    std::array<okage::BattleActor,64> actors;
    std::vector<okage::BattleEntityBinding> bindings;
    explicit Fixture(unsigned count):bindings(count) {
        root.bytes.fill(0xa5);root.initialize_root({1,0,1});put(root.bytes,0x50,count);
        for(unsigned i=0;i<count;++i) {
            auto e=root.entity(i);put(e,0x1f4,0);put(e,0x1f8,0);
            for(unsigned j=0;j<64;++j)e[j]=std::uint8_t(i==j?1u<<(i%3):0x80+i);
        }
    }
    void actor(unsigned i,okage::Float4 position,float radius=2) {
        auto& a=actors[i];a.entity_id=i;
        okage::ModelFile model;model.models.emplace_back();
        a.model=std::make_unique<okage::FieldActor>(std::move(model));
        a.model->animation().set_channel(1,position);
        put(a.storage,0x4c,__builtin_bit_cast(std::uint32_t, (radius)));
        bindings[i].actor=&a;
    }
    void run(){okage::battle_refresh_entities(root,bindings);}
};
void geometry_and_live_channels() {
    Fixture f(2);f.actor(0,{0,0,0,100},2);f.actor(1,{0,3,0,-900},4);
    f.actors[0].model->visible=false;put(f.root.entity(0),0x40,0x80000000); // No visibility/status gate in151528.
    f.run();require(mask(f.root.entity(0),0x50)==2&&mask(f.root.entity(1),0x50)==1,"Radius equality includes XYZ and excludes W");
    require(f.root.entity(0)[1]==2&&f.root.entity(1)[0]==1,"Asymmetric category propagation uses each diagonal");
    require(mask(f.root.entity(0),0x58)==2,"New overlap is reported");
    f.run();require(mask(f.root.entity(0),0x58)==0&&mask(f.root.entity(1),0x58)==0,"Unchanged overlap is not new");
    f.actors[1].model->animation().set_channel(1,{0,std::nextafter(3.f,4.f),0,0});
    f.run();require(mask(f.root.entity(0),0x50)==0&&f.root.entity(0)[1]==0&&f.root.entity(1)[0]==0,"Beyond radius on Y clears both cross bytes");
    f.actors[1].model->animation().set_channel(1,{0,0,0,std::numeric_limits<float>::max()});
    f.run();require(mask(f.root.entity(0),0x50)==2,"Lazy live channel read sees new position and ignores large W");
    require(f.actors[1].model->channels()[1][1]!=f.actors[1].model->animation().value(1)[1] ||
            f.actors[1].model->channels()[1][3]!=f.actors[1].model->animation().value(1)[3],"Fixture retains stale rendered channels");
}
void masks_missing_and_preservation() {
    Fixture f(4);f.actor(0,{0,0,0,1});f.actor(1,{1,0,0,1});f.actor(2,{10,0,0,1});
    put(f.root.entity(0),0x50,6,8); // Existing near bit1 plus stale far bit2.
    put(f.root.entity(1),0x50,0,8);
    put(f.root.entity(3),0x50,~std::uint64_t(0),8);
    const auto before=f.root.bytes;const auto actor_before=f.actors[0].storage;
    f.run();require(mask(f.root.entity(0),0x50)==2&&mask(f.root.entity(0),0x58)==0,"Only newly added bits enter delta mask");
    require(mask(f.root.entity(1),0x58)==1,"Previously absent bit is new for opposite entity");
    require(mask(f.root.entity(3),0x50)==0&&mask(f.root.entity(3),0x58)==0,"Absent body still refreshes masks");
    for(unsigned i=0;i<4;++i) {
        require(f.root.entity(i)[i]==before[0x60+i*0x200+i],"Diagonal category is preserved");
        require(f.root.entity(i)[3]==before[0x60+i*0x200+3]&&f.root.entity(3)[i]==before[0x60+3*0x200+i],"Absent actor preserves both cross bytes");
    }
    for(unsigned at=0;at<before.size();++at) {
        bool mutable_byte=false;
        if(at>=0x60&&at<0x60+4*0x200) {
            const auto i=(at-0x60)/0x200,offset=(at-0x60)%0x200;
            mutable_byte=(offset<3&&i<3&&offset!=i)||(offset>=0x50&&offset<0x60);
        }
        if(!mutable_byte)require(f.root.bytes[at]==before[at],"Refresh preserves all unrelated root bytes and list tails");
    }
    require(f.actors[0].storage==actor_before,"Refresh does not mutate actor task storage");
    Fixture edge(64);edge.actor(0,{0,0,0,1});edge.actor(63,{0,0,0,1});edge.run();
    require(mask(edge.root.entity(0),0x50)==(std::uint64_t(1)<<63)&&mask(edge.root.entity(63),0x50)==1,"Full64 record mask includes highest bit");
    require(edge.root.entity(0)[63]==edge.root.entity(63)[63],"Highest cross-byte index is valid");
    Fixture empty(0);const auto empty_before=empty.root.bytes;empty.run();require(empty.root.bytes==empty_before,"Zero entities preserves root");
}
void validation_and_rounding() {
    for(unsigned at:{0x6cu,0x100u,0x168u,0x16cu,0x1f4u,0x1f8u}) {
        Fixture f(2);f.actor(0,{0,0,0,1});f.actor(1,{1,0,0,1});f.root.entity(1)[at]=1;
        const auto before=f.root.bytes;rejects([&]{f.run();});require(f.root.bytes==before,"Unsupported lists and links reject before mutation");
    }
    Fixture f(1);f.actor(0,{0,0,0,1});
    auto reject_unchanged=[&]{const auto before=f.root.bytes;rejects([&]{f.run();});require(f.root.bytes==before,"Binding validation is atomic");};
    f.actors[0].entity_id=1;reject_unchanged();f.actors[0].entity_id=0;
    f.actors[0].model.reset();reject_unchanged();f.bindings[0].actor=nullptr;
    put(f.root.bytes,0x50,65);reject_unchanged();put(f.root.bytes,0x50,0);reject_unchanged();
    Fixture arithmetic(2);arithmetic.actor(0,{0,0,0,1});arithmetic.actor(1,{2,0,0,1});
    const auto old=std::fegetround();require(std::fesetround(FE_UPWARD)==0,"Set host rounding");
    arithmetic.run();require(std::fegetround()==FE_UPWARD,"Successful refresh restores caller rounding");
    const float one=std::nextafter(2.f,3.f),two=std::nextafter(one,3.f),three=std::nextafter(two,3.f);
    put(arithmetic.actors[1].storage,0x4c,__builtin_bit_cast(std::uint32_t, (three)));
    arithmetic.actors[1].model->animation().set_channel(1,{two,0,0,1});
    arithmetic.run();require(mask(arithmetic.root.entity(0),0x50)==0,"Radius operations use toward-zero, not caller rounding");
    put(arithmetic.actors[1].storage,0x4c,0x7f800000);
    rejects([&]{arithmetic.run();});require(std::fegetround()==FE_UPWARD,"Failure restores caller rounding");
    std::fesetround(old);
}
}
int main() {
    try {geometry_and_live_channels();masks_missing_and_preservation();validation_and_rounding();
        std::cout<<"battle entity update tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
