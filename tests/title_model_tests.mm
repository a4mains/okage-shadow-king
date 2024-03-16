#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "okage/scene_renderer.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace okage;
namespace fs=std::filesystem;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size=fs::file_size(path);
    if(size>128*1024*1024)throw std::runtime_error("Model fixture too large");
    std::vector<std::uint8_t> bytes(size);std::ifstream file(path,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("Model fixture read failed");
    return bytes;
}
template<class F> static void rejects(F&& f,const char* expected) {
    try {f();}catch(const std::runtime_error& e){require(e.what()==std::string(expected),"Unexpected guard");return;}
    throw std::runtime_error("Expected explicit descriptor guard");
}
int main(int argc,const char** argv) {@autoreleasepool {try {
    if(argc!=2)throw std::runtime_error("Usage: title-model-tests NIGHT_V_XPF/a.xmd");
    const fs::path path=argv[1];
    const auto model=read_xmd(read(path));
    require(model.models.size()==1&&model.models[0].bones.size()>1,"Expected authored static title hierarchy");
    bool opaque=false,translucent=false;
    for(const auto& block:model.models[0].blocks) {
        require(block.attribute_extents[1]==0,"Fixture unexpectedly uses normal lighting");
        for(const auto& material:block.materials) {
            opaque|=material.words[5]==0;
            translucent|=material.words[5]==0x3f000000;
        }
    }
    require(opaque&&translucent,"Fixture must exercise opaque and half-transparent materials");
    FieldActor actor(model);
    require(!actor.has_motion()&&actor.channels().size()==4&&actor.render_flags==0,"Static instance contract");
    const auto sine=make_sine_table();
    //1296f8 writes XYZ, preserving channel W;1b9be0 reads world channels lazily.
    actor.animation().set_channel(0,{.02f,0,0,1});
    actor.animation().set_channel(1,{0,50,200,1});
    actor.evaluate_world();
    SceneRenderer renderer;
    renderer.add_actor({&actor,path,0,SceneActorDescriptor{0,false}});
    require(renderer.actors()==1,"Title instance not registered");
    require(field_ordinary_bucket(0,0)==5&&field_ordinary_bucket(0,511)==5,"Flags0 gained a depth bucket");
    FieldProjection reset;reset.reset_scene();
    auto projection=reset.frame();projection.ordinary.depth_far=2048;
    const Float4 eye{0,0,-100,1},rotation{};
    const auto view=camera_matrix({0,0,100,1},rotation,{1,1,1,1},sine);
    const auto clip=field_clip_projection(projection.ordinary);
    const auto prepare=[&]{return renderer.prepare(view,clip,nullptr,rotation,
        projection.ordinary.clip_far,eye,&projection,1,false);};
    actor.visible=false;const auto empty=renderer.render_prepared(prepare());
    actor.visible=true;const auto retained=prepare();const auto visible=renderer.render_prepared(retained);
    require(visible.width==640&&visible.height==224,"Unexpected renderer extent");
    bool rgbChanged=false;
    for(std::size_t p=0;p<visible.rgba.size();p+=4)
        for(unsigned c=0;c<3;++c)rgbChanged|=visible.rgba[p+c]!=empty.rgba[p+c];
    require(rgbChanged,"Owned title model did not contribute visible RGB");
    require(renderer.shadow_submissions().empty(),"Flags0 submitted a companion shadow");
    //Mode0 never consumes channel3 ambient; this discriminates flags100000.
    actor.animation().set_channel(3,{0,0,0,0});actor.evaluate();
    require(renderer.render_prepared(prepare()).rgba==visible.rgba,"Flags0 consumed ambient channel3");
    actor.visible=false;
    require(renderer.render_prepared(prepare()).rgba==empty.rgba,"Hidden title still rendered");
    require(renderer.render_prepared(retained).rgba==visible.rgba,"Prepared title borrowed live instance visibility");
    actor.visible=true;actor.animation().set_channel(0,{.02f,.25f,0,1});actor.evaluate_world();
    require(renderer.render_prepared(prepare()).rgba!=visible.rgba,"Title yaw did not change authored geometry");
    require(renderer.render_prepared(retained).rgba==visible.rgba,"Prepared title borrowed world channels");
    auto validations=std::make_shared<unsigned>(0);
    SceneGpuPass titlePass{[validations](id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture>) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(17./255,31./255,47./255,63./255);
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];[encoder endEncoding];
        return [validations]{++*validations;};
    },true};
    const auto ticket=renderer.submit_prepared(retained,nullptr,&titlePass);
    const auto hooked=ticket.readback();
    for(std::size_t p=0;p<hooked.rgba.size();p+=4)
        require(hooked.rgba[p]==17&&hooked.rgba[p+1]==31&&hooked.rgba[p+2]==47&&hooked.rgba[p+3]==63,
            "Title hook was overwritten by model or empty deferred pass");
    require(*validations==1,"Title hook validation missing");
    require(ticket.readback().rgba==hooked.rgba&&*validations==1,"Title hook validation repeated");
    const auto withoutProjection=renderer.prepare(view,clip);
    rejects([&]{(void)renderer.submit_prepared(withoutProjection,nullptr,&titlePass);},
        "Title GPU pass requires GS field projection");
    FieldActor invalid(model);invalid.render_flags=4;
    rejects([&]{renderer.add_actor({&invalid,path,1,SceneActorDescriptor{4,false}});},"Unrecovered explicit actor flags");
    invalid.render_flags=0x80;
    rejects([&]{renderer.add_actor({&invalid,path,1,SceneActorDescriptor{0,false}});},"Explicit actor descriptor disagrees with instance");
    actor.render_flags=4;
    rejects([&]{(void)prepare();},"Explicit actor flags changed after binding");
    std::cout<<"Static NIGHT_V ordinary materials, world channels and retained frame passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
