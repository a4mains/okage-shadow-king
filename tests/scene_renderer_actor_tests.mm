// Owned-asset, offscreen Metal regression. No NSApplication/window or game replay.
#import <Foundation/Foundation.h>
#include "okage/scene_renderer.hpp"
#include "okage/enemy.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
using namespace okage;
static std::vector<std::uint8_t> read(const fs::path& p) {
    const auto size=fs::file_size(p);
    if(size>128*1024*1024)throw std::runtime_error("Test input limit");
    std::vector<std::uint8_t> b(size);std::ifstream in(p,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),size))throw std::runtime_error("Test input read failed");
    return b;
}
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> static void rejects(F action,const char* message=nullptr) {
    try {action();}catch(const std::exception& e){
        if(message)require(e.what()==std::string(message),"Unexpected rejection");
        return;
    }
    throw std::runtime_error("Expected rejection");
}
static bool same_submissions(const std::vector<FieldShadowSubmission>& a,const std::vector<FieldShadowSubmission>& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)
        if(a[i].alias!=b[i].alias||a[i].selector!=b[i].selector||a[i].bucket!=b[i].bucket||
            a[i].blocks!=b[i].blocks||a[i].triangles!=b[i].triangles||a[i].emitted!=b[i].emitted)return false;
    return true;
}
int main(int argc,const char** argv) {@autoreleasepool {try {
    if(argc!=5)throw std::runtime_error("Usage: scene-renderer-actor-tests STATIC_XMP ACTOR_XMD EFFECT_TEXTURE_DIRECTORY EFD");
    const fs::path map=argv[1],path=argv[2];
    auto motion=path,xta=path,stage=map;motion.replace_extension(".xmt");xta.replace_extension(".xta");stage.replace_extension(".stg");
    const auto model=read_xmd(read(path));
    const auto motionBytes=read(motion);
    TextureAnimation textureAnimation;
    if(fs::exists(xta)){const auto b=read(xta);textureAnimation=read_xta({reinterpret_cast<const char*>(b.data()),b.size()});}
    FieldActor first(model,motionBytes,textureAnimation),second(model,motionBytes,textureAnimation),third(model,motionBytes,textureAnimation);
    const SceneActor bindings[]{{&first,path,-1},{&second,path,2},{&third,path,3}};
    const auto sine=make_sine_table();
    for(auto* actor:{&first,&second,&third}){actor->visible=false;actor->animation().set_channel(1,{0,0,0,1});actor->evaluate();}
    SceneRenderer constructed(map,bindings),appended(map,std::span(bindings,1));
    constructed.load_effect_textures(argv[3]);appended.load_effect_textures(argv[3]);
    const auto sizes=appended.effect_texture_sizes();
    const auto placements=appended.placements(),omitted=appended.omitted_effects();
    FieldProjection projection;projection.load_stage(read(stage));projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    const Float4 eye{0,5,-40,1},camera{};
    const auto view=camera_matrix({0,-5,40,1},camera,{1,1,1,1},sine);
    const auto clip=field_clip_projection(projection.frame().ordinary);
    // Populate immutable preparation before insertion, then verify it survives.
    auto render=[&](SceneRenderer& r){return r.render(view,clip,nullptr,camera,projection.frame().ordinary.clip_far,eye,&projection.frame());};
    const auto before=render(appended);
    rejects([&]{appended.add_actor(bindings[0]);},"Scene actor already registered");
    rejects([&]{appended.add_actor({nullptr,path,4});},"Actor and model path required");
    rejects([&]{appended.add_actor({&second,{},2});},"Actor and model path required");
    rejects([&]{appended.add_actor({&second,path.parent_path()/"missing-actor"/"a.xmd",2});});
    require(appended.actors()==1&&render(appended).rgba==before.rgba,"Failed append changed renderer");
    appended.add_actor(bindings[1]);appended.add_actor(bindings[2]);
    require(appended.actors()==3&&appended.placements()==placements&&appended.omitted_effects()==omitted,"Append count changed placements");
    require(appended.effect_texture_sizes()==sizes,"Append shifted effect texture offsets");
    require(render(appended).rgba==before.rgba,"Hidden actor append changed image");
    rejects([&]{appended.add_actor(bindings[2]);},"Scene actor already registered");
    const SceneActor duplicate[]{bindings[0],bindings[0]};
    rejects([&]{SceneRenderer invalid(map,duplicate);},"Scene actor already registered");
    const auto bank=read_efd(read(argv[4]));
    const auto definition=std::find_if(bank.begin(),bank.end(),[](const auto& d){return d.id==24;});
    require(definition!=bank.end(),"Missing effect24 definition");
    GameRandom rng{1};const auto random=[&]{return rng.next();};
    MapEffects effects;
    effects[0]=std::make_unique<MapEffect>(*definition,Float4{5,0,0,1},Float4{0,0,0,1},Float4{1,1,1,1},sizes,random,EffectStart::FirstFrame);
    effects[0]->shape55_geometry=std::make_shared<const std::vector<MapEffectVertex>>(make_shadow_effect_shape(55,sine,random));
    constructed.bind_map_effects(&effects);appended.bind_map_effects(&effects);
    std::size_t comparisons=0,shadowComparisons=0;bool changed=false;
    for(unsigned tick=0;tick<12;++tick) {
        unsigned index=0;
        for(auto* actor:{&first,&second,&third}) {
            actor->visible=tick!=0;actor->tick_script();actor->tick_channels();
            actor->animation().set_channel(1,{float(index++)*3-3,0,float(tick)*.125f,1});actor->evaluate();
        }
        effects[0]->tick(random,true,&eye);
        const auto a=render(constructed),b=render(appended);
        require(a.rgba==b.rgba,"Constructed/appended RGBA differs");
        require(same_submissions(constructed.shadow_submissions(),appended.shadow_submissions()),"Constructed/appended shadow order differs");
        shadowComparisons+=appended.shadow_submissions().size();
        changed=changed||b.rgba!=before.rgba;++comparisons;
    }
    require(changed,"Fixture did not change the image");
    require(shadowComparisons>0,"Fixture did not exercise companion shadows");
    // A separate append-before-effects instance exercises the other texture order.
    SceneRenderer early(map);for(const auto& binding:bindings)early.add_actor(binding);
    early.load_effect_textures(argv[3]);early.bind_map_effects(&effects);
    const auto visible=render(constructed);
    require(visible.rgba==render(early).rgba,"Append before effects differs");++comparisons;
    for(auto* actor:{&first,&second,&third})actor->visible=false;
    require(render(constructed).rgba!=visible.rgba,"Fixture did not expose actor geometry or shadows");
    for(auto* actor:{&first,&second,&third})actor->visible=true;
    constructed.bind_map_effects(nullptr);
    require(render(constructed).rgba!=visible.rgba,"Fixture did not expose the textured effect");
    std::cout<<"Passed "<<comparisons<<" exact RGBA pairs; "<<shadowComparisons<<" ordered shadow submissions; hidden append and failure recovery\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
