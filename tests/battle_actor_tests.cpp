#include "okage/battle_actor.hpp"
#include "okage/scene_model_asset.hpp"
#include "okage/event.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool value,const char* why,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error(std::string(why)+" line "+std::to_string(at.line()));
}
template<class F>void rejects(F action) {
    try{action();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected construction failure");
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value,unsigned n=4) {
    for(unsigned i=0;i<n;++i)b[at+i]=std::uint8_t(value>>(i*8));
}
unsigned word(okage::Bytes b,unsigned at) {
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
std::vector<std::uint8_t> table(bool unit) {
    std::string strings;const auto text=[&](std::string_view v){const auto at=strings.size();strings+=v;strings+='\0';return unsigned(at);};
    const auto section=text(unit?"unit":"item"),name=text("Name"),shared=text("shared"),stan=text("stan"),enemy=text("enemy");
    const unsigned columns=unit?27:15,rows=unit?3:1,bytes=columns*rows*4;
    std::vector<std::uint8_t> b(48+bytes+strings.size());
    put(b,0,0x424458);put(b,4,bytes);put(b,8,strings.size());put(b,12,1);put(b,16,0xffffffff);
    put(b,32,section);put(b,40,columns*rows);
    for(unsigned row=0;row<rows;++row) {
        const auto at=48+row*columns*4;put(b,at,name);put(b,at+(unit?1:14)*4,unit?(row==0?shared:row==1?stan:enemy):name);
        if(unit){put(b,at+2*4,100);put(b,at+10*4,50);put(b,at+18*4,4);}
    }
    std::copy(strings.begin(),strings.end(),b.begin()+48+bytes);return b;
}
std::vector<std::uint8_t> xmd() {
    // One bounded authored model, one named root bone, no render blocks.
    std::vector<std::uint8_t> b(128);put(b,0,24);put(b,4,12);put(b,12,1);
    std::copy_n("base",5,b.begin()+16);put(b,24,1);put(b,28,0x55);put(b,32,100);
    put(b,36,24);put(b,40,76);put(b,44,84);put(b,48,88);put(b,52,1);
    for(unsigned at:{56u,60u,64u,68u,100u})put(b,at,0x3f800000);
    put(b,104,1);put(b,108,0xffff0000);put(b,116,1);std::copy_n("root",5,b.begin()+120);return b;
}
std::vector<std::uint8_t> xmt() {
    // Ten eight-byte entries contain END, including battle entries1/5/9.
    std::vector<std::uint8_t> b(112);put(b,0,0x544d58);put(b,4,80);put(b,16,0xffffffff);put(b,24,1);return b;
}
std::shared_ptr<const okage::SceneModelAsset> asset(bool motion=true,bool shadow=false) {
    auto a=std::make_shared<okage::SceneModelAsset>();a->model=okage::read_xmd(xmd());
    if(motion)a->motion=std::make_shared<const std::vector<std::uint8_t>>(xmt());
    if(shadow)a->shadow=std::make_shared<const okage::ModelFile>(a->model);
    // Opaque retained payload metadata, not a claimed source texture allocation.
    okage::ResourceTextureSlot slot;slot.bytes=std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{1,2,3});
    a->textures.push_back(std::move(slot));return a;
}
struct Fixture {
    okage::BattleState root;
    okage::BattleRoster roster{root};
    okage::GameDataTables tables{table(true),table(false)};
    okage::BattleEntryContext entry;
    std::array<std::uint8_t,0xd78> variables{};
    unsigned draws=0;
    std::vector<std::string> events;
    const std::filesystem::path absent="/nonexistent-okage-owned-battle-fixture";
    Fixture(bool party=true) {
        root.initialize_root({1,1,17});
        if(party)for(unsigned source:{0u,1u,6u}) {
            okage::set_event_variable(variables,24+source,4,3);
            const auto at=0x5c4+source*0x50;variables[at]=1;put(variables,at+2,source==6?2:1,2);put(variables,at+0x28,50,2);
        }
    }
    std::uint32_t random(){events.push_back("rng");++draws;return 32;}
    void party(const okage::BattleModelAssetProvider& provider={},const std::filesystem::path& path={}) {
        roster.construct_party(path.empty()?absent:path,variables,tables,entry,[&]{return random();},provider);
    }
    void enemies(const okage::BattleModelAssetProvider& provider={},const std::filesystem::path& path={},unsigned count=2,int entry_half32=0) {
        const std::array<okage::BattleFormationEntry,2> formation={okage::BattleFormationEntry{3,1,0,-2,3},okage::BattleFormationEntry{3,1,0,2,-3}};
        roster.construct_enemies(path.empty()?absent:path,17,std::span(formation).first(count),tables,entry_half32,[&]{return random();},provider);
    }
};
struct Provider {
    struct Call {std::string name;unsigned flags,draws,entities;};
    Fixture& f;
    std::vector<Call> calls;
    std::map<std::string,std::shared_ptr<const okage::SceneModelAsset>> cache;
    bool with_shadow=false;
    std::string fail;
    unsigned authored=0;
    explicit Provider(Fixture& fixture):f(fixture) {}
    std::shared_ptr<const okage::SceneModelAsset> operator()(const std::filesystem::path& path,unsigned flags) {
        const auto name=path.parent_path().stem().string();
        calls.push_back({name,flags,f.draws,unsigned(f.roster.entities().size())});f.events.push_back(name);
        if(name==fail)throw std::runtime_error("Synthetic reached resource failure");
        auto& result=cache[name];if(!result){result=asset(name!="BAG"&&name!="BOX",with_shadow&&((flags>>5)&3));++authored;}
        return result;
    }
    okage::BattleModelAssetProvider callback(){return [this](const auto& path,unsigned flags){return (*this)(path,flags);};}
};
void ordered_owned_cache() {
    Fixture f;Provider p{f};const auto callback=p.callback();f.party(callback);f.enemies(callback);
    require(f.events==std::vector<std::string>{"SHARED","rng","SHARED","rng","STAN","rng","ENEMY","rng","rng","rng","BAG","BOX","ENEMY","rng","rng","rng","BAG","BOX"},"Source resource/RNG interleave");
    require(p.calls.size()==9&&p.authored==5&&f.draws==9,"Every reached source boundary invokes provider, including hits");
    const std::array<unsigned,9> draws={0,1,2,3,6,6,6,9,9},flags={0x200080,0x2000e0,0x2000a0,0x2000e0,0x200080,0x200080,0x2000e0,0x200080,0x200080};
    for(unsigned i=0;i<p.calls.size();++i)require(p.calls[i].draws==draws[i]&&p.calls[i].flags==flags[i],"Requested flags and pre/post actor RNG boundary");
    require(p.calls[3].entities==4&&p.calls[4].entities==5&&p.calls[6].entities==5&&p.calls[7].entities==6,"Main precedes registration, children follow it");
    const auto& a=f.roster.actors();require(a.size()==5&&f.roster.entities().size()==6,"Party hole and roster construction");
    require(a[0]->model_asset==a[1]->model_asset&&a[0]->model.get()!=a[1]->model.get(),"Cache authored asset reused by distinct mutable instances");
    require(a[0]->model_asset->textures[0].bytes==a[1]->model_asset->textures[0].bytes,"Retained immutable payload identity");
    require(a[3]->model_asset==a[4]->model_asset&&a[3]->bag_asset==a[4]->bag_asset&&a[3]->box_asset==a[4]->box_asset,"Enemy and child cache identity");
    require(a[3]->bag_asset!=a[3]->box_asset&&a[3]->bag.get()!=a[4]->bag.get(),"BAG and BOX distinct resource owners and mutable instances");
    require(!a[3]->bag->visible&&!a[3]->box->visible&&!a[3]->bag->has_motion()&&!a[3]->box->has_motion(),"Owned static hidden children require no filesystem motion fallback");
    require(a[0]->model->render_flags==0x200080&&a[1]->model->render_flags==0x200080&&a[2]->model->render_flags==0x2000a0,"Cache first authored shadow policy and selector1 absence");
    require(!a[1]->bound_shadow&&!a[3]->bound_shadow,"Missing cached shadow clears selector2/3");
    const auto retained=a[3]->bag_asset;p.cache.clear();require(retained==a[3]->bag_asset&&retained->textures[0].bytes->at(2)==3,"Roster retains provider owners independently of provider cache");
}
void shadow_metadata() {
    Fixture f(false);Provider p{f};p.with_shadow=true;const auto callback=p.callback();f.party(callback);f.enemies(callback,{},1);
    const auto& a=*f.roster.actors()[0];require(a.bound_shadow&&a.model_asset->shadow&&a.model->render_flags==0x2000e0,"Owned shadow metadata binds without filesystem access");
    require(!a.bag_asset->shadow&&!a.box_asset->shadow,"BAG/BOX selector0 does not request shadows");
}
void failure_prefixes() {
    {
        Fixture f;unsigned calls=0;auto valid=asset();
        okage::BattleModelAssetProvider provider=[&](const auto&,unsigned){++calls;return calls==1?valid:nullptr;};
        rejects([&]{f.party(provider);});require(calls==2&&f.draws==1&&f.roster.entities().size()==1,"Party failure retains first completed actor RNG/registration");
        require(f.roster.actors().size()==2&&!f.roster.actors()[1]->model&&!f.roster.actors()[1]->model_asset&&f.roster.actors()[1]->record_before.size()==0x50,"Reached party prefix retained without fallback");
    }
    for(const std::string fail:{"ENEMY","BAG","BOX"}) {
        Fixture f(false);Provider p{f};p.fail=fail;const auto callback=p.callback();f.party(callback);
        rejects([&]{f.enemies(callback,{},1);});const auto& a=*f.roster.actors()[0];
        require(f.draws==(fail=="ENEMY"?0u:3u)&&f.roster.entities().size()==(fail=="ENEMY"?3u:4u),"Failure does not precompute or roll back RNG/registration");
        require(p.calls.size()==(fail=="ENEMY"?1u:fail=="BAG"?2u:3u),"No later resources after reached failure");
        require(bool(a.model)==(fail!="ENEMY")&&bool(a.bag)==(fail=="BOX")&&!a.box,"Successful resource prefix survives failure");
        require(word(f.root.bytes,0x8074)==0&&word(f.root.bytes,0x8078)==0,"Formation total not committed after partial failure");
    }
    {
        Fixture f;auto invalid=std::make_shared<okage::SceneModelAsset>();invalid->model=okage::read_xmd(xmd());
        invalid->motion=std::make_shared<const std::vector<std::uint8_t>>();
        rejects([&]{f.party([&](const auto&,unsigned){return invalid;});});
        require(f.draws==0&&f.roster.entities().empty()&&f.roster.actors()[0]->model_asset==invalid,"Invalid nonnull owned motion rejects and retains reached owner");
    }
}
struct Directory {
    std::filesystem::path path;
    Directory(){char pattern[]="/tmp/okage-battle-actor-XXXXXX";const auto p=mkdtemp(pattern);if(!p)throw std::runtime_error("mkdtemp");path=p;}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void write(const std::filesystem::path& path,okage::Bytes bytes) {
    std::filesystem::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(bool(out),"Write synthetic legacy model");
}
void legacy_equivalence_and_null_rejection() {
    Directory directory;
    for(const std::string name:{"SHARED","STAN","ENEMY","BAG","BOX"}) {
        const bool effect=name=="BAG"||name=="BOX";
        const auto path=directory.path/"BTLDATA/BETA2"/(effect?"EFFECT":"UNIT")/(name+".XPF")/"a.xmd";
        write(path,xmd());if(!effect){auto motion=path;motion.replace_extension(".xmt");write(motion,xmt());}
    }
    Fixture legacy,owned;Provider p{owned};legacy.party({},directory.path);legacy.enemies({},directory.path);owned.party(p.callback());owned.enemies(p.callback());
    require(legacy.variables==owned.variables&&legacy.root.bytes==owned.root.bytes&&legacy.draws==owned.draws&&legacy.entry.bytes()==owned.entry.bytes(),"Optional provider preserves legacy root/variable/context/RNG behavior");
    for(unsigned i=0;i<legacy.roster.actors().size();++i) {
        const auto& a=*legacy.roster.actors()[i];const auto& b=*owned.roster.actors()[i];
        require(a.storage==b.storage&&a.record_before==b.record_before&&a.constructor_random_draws==b.constructor_random_draws&&a.entity_id==b.entity_id,"Same constructor stores and per-actor draws");
        require(!a.model_asset&&b.model_asset&&a.model->render_flags==b.model->render_flags&&a.model->motion()==b.model->motion()&&a.model->channels()==b.model->channels(),"Legacy callers unchanged; owned metadata retained separately");
        for(unsigned channel=0;channel<4;++channel)require(a.model->animation().value(channel)==b.model->animation().value(channel),"Identical world channels");
    }
    Fixture rejected;unsigned calls=0;
    rejects([&]{rejected.party([&](const auto&,unsigned)->std::shared_ptr<const okage::SceneModelAsset>{++calls;return {};},directory.path);});
    require(calls==1&&rejected.draws==0&&!rejected.roster.actors()[0]->model,"Null owned provider must reject even with valid legacy files available");
}
}
void rear_attack_entry() {
    Fixture normal,rear;rear.entry.reset(0,1,0,0);
    Provider pn{normal},pr{rear};
    normal.party(pn.callback());rear.party(pr.callback());
    require(normal.draws==3&&rear.draws==0,"Rear entry skips every party timer draw");
    require(rear.events==std::vector<std::string>{"SHARED","SHARED","STAN"},"No party RNG between rear party resources");
    for(unsigned i:{0u,1u,2u}) {
        const auto& n=*normal.roster.actors()[i];const auto& r=*rear.roster.actors()[i];
        if(!n.model)continue;
        require(word(r.storage,0x194)==0&&word(n.storage,0x194)!=0,"Rear party timer starts empty");
        require(word(r.storage,0x198)==word(n.storage,0x198),"Rear entry keeps timer duration");
        require(r.constructor_random_draws.empty()&&n.constructor_random_draws.size()==1,"Per-actor draw record");
    }
    normal.enemies(pn.callback(),{},2,0);rear.enemies(pr.callback(),{},2,1);
    require(normal.draws==9&&rear.draws==6,"Enemy draws unchanged by rear entry");
    const auto& ne=normal.roster.actors();const auto& re=rear.roster.actors();
    for(unsigned i=3;i<5;++i) {
        require(word(re[i]->storage,0x194)==word(ne[i]->storage,0x194)&&word(re[i]->storage,0x194)!=0,"Enemy timer keeps its rand%50 start");
        require(re[i]->constructor_random_draws.size()==3,"Enemy constructor still draws");
    }
    require(rear.entry.parameters().argument2==1&&normal.entry.parameters().argument2==0,"Entry +32 survives construction");
}
int main() {
    try {rear_attack_entry();ordered_owned_cache();shadow_metadata();failure_prefixes();legacy_equivalence_and_null_rejection();std::cout<<"battle actor owned asset tests passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
