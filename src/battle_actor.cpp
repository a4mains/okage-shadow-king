#include "okage/input_log.hpp"
#include "okage/battle_actor.hpp"
#include "okage/battle_actor_math.hpp"
#include "okage/scene_model_asset.hpp"
#include "okage/event.hpp"
#include "okage/shadow.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cctype>
#include <fstream>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
using Mutable=std::span<std::uint8_t>;
std::uint32_t word(Bytes b,unsigned at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Battle actor word bounds");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
unsigned half(Bytes b,unsigned at) {
    if(at>b.size()||b.size()-at<2)throw std::runtime_error("Battle actor half bounds");
    return unsigned(b[at])|(unsigned(b[at+1])<<8);
}
int signed_half(Bytes b,unsigned at){return std::bit_cast<std::int16_t>(std::uint16_t(half(b,at)));}
void put(Mutable b,unsigned at,std::uint32_t v,unsigned n=4) {
    if(at>b.size()||b.size()-at<n)throw std::runtime_error("Battle actor write bounds");
    for(unsigned j=0;j<n;++j)b[at+j]=v>>(8*j);
}
void vector(Mutable b,unsigned at,const Float4& value) {
    for(unsigned j=0;j<4;++j)put(b,at+j*4,std::bit_cast<std::uint32_t>(value[j]));
}
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    if(size>128*1024*1024)throw std::runtime_error("Battle model asset size");
    std::vector<std::uint8_t> b(size);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read battle model asset");
    record_input_read(path,b);return b;
}
std::filesystem::path model_path(const std::filesystem::path& decoded,std::string name,bool effect=false) {
    if(name.empty()||std::any_of(name.begin(),name.end(),[](unsigned char c){return !std::isalnum(c)&&c!='_'&&c!='-';}))
        throw std::runtime_error("Invalid battle model resource name");
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});
    return decoded/"BTLDATA/BETA2"/(effect?"EFFECT":"UNIT")/(name+".XPF")/"a.xmd";
}
std::unique_ptr<FieldActor> model(const std::filesystem::path& path,const Float4& position,unsigned flags,
                                const BattleModelAssetProvider& provider,
                                std::shared_ptr<const SceneModelAsset>& owned,
                                std::optional<Model>* shadow=nullptr) {
    std::unique_ptr<FieldActor> actor;
    if(provider) {
        owned=provider(path,flags);
        if(!owned)throw std::runtime_error("Battle owned model provider returned no asset");
        if(owned->motion)
            actor=std::make_unique<FieldActor>(owned->model,*owned->motion,owned->animation,true,true,owned->packet_textures);
        else {
            if(!owned->animation.groups.empty())throw std::runtime_error("Static battle model texture animation is unrecovered");
            actor=std::make_unique<FieldActor>(owned->model,owned->packet_textures);
        }
    } else {
        auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
        TextureAnimation textures;
        if(std::filesystem::exists(xta)) {
            const auto data=read(xta);textures=read_xta({reinterpret_cast<const char*>(data.data()),data.size()});
        }
        if(std::filesystem::exists(motion))actor=std::make_unique<FieldActor>(read_xmd(read(path)),read(motion),std::move(textures),true,true);
        else {
            if(!textures.groups.empty())throw std::runtime_error("Static battle model texture animation is unrecovered");
            actor=std::make_unique<FieldActor>(read_xmd(read(path)));
        }
    }
    const auto selector=(flags>>5)&3;
    if(selector) {
        if(provider) {
            if(owned->shadow) {
                if(!shadow)throw std::runtime_error("Battle shadow owner is missing");
                if(owned->shadow->models.size()!=1)throw std::runtime_error("Battle shadow requires one model");
                *shadow=bind_shadow_model(actor->model().models[0],owned->shadow->models[0]);
            } else if(selector!=1)flags&=~0x60u;
        } else {
            const auto companion=path.parent_path()/"shadow.xmd";
            if(std::filesystem::exists(companion)) {
                if(!shadow)throw std::runtime_error("Battle shadow owner is missing");
                const auto resource=read_xmd(read(companion));
                if(resource.models.size()!=1)throw std::runtime_error("Battle shadow requires one model");
                *shadow=bind_shadow_model(actor->model().models[0],resource.models[0]);
            } else if(selector!=1)flags&=~0x60u;
        }
    }
    // 0x1bae40..58: absent cached shadow clears selector 2/3 bits, while
    // selector 1 is allowed without a companion. Other requested flags survive.
    actor->render_flags=flags;
    actor->animation().set_channel(1,{position[0],position[1],position[2],1});
    return actor;
}
void metadata(BattleActor& a,const std::array<std::uint32_t,27>& unit) {
    for(unsigned j=0;j<27;++j)put(a.storage,0x350+j*4,unit[j]);
}
void base(BattleActor& a,const BattleState& root,unsigned kind,unsigned ordinal,unsigned source,unsigned category,int field8) {
    auto b=Mutable(a.storage);put(b,0x20,0);put(b,0x24,0);put(b,0x2c,word(root.bytes,0x2c));
    put(b,0x40,kind,2);put(b,0x42,ordinal,2);put(b,0x44,source,2);
    put(b,0x46,category,2);put(b,0x48,field8,2);put(b,0x4a,0,2);put(b,0x11c,0);
}
void facing_and_channels(BattleActor& a,float facing) {
    auto rotation=a.model->animation().value(0);rotation[1]=facing;
    a.model->animation().set_channel(0,rotation);
    vector(a.storage,0x90,rotation);
    vector(a.storage,0x60,a.model->animation().value(1));
    vector(a.storage,0xc0,a.model->animation().value(2));
}
void select(BattleActor& a,unsigned motion) {
    put(a.storage,0x128,motion);a.model->select_motion(motion);put(a.storage,0x12c,0xbf800000);
}
void status_block(BattleActor& a) {
    put(a.storage,0x148,0xffff,2);put(a.storage,0x14a,0xffff,2);
    std::fill_n(a.storage.begin()+0x132,10,0xff);
    for(unsigned off:{0xe,0x10,0x12,0x14})put(a.storage,0x132+off,0,2);
}
void common_tail(BattleActor& a) {
    for(unsigned at:{0x310,0x320,0x330})std::fill_n(a.storage.begin()+at,16,0);
    put(a.storage,0x21c,0);for(unsigned at:{0x222,0x224,0x226})put(a.storage,at,0,2);
    put(a.storage,0x300,0);put(a.storage,0x304,0);put(a.storage,0x340,0);
    //156908: scale pointer is typed through the actor owner, not a guest word.
    for(unsigned at:{0x2f0,0x2f4,0x2f8})put(a.storage,at,0);
}
}
unsigned BattleRoster::register_entity(BattleActor* actor,Mutable record,unsigned kind) {
    const auto i=word(root_.bytes,0x50);
    if(i>=battle_entity_count||i!=entities_.size()||kind>7)
        throw std::runtime_error("Battle registration bounds/state");
    auto e=root_.entity(i);
    e[i]|=1u<<kind;
    auto flags=word(e,0x1fc)&0x7fffffff;
    if(kind!=0&&kind!=1){flags|=0x80000000;record={};}
    put(e,0x1fc,flags);put(e,0x1f4,0);put(e,0x1f8,0);
    entities_.push_back({actor,record,kind});put(root_.bytes,0x50,i+1);
    if(actor){actor->entity_id=i;put(actor->storage,0x130,i,2);}return i;
}
void BattleRoster::construct_party(const std::filesystem::path& decoded,Mutable variables,
                                  const GameDataTables& tables,BattleEntryContext& entry,
                                  const std::function<std::uint32_t()>& random,const BattleModelAssetProvider& provider) {
    if(party_done_||!actors_.empty()||variables.size()<0xd78)throw std::runtime_error("Battle party construction state");
    constexpr std::array<Float4,4> positions={Float4{37.5f,0,0,0},Float4{32.5f,0,-30,0},Float4{32.5f,0,30,0},Float4{37.5f,0,0,0}};
    constexpr std::array<unsigned,7> flags={0x200080,0x2000e0,0x2000e0,0x2000e0,0x2000e0,0x2000e0,0x2000a0};
    auto construct=[&](unsigned source,unsigned ordinal) {
        actors_.push_back(std::make_unique<BattleActor>());auto& a=*actors_.back();auto c=Mutable(a.storage);
        a.record=variables.subspan(0x5c4+source*0x50,0x50);
        a.record_before.assign(a.record.begin(),a.record.end());
        const auto draw=[&]{const auto value=random();a.constructor_random_draws.push_back(value);return value;};
        const int unit_id=signed_half(a.record,2);
        base(a,root_,0,ordinal,source,source==6?20:11,-1);put(c,0x4c,0x40a00000);
        metadata(a,tables.unit(unit_id));vector(c,0x60,positions[ordinal]);
        auto name=tables.unit_model_name(unit_id);
        if(source==0&&signed_half(entry.bytes(),0x34)==1)name="lucanb";
        a.model_path=model_path(decoded,name);
        a.model=model(a.model_path,positions[ordinal],flags[source],provider,a.model_asset,&a.bound_shadow); //0x15374c
        facing_and_channels(a,.25f);register_entity(&a,a.record,0);status_block(a);
        a.stat_bindings={a.record.subspan(8,4),a.record.subspan(0,1),a.record.subspan(0x4a,2),
            a.record.subspan(1,1),a.record.subspan(0x4c,2),a.record.subspan(0x4e,2)};
        constexpr std::array<unsigned,10> from={0x18,0x1a,0x1c,0x1e,0x28,0x2a,0x2c,0x2e,0x32,0x34};
        for(unsigned j=0;j<from.size();++j)put(c,0x164+j*2,half(a.record,from[j]),2);
        put(c,0x178,9,2);for(unsigned at:{0x17c,0x180,0x184})put(c,at,0);
        for(unsigned at:{0x188,0x18a,0x18c,0x18e,0x190})put(c,at,0,2);
        if(a.record[0]!=6){a.record[0]=1;put(a.record,0x4a,0,2);}
        const int status=std::bit_cast<std::int8_t>(a.record[1]);
        if(status==6) {
            const auto value=std::bit_cast<std::int16_t>(std::uint16_t(half(a.record,0x4c)+1));
            put(a.record,0x4c,value>999?999:value,2);
            auto remain=std::bit_cast<std::int16_t>(std::uint16_t(9-half(a.record,0x4c)));
            put(c,0x178,remain<=0?1:remain,2);
        } else if(status==7||status==8){a.record[0]=status==7?3:4;put(a.record,0x4a,1,2);}
        select(a,a.record[0]==1?1:a.record[0]==6?9:5);
        if(a.record[0]==6){a.dead_callback=true;a.first_callback=BattleActor::FirstCallback::PartyDeath;put(c,0x24,2);put(c,0x30,0);}
        a.model->tick_channels(); //1b7108: no ordinary script tick here.
        initialize_battle_actor_timer(c.subspan(0x194,0x14),signed_half(c,0x16c),0,signed_half(entry.bytes(),0x32),draw);
        common_tail(a);put(c,0x132,4,1);put(c,0x220,0,2);put(a.record,0x26,0,2);
        if(source==6)put(c,0x1a4,0);
        put(c,0x344,0xffffffff);put(c,0x3bc,0,2);put(c,0x3be,0,2);
        put(c,0x1d4,0xffffffff);put(c,0x1a8,0);put(c,0x1fc,0,2);put(c,0x20,0);
    };
    unsigned ordinal=0;
    for(unsigned slot=0;slot<6&&ordinal<3;++slot)
        if(event_variable(variables,0x40018+slot)==3)construct(slot,ordinal++);
    for(;ordinal<3;++ordinal)register_entity(nullptr,{},4);
    if(event_variable(variables,0x4001e)==3)construct(6,3);
    entry.clear_party_slots();party_done_=true;
}
void BattleRoster::construct_enemies(const std::filesystem::path& decoded,unsigned formation_index,
    std::span<const BattleFormationEntry> formation,const GameDataTables& tables,int entry_half32,
    const std::function<std::uint32_t()>& random,const BattleModelAssetProvider& provider) {
    if(!party_done_||enemies_done_||formation_index>=126||formation.size()+entities_.size()>battle_entity_count)
        throw std::runtime_error("Unrecovered special battle formation or enemy construction state");
    std::int32_t total=0;
    for(unsigned ordinal=0;ordinal<formation.size();++ordinal) {
        const auto& authored=formation[ordinal];actors_.push_back(std::make_unique<BattleActor>());auto& a=*actors_.back();
        a.enemy=true;auto c=Mutable(a.storage);const auto& unit=tables.unit(authored.unit_id);
        const auto draw=[&]{const auto value=random();a.constructor_random_draws.push_back(value);return value;};
        base(a,root_,1,ordinal,authored.unit_id,authored.field_2,authored.field_3);metadata(a,unit);
        a.record=c.subspan(0x350,27*4);
        a.record_before.assign(a.record.begin(),a.record.end());
        float radius=5;switch(unit[25]){case 1:case 3:radius=10;break;case 2:radius=15;break;case 4:radius=60;put(c,0x4a,1,2);break;}
        put(c,0x4c,std::bit_cast<std::uint32_t>(radius));
        const auto old_round=std::fegetround();if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Battle position rounding unavailable");
        const Float4 position={authored.x*5,0,authored.z*5,1};std::fesetround(old_round);
        vector(c,0x60,position);a.model_path=model_path(decoded,tables.unit_model_name(authored.unit_id));
        a.model=model(a.model_path,position,0x2000e0,provider,a.model_asset,&a.bound_shadow); //0x15b708, before warmup RNG.
        facing_and_channels(a,-.25f);select(a,1);a.model->tick_channels();
        const auto warmup=(draw()>>5)&15;
        for(unsigned j=0;j<warmup;++j){a.model->tick_channels();a.model->evaluate_bones();}
        register_entity(&a,a.record,1);status_block(a);
        put(c,0x3ce,0,2);put(c,0x3cd,1,1);put(c,0x3d0,0,1);put(c,0x3d2,0,2);put(c,0x3c8,unit[2]);
        a.stat_bindings={c.subspan(0x3c8,4),c.subspan(0x3cd,1),c.subspan(0x3ce,2),c.subspan(0x3d0,1),c.subspan(0x3d2,2),Mutable{}};
        constexpr std::array<unsigned,10> offsets={8,0x10,0x18,0x20,0x28,0x2c,0x30,0x34,0x3c,0x40};
        for(unsigned j=0;j<offsets.size();++j)put(c,0x164+j*2,half(a.record,offsets[j]),2);
        put(c,0x178,9,2);for(unsigned at:{0x17c,0x180,0x184})put(c,at,0);
        put(c,0x188,half(a.record,0x48),2);put(c,0x18a,half(a.record,0x4c),2);
        initialize_battle_enemy_selector(c.subspan(0x14c,0x44),std::bit_cast<std::int32_t>(word(c,0x3c8)),int(formation.size()),draw);
        put(c,0x190,0,2);
        initialize_battle_actor_timer(c.subspan(0x194,0x14),std::bit_cast<std::int32_t>(unit[10]),1,entry_half32,draw);
        common_tail(a);put(c,0x220,0xffff,2);put(c,0x132,0,1);
        //0x15b920/0x15b954: distinct hidden instances, after selector/timer RNG.
        //Both pass 0x200080 (selector 0), even when BAG/BOX resource caches hit.
        a.bag_path=model_path(decoded,"bag",true);
        a.bag=model(a.bag_path,position,0x200080,provider,a.bag_asset);a.bag->visible=false;
        a.box_path=model_path(decoded,"box",true);
        a.box=model(a.box_path,position,0x200080,provider,a.box_asset);a.box->visible=false;
        put(c,0x20,0);total+=signed_half(c,0x16a);
    }
    put(root_.bytes,0x8074,std::uint32_t(total));put(root_.bytes,0x8078,std::uint32_t(total));enemies_done_=true;
}
}
