// Synthetic source assets through public SceneRenderer/MapObjects APIs, offscreen.
#import <Foundation/Foundation.h>
#include "okage/scene_renderer.hpp"
#include "okage/map_objects.hpp"
#include "ui_metal.hpp"
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <source_location>
using namespace okage;
namespace fs=std::filesystem;
using Data=std::vector<std::uint8_t>;
static void require(bool v,const std::source_location at=std::source_location::current()) {
    if(!v)throw std::runtime_error("Scene map material assertion line "+std::to_string(at.line()));
}
static void put(Data& b,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
static void word(Data& b,std::uint32_t v){const auto at=b.size();b.resize(at+4);put(b,at,v);}
static void floats(Data& b,const Float4& v){for(float f:v)word(b,std::bit_cast<unsigned>(f));}
static void write(const fs::path& p,const Data& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("Fixture write");}
struct Surface {Float4 color;float transparency=0;bool normal=false,bypass=false;unsigned blend=0;};
static Data model(std::span<const Surface> surfaces,bool selected,bool textured,bool reverse) {
    // One identity bone/node; source color and normal independent per surface.
    Data prefix(28);put(prefix,0,24);put(prefix,4,12);put(prefix,12,1);
    std::memcpy(prefix.data()+16,"t.xim",6);put(prefix,24,1);
    Data b(20+52+8+4+16);put(b,0,0x15);put(b,8,20);put(b,12,72);put(b,16,80);
    put(b,20,1);for(unsigned i=0;i<3;++i)put(b,24+4*i,0x3f800000);
    put(b,72,1);put(b,76,0xffff0000);put(b,80,1);
    word(b,0x0200001c);word(b,0xffff0000);word(b,0x00010001);floats(b,{0,0,1,1});
    word(b,0x03000000|unsigned(4+32*surfaces.size()));
    for(const auto& s:surfaces){floats(b,{0,0,0,0});word(b,0);word(b,std::bit_cast<unsigned>(s.transparency));word(b,s.blend);word(b,s.bypass?1:0);}
    word(b,0x04000008);word(b,0);
    word(b,0x0500004c);word(b,1);word(b,4<<16);
    for(const auto& p:std::array{Float4{-2,-2,10,1},Float4{2,-2,10,1},Float4{2,2,10,1},Float4{-2,2,10,1}})floats(b,p);
    word(b,0x0600001c);word(b,1);word(b,1<<16);floats(b,{0,0,1,0});
    word(b,0x07000000|unsigned(12+16*surfaces.size()));word(b,0);word(b,unsigned(surfaces.size())<<16);
    for(const auto& s:surfaces)floats(b,s.color);
    word(b,0x0800002c);word(b,0);word(b,4<<16);
    for(unsigned i=0;i<4;++i){word(b,0);word(b,0);}
    word(b,0x09000008);word(b,selected?0:0xffffffff);
    for(unsigned i=0;i<surfaces.size();++i){
        word(b,0x0a000008);word(b,i);
        word(b,0x0c000028);word(b,textured?0x30:0x20);
        for(unsigned c=0;c<4;++c){const auto index=reverse?c:3-c;
            word(b,(surfaces[i].normal?0:0xffff0000u)|index);word(b,(index<<16)|i);}
    }
    word(b,0x01000004);put(b,4,b.size());prefix.insert(prefix.end(),b.begin(),b.end());return prefix;
}
static Data map(bool animated) {
    Data b(64+80+148);put(b,8,64);put(b,12,1);put(b,40,144);put(b,44,1);
    for(unsigned i=0;i<3;++i)put(b,64+(12+i)*4,0x3f800000);
    std::memcpy(b.data()+144,"m.xmd",6);put(b,144+28,animated?8:0);return b;
}
static Data stage(const Float4& base,const Float4& ambient,bool zeroDirections) {
    Data b(32);put(b,0,0x475453);put(b,8,32);put(b,12,zeroDirections?9:6);
    word(b,0x44);floats(b,{1,1,128,1024});
    word(b,0x40);floats(b,base);word(b,0x41);floats(b,ambient);
    for(unsigned i=1;i<=3;++i){word(b,i);floats(b,{0,0,0,0});}
    if(zeroDirections)for(unsigned i=0x11;i<=0x13;++i){word(b,i);floats(b,{0,0,0,0});}
    return b;
}
static Texture sample(const fs::path& dir,std::span<const Surface> surfaces,bool selected,bool textured,
                      bool animated,const Float4& base={1,1,1,1},const Float4& ambient={.25f,.25f,.25f,1},bool zeroDirections=false,bool reverse=false) {
    fs::create_directory(dir);
    write(dir/"m.xmd",model(surfaces,selected,textured,reverse));write(dir/"a.xmp",map(animated));
    const auto stg=stage(base,ambient,zeroDirections);write(dir/"a.stg",stg);
    Data xim(36);put(xim,24,1);put(xim,28,1);xim[32]=32;xim[33]=64;xim[34]=96;xim[35]=128;write(dir/"t.xim",xim);
    Data motion(56);put(motion,0,0x544d58);put(motion,4,24);put(motion,16,0xffffffff);write(dir/"m.xmt",motion);
    MapObjects objects(dir/"a.xmp");
    for(const auto& binding:objects.bindings()) {
        require(binding.actor->render_flags==0);
        binding.actor->animation().set_channel(4,{0,0,0,1});binding.actor->animation().set_channel(5,{0,0,0,1});binding.actor->evaluate();
    }
    SceneRenderer renderer(dir/"a.xmp",{},&objects);renderer.set_clear_rgb({14,18,24});
    require(renderer.actors()==0&&renderer.placements()==1);
    FieldProjection projection;projection.load_stage(stg);projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    const Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    const auto clip=field_clip_projection(projection.frame().ordinary);
    const auto cold=renderer.render(identity,clip,nullptr,{},1024,{},&projection.frame());
    const auto warm=renderer.render(identity,clip,nullptr,{},1024,{},&projection.frame());
    require(cold.rgba==warm.rgba);
    const auto standalone=renderer.render(identity,clip);
    SceneRenderer fresh(dir/"a.xmp",{},&objects);fresh.set_clear_rgb({14,18,24});
    require(standalone.rgba==fresh.render(identity,clip).rgba);
    require(cold.rgba==renderer.render(identity,clip,nullptr,{},1024,{},&projection.frame()).rgba);
    return cold;
}
static std::array<unsigned,3> center(const Texture& t){const auto n=(112*640+320)*4;return {t.rgba[n],t.rgba[n+1],t.rgba[n+2]};}
static Texture battle_actor_sample(const fs::path& dir,std::span<const Surface> surfaces,unsigned flags,
        const Float4& base,const Float4& ambient,bool textured=false,bool no_motion=false) {
    const auto expected=sample(dir,surfaces,true,textured,false,base,ambient);
    auto hidden=map(false);put(hidden,64+12,0x10000);write(dir/"a.xmp",hidden);
    Data motion(56);put(motion,0,0x544d58);put(motion,4,24);put(motion,16,0xffffffff);
    auto resource=read_xmd(model(surfaces,true,textured,false));
    auto owner=no_motion?std::make_unique<FieldActor>(std::move(resource)):
        std::make_unique<FieldActor>(std::move(resource),motion,TextureAnimation{});
    auto& actor=*owner;
    if(no_motion)require(actor.channels().size()==4&&!actor.has_motion());
    else {actor.animation().set_channel(4,{0,0,0,1});actor.animation().set_channel(5,{0,0,0,1});}
    // A decoy channel3 must not replace stage ambient for bit200000.
    actor.animation().set_channel(3,{0,0,0,0});actor.evaluate();actor.render_flags=flags;
    SceneActor binding{&actor,dir/"m.xmd",-1,SceneActorDescriptor{flags,false}};
    SceneRenderer renderer(dir/"a.xmp",std::span(&binding,1));renderer.set_clear_rgb({14,18,24});renderer.set_sort_depth_shift(16);
    FieldProjection projection;projection.load_stage(stage(base,ambient,false));
    projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    const Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    auto render=[&]{return renderer.render(identity,field_clip_projection(projection.frame().ordinary),nullptr,{},1024,{},&projection.frame());};
    const auto image=render();require(image.rgba==expected.rgba);require(renderer.shadow_submissions().empty());
    const auto snapshot=renderer.prepare(identity,field_clip_projection(projection.frame().ordinary),nullptr,{},1024,{},&projection.frame());
    const auto copy=snapshot;
    actor.animation().set_channel(1,{40,0,0,1});actor.evaluate();actor.visible=false;actor.render_flags=0x300080;
    renderer.set_clear_rgb({1,2,3});renderer.set_sort_depth_shift(19);
    require(renderer.render_prepared(copy).rgba==image.rgba);
    const auto live_clear=renderer.render_prepared(copy,nullptr,nullptr,std::array<std::uint8_t,4>{1,2,3,255});
    if(std::all_of(surfaces.begin(),surfaces.end(),[](const auto& s){return s.blend==0;}))
        require(center(live_clear)==center(image));
    require(live_clear.rgba[0]==1&&live_clear.rgba[1]==2&&live_clear.rgba[2]==3);
    actor.visible=true;actor.render_flags=flags;
    require(render().rgba!=image.rgba);
    require(renderer.render_prepared(snapshot).rgba==image.rgba);
    actor.animation().set_channel(1,{0,0,0,1});actor.evaluate();renderer.set_clear_rgb({14,18,24});renderer.set_sort_depth_shift(16);
    actor.animation().set_channel(3,{.9f,.8f,.7f,.6f});actor.evaluate();require(render().rgba==image.rgba);
    for(unsigned phase=0;phase<3;++phase) {
        const Float4 dying{.25f,.25f,.25f,1.f-float(phase)*.5f};
        actor.animation().set_channel(3,dying);actor.evaluate();actor.render_flags=flags|0x100000;
        const auto overrideExpected=sample(dir/("ambient-"+std::to_string(phase)),surfaces,true,textured,false,base,dying);
        require(render().rgba==overrideExpected.rgba);
        require(renderer.render_prepared(snapshot).rgba==image.rgba);
        actor.render_flags=flags;require(render().rgba==image.rgba);
    }
    actor.render_flags=flags^0x40;bool caught=false;try{render();}catch(const std::runtime_error& e){caught=std::string(e.what())=="Explicit actor flags changed after binding";}require(caught);
    actor.render_flags=flags;
    caught=false;try{renderer.set_sort_depth_shift(32);}catch(const std::runtime_error&){caught=true;}require(caught);require(render().rgba==image.rgba);
    SceneActor invalid{&actor,dir/"m.xmd",-1,SceneActorDescriptor{0x300080,false}};
    caught=false;try{SceneRenderer unsupported(dir/"a.xmp",std::span(&invalid,1));}catch(const std::runtime_error& e){caught=std::string(e.what())=="Unrecovered explicit actor flags";}require(caught);
    return image;
}
static void ui_bridge(const fs::path& dir) {
    const std::array<Surface,1> surfaces{{{{.25f,.5f,.75f,1}}}};
    const auto expected=sample(dir,surfaces,false,false,false);
    SceneRenderer renderer(dir/"a.xmp");renderer.set_clear_rgb({14,18,24});
    FieldProjection projection;projection.load_stage(stage({1,1,1,1},{.25f,.25f,.25f,1},false));
    projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    const Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    const auto clip=field_clip_projection(projection.frame().ordinary);
    const UiMetalBaseline baseline{0x6c00,0x7900,true,0,1,0,1};
    UiMetalTextureResolver resolver;
    auto list=[] {
        UiGeometryList l;l.commands={UiDrawControl::Begin,UiDrawRegister{0x49,0},UiDrawRegister{0x47,0x30003},
            UiDrawRegister{0x3b,0x100008080ull},UiDrawRegister{0x4a,0},UiDrawRegister{0x42,0x48},
            UiDrawRegister{0x40,(223ull<<48)|(639ull<<16)},UiDrawRegister{0x14,0x60},UiDrawRegister{8,5},UiDrawCurrentFramebuffer{}};
        return l;
    };
    auto quad=[](std::array<unsigned,4> color,unsigned z) {
        UiResolvedPrimitive p{};p.prim=0x4d;p.quad.emitted=true;
        constexpr std::array<std::array<unsigned,2>,4> xy={{{300,100},{340,100},{340,124},{300,124}}};
        for(unsigned i=0;i<4;++i){auto& v=p.quad.vertices[i];v.xyzf={0x6c00+xy[i][0]*16,0x7900+xy[i][1]*16,z<<4,0};
            for(unsigned j=0;j<4;++j)v.rgba[j]=0x4b000000|color[j];}
        return p;
    };
    auto render=[&](const SceneUiPass* ui=nullptr){return renderer.render(identity,clip,nullptr,{},1024,{},&projection.frame(),1,ui);};
    require(render().rgba==expected.rgba);
    auto geometry=list();SceneUiPass pass{geometry,baseline,resolver};
    require(render(&pass).rgba==expected.rgba); // Empty pass leaves scene untouched.
    geometry.commands.push_back(quad({129,129,129,127},100));
    const auto overlay=render(&pass);const auto before=center(expected),after=center(overlay);
    for(unsigned c=0;c<3;++c)require(after[c]==std::min(255u,before[c]+127));
    require(overlay.rgba[(112*640+320)*4+3]==127); // Raw GS alpha, no post-UI restore.
    require(std::equal(expected.rgba.begin(),expected.rgba.begin()+640*4,overlay.rgba.begin()));
    require(render().rgba==expected.rgba);
    const auto prepared=renderer.prepare(identity,clip,nullptr,{},1024,{},&projection.frame());
    require(renderer.render_prepared(prepared,&pass).rgba==overlay.rgba);
    const auto rawPrepared=renderer.prepare(identity,clip,nullptr,{},1024,{},&projection.frame(),1,false);
    const auto raw=renderer.render_prepared(rawPrepared);
    require(raw.rgba[3]==64&&expected.rgba[3]==255);
    for(std::size_t i=0;i<raw.rgba.size();++i)if(i%4!=3)require(raw.rgba[i]==expected.rgba[i]);
    require(renderer.render_prepared(prepared).rgba==expected.rgba); // Policy belongs to each immutable frame.
    require(renderer.render_prepared(rawPrepared).rgba==raw.rgba);

    bool encoded=false,validated=false;
    const SceneGpuPass hook{[&](id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth){
        require((depth.usage&MTLTextureUsageShaderRead)!=0);require(command.status==MTLCommandBufferStatusNotEnqueued);
        encoded=true;
        auto descriptor=[MTLRenderPassDescriptor renderPassDescriptor];descriptor.colorAttachments[0].texture=color;
        descriptor.colorAttachments[0].loadAction=MTLLoadActionClear;descriptor.colorAttachments[0].storeAction=MTLStoreActionStore;
        descriptor.colorAttachments[0].clearColor=MTLClearColorMake(8/255.,16/255.,24/255.,128/255.);
        auto encoder=[command renderCommandEncoderWithDescriptor:descriptor];[encoder endEncoding];
        return [&,command]{require(encoded&&command.status==MTLCommandBufferStatusCompleted);validated=true;};
    }};
    const auto hooked=renderer.render_prepared(prepared,&pass,&hook);
    require(encoded&&validated&&center(hooked)==std::array<unsigned,3>{135,143,151});
    require(hooked.rgba[0]==8&&hooked.rgba[1]==16&&hooked.rgba[2]==24&&hooked.rgba[3]==128);
    require(hooked.rgba[(112*640+320)*4+3]==127); // UI follows hook; hook follows scene.
    const SceneGpuPass failed{[](id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>){return []{throw std::runtime_error("Synthetic filter GPU validation failure");};}};
    bool rejected=false;try{renderer.render_prepared(prepared,nullptr,&failed);}catch(const std::runtime_error& e){rejected=std::string(e.what())=="Synthetic filter GPU validation failure";}require(rejected);
    SceneRenderer other(dir/"a.xmp");rejected=false;
    try{other.render_prepared(prepared);}catch(const std::runtime_error&){rejected=true;}require(rejected);
    rejected=false;try{renderer.render_prepared({});}catch(const std::runtime_error&){rejected=true;}require(rejected);
    const SceneGpuPass empty_validator{[](id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>){return std::function<void()>{};}};
    rejected=false;try{renderer.render_prepared(prepared,nullptr,&empty_validator);}catch(const std::runtime_error&){rejected=true;}require(rejected);
    require(renderer.render_prepared(prepared).rgba==expected.rgba);
    const auto empty=renderer.prepare_empty(projection.frame());
    const auto cleared=renderer.render_prepared(empty,nullptr,nullptr,std::array<std::uint8_t,4>{8,16,24,0});
    require(center(cleared)==std::array<unsigned,3>{8,16,24}&&cleared.rgba[3]==64);
    require(renderer.shadow_submissions().empty());
    geometry=list();geometry.commands.push_back(UiDrawRegister{0x47,0x5000d});geometry.commands.push_back(quad({129,129,129,127},0));
    const auto on_empty=renderer.render_prepared(empty,&pass,nullptr,std::array<std::uint8_t,4>{8,16,24,0});
    require(center(on_empty)==std::array<unsigned,3>{135,143,151}); // Z0 passes freshly cleared depth.
    const auto depth_override=renderer.render_prepared(empty,&pass,nullptr,std::array<std::uint8_t,4>{8,16,24,0},0);
    require(center(depth_override)==std::array<unsigned,3>{8,16,24}&&depth_override.rgba[3]==64);
    rejected=false;try{renderer.render_prepared(empty,nullptr,nullptr,std::nullopt,-1);}catch(const std::runtime_error&){rejected=true;}require(rejected);
    require(renderer.render_prepared(empty,&pass,&hook).rgba==hooked.rgba); // Empty scene still runs hook before UI.

    geometry=list();geometry.commands.push_back(UiDrawRegister{0x47,0x5000d});
    geometry.commands.push_back(quad({255,255,255,128},0));
    require(render(&pass).rgba==expected.rgba);
    geometry=list();geometry.commands.push_back(quad({128,128,128,0},100));
    geometry.commands.push_back(UiDrawRegister{0x47,0x5000d});geometry.commands.push_back(quad({255,255,255,128},50));
    const auto depth=render(&pass);require(center(depth)==before&&depth.rgba[(112*640+320)*4+3]==0);
    auto gpu=upload_ui_gs_texture(MTLCreateSystemDefaultDevice(),Texture{1,1,0,0,{128,128,128,255}});
    UiGeometryTexture binding{UiGeometryTexture::Kind::Window,0,0x20000006a8000000ull,{},std::make_shared<int>(1)};
    resolver=[&](const auto&){return UiMetalTexture{gpu,binding.window};};
    geometry=list();geometry.commands.push_back(binding);auto bad=quad({128,128,128,128},100);bad.prim=0x5d;bad.texture=binding;
    for(auto& v:bad.quad.vertices)v.stq={1.f/1024.f,.5f/1024.f,1};geometry.commands.push_back(bad);
    bool caught=false;try{render(&pass);}catch(const std::runtime_error& e){caught=std::string(e.what()).find("UI unsupported sampled texture footprint:")==0;}
    require(caught);
    require(render().rgba==expected.rgba);
    caught=false;try{renderer.render(identity,clip,nullptr,{},1024,{},nullptr,1,&pass);}catch(const std::runtime_error& e){caught=std::string(e.what())=="Scene UI requires GS field projection depth";}
    require(caught);
}
static void submitted_frame_tickets() {
    FieldProjection projection;projection.load_stage(stage({1,1,1,1},{.25f,.25f,.25f,1},false));
    projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    SceneRenderer::SubmittedFrame empty_ticket;
    require(!empty_ticket.ready());
    bool caught=false;try{(void)empty_ticket.wait();}catch(const std::runtime_error& e){caught=std::string(e.what())=="Empty submitted scene frame";}require(caught);

    SceneRenderer renderer;
    const auto prepared=renderer.prepare_empty(projection.frame());
    unsigned validation_a=0,validation_b=0;
    const SceneGpuPass pass_a{[&](id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>){return [&]{++validation_a;};}};
    const SceneGpuPass pass_b{[&](id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>){return [&]{++validation_b;};}};
    auto red=renderer.submit_prepared(prepared,nullptr,&pass_a,std::array<std::uint8_t,4>{20,30,40,200});
    auto blue=renderer.submit_prepared(prepared,nullptr,&pass_b,std::array<std::uint8_t,4>{3,7,11,190});
    require(validation_a==0&&validation_b==0);
    auto blue_copy=blue;
    const auto blue_image=blue_copy.readback();
    require(validation_b==1&&validation_a==0);
    require(blue_image.rgba[0]==3&&blue_image.rgba[1]==7&&blue_image.rgba[2]==11&&blue_image.rgba[3]==64);
    const auto red_image=red.wait();
    require(validation_a==1&&red_image.rgba[0]==20&&red_image.rgba[1]==30&&red_image.rgba[2]==40&&red_image.rgba[3]==64);
    require(blue.readback().rgba==blue_image.rgba&&red.readback().rgba==red_image.rgba);
    require(validation_a==1&&validation_b==1);

    unsigned failing_validation=0;
    const SceneGpuPass failing{[&](id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>){
        return [&]{++failing_validation;throw std::runtime_error("Ticket completion validation failed");};
    }};
    const auto failed=renderer.submit_prepared(prepared,nullptr,&failing);
    (void)failed.ready();
    require(failing_validation==0);
    for(unsigned i=0;i<2;++i) {
        caught=false;try{(void)failed.readback();}
        catch(const std::runtime_error& e){caught=std::string(e.what())=="Ticket completion validation failed";}
        require(caught);
    }
    require(failing_validation==1);

    SceneRenderer::SubmittedFrame detached;
    {
        SceneRenderer temporary;
        const auto temporary_frame=temporary.prepare_empty(projection.frame());
        detached=temporary.submit_prepared(temporary_frame,nullptr,nullptr,std::array<std::uint8_t,4>{41,43,47,211});
    }
    const auto detached_image=detached.readback();
    require(detached_image.rgba[0]==41&&detached_image.rgba[1]==43&&detached_image.rgba[2]==47&&detached_image.rgba[3]==64);

    SceneRenderer::SubmittedFrame ui_ticket;
    std::weak_ptr<int> weak_owner;
    bool ui_rejected=false;
    {
        auto owner=std::make_shared<int>(7);weak_owner=owner;
        const auto gpu=upload_ui_gs_texture(MTLCreateSystemDefaultDevice(),Texture{1,1,0,0,{128,128,128,255}});
        UiGeometryTexture binding{UiGeometryTexture::Kind::Window,0,0x20000006a8000000ull,{},owner};
        UiGeometryList geometry;
        geometry.commands={UiDrawControl::Begin,UiDrawRegister{0x49,0},UiDrawRegister{0x47,0x30003},
            UiDrawRegister{0x3b,0x100008080ull},UiDrawRegister{0x4a,0},UiDrawRegister{0x42,0x48},
            UiDrawRegister{0x40,(223ull<<48)|(639ull<<16)},UiDrawRegister{0x14,0x60},UiDrawRegister{8,5},
            UiDrawCurrentFramebuffer{},binding};
        UiResolvedPrimitive primitive{};primitive.prim=0x5d;primitive.quad.emitted=true;primitive.texture=binding;
        constexpr std::array<std::array<unsigned,2>,4> xy={{{300,100},{340,100},{340,124},{300,124}}};
        for(unsigned i=0;i<4;++i) {
            auto& vertex=primitive.quad.vertices[i];
            vertex.xyzf={0x6c00+xy[i][0]*16,0x7900+xy[i][1]*16,100u<<4,0};
            vertex.stq={2,2,1};vertex.rgba={128,128,128,128};
        }
        geometry.commands.push_back(primitive);
        const UiMetalBaseline baseline{0x6c00,0x7900,true,0,1,0,1};
        UiMetalTextureResolver resolver=[gpu,owner](const UiGeometryTexture&){return UiMetalTexture{gpu,owner};};
        const SceneUiPass ui{geometry,baseline,resolver};
        ui_ticket=renderer.submit_prepared(prepared,&ui);
    }
    require(!weak_owner.expired());
    try{(void)ui_ticket.readback();}
    catch(const std::runtime_error& e){ui_rejected=std::string(e.what()).find("UI unsupported sampled texture footprint:")==0;}
    require(ui_rejected);
    try{(void)ui_ticket.wait();}
    catch(const std::runtime_error& e){ui_rejected=ui_rejected&&std::string(e.what()).find("UI unsupported sampled texture footprint:")==0;}
    require(ui_rejected&&!weak_owner.expired());
    ui_ticket={};require(weak_owner.expired());
}
static void retired_instances(const fs::path& dir) {
    const auto check=[](bool valid,const char* reason,const std::source_location at=std::source_location::current()) {
        if(!valid)throw std::runtime_error(std::string("Scene retirement: ")+reason+" at line "+std::to_string(at.line()));
    };
    const std::array<Surface,1> surfaces{{{{.25f,.5f,.75f,1}}}};
    sample(dir,surfaces,true,true,true);
    auto objects=std::make_unique<MapObjects>(dir/"a.xmp");
    for(const auto& b:objects->bindings()) {
        b.actor->animation().set_channel(4,{0,0,0,1});b.actor->animation().set_channel(5,{0,0,0,1});b.actor->evaluate();
    }
    auto actor=std::make_unique<FieldActor>(read_xmd(model(surfaces,true,true,false)));
    actor->render_flags=0x200080;actor->evaluate();
    SceneActor binding{actor.get(),dir/"m.xmd",-1,SceneActorDescriptor{0x200080,false}};
    SceneRenderer renderer(dir/"a.xmp",std::span(&binding,1),objects.get());renderer.set_clear_rgb({14,18,24});
    FieldProjection projection;projection.load_stage(stage({1,1,1,1},{.25f,.25f,.25f,1},false));
    projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    const Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    const auto clip=field_clip_projection(projection.frame().ordinary);
    auto prepare=[&]{return renderer.prepare(identity,clip,nullptr,{},1024,{},&projection.frame(),1,false);};
    const auto pending=prepare();const auto expected=renderer.render_prepared(pending);
    check(renderer.placements()==1&&renderer.actors()==1,"fixture must bind both map and actor inputs");
    const auto texdir=dir/"effects";fs::create_directory(texdir);
    Data xim(32+64*64*4);put(xim,24,64);put(xim,28,64);
    for(unsigned i=32;i<xim.size();i+=4){xim[i]=80;xim[i+1]=160;xim[i+2]=240;xim[i+3]=128;}
    for(const auto name:effect_texture_names)write(texdir/name,xim);
    renderer.load_effect_textures(texdir);const auto sizes=renderer.effect_texture_sizes();
    EffectDefinition vanish;vanish.id=20;vanish.duration=30;vanish.parameters={.6f,.3f,.6f,0};vanish.elements.resize(9);
    for(unsigned i=0;i<9;++i) {
        auto& part=vanish.elements[i];part.id=i+1;part.shape=31;part.flags=i?0x601:0x401;
        part.primitive_flags=0x41;part.channels=13;part.program=i?0x1104c114:0x4c114;
        part.texture=17;part.change_time=1.1f;part.values[0]={.5f,.5f,.5f,.8f};
        part.values[2]={0,0,0,1};part.values[3]={1,1,1,1};
    }
    MapEffects effects;effects[0]=std::make_unique<MapEffect>(vanish,Float4{0,0,10,1},Float4{},Float4{8,8,8,1},sizes,[]{return 0u;},EffectStart::FirstFrame);
    renderer.bind_map_effects(&effects);
    renderer.retire_instances();renderer.retire_instances();
    check(renderer.actors()==0&&renderer.placements()==0&&renderer.omitted_effects()==0&&renderer.effect_texture_sizes()==sizes,
          "retirement must clear counts and preserve effect texture dimensions");
    actor.reset();objects.reset();
    check(renderer.render_prepared(pending).rgba==expected.rgba,"prepared frame changed after retirement or later preparation");
    const auto withEffects=renderer.render_prepared(prepare());
    check(renderer.shadow_submissions().empty(),"retired actors still submitted shadows");
    renderer.bind_map_effects(nullptr);
    const auto empty=renderer.render_prepared(prepare());
    check(center(empty)==std::array<unsigned,3>{14,18,24}&&empty.rgba[3]==64,"empty range lost clear color or raw alpha64");
    check(withEffects.rgba!=empty.rgba,"fixture must expose a visible textured effect after retirement");
    EffectSubmissions submissions{{0,*effects[0]}};renderer.bind_effect_submissions(&submissions);
    check(renderer.render_prepared(prepare()).rgba==withEffects.rgba,"effect-only preparation changed after rebinding or retirement");
    renderer.retire_instances();check(renderer.render_prepared(prepare()).rgba==withEffects.rgba,"effect-only preparation changed after rebinding or retirement");
    check(renderer.render_prepared(pending).rgba==expected.rgba,"prepared frame changed after retirement or later preparation");
    // Different model texture prefix must not change effect texture resolution.
    write(dir/"a.xmp",map(false));
    SceneRenderer other(dir/"a.xmp");other.set_clear_rgb({14,18,24});other.retire_instances();
    other.load_effect_textures(texdir);other.bind_effect_submissions(&submissions);
    const auto alternate=other.prepare(identity,clip,nullptr,{},1024,{},&projection.frame(),1,false);
    check(other.render_prepared(alternate).rgba==withEffects.rgba,"different model texture prefixes shifted effect texture indices");
    auto rebound=std::make_unique<FieldActor>(read_xmd(model(surfaces,true,true,false)));
    rebound->render_flags=0x200080;rebound->evaluate();
    renderer.add_actor({rebound.get(),dir/"m.xmd",-1,SceneActorDescriptor{0x200080,false}});
    check(renderer.placements()==0&&renderer.actors()==1&&renderer.effect_texture_sizes()==sizes,
          "appending after retirement broke counts or effect texture indices");
    renderer.render_prepared(prepare());
    renderer.retire_instances();rebound.reset();
    check(renderer.render_prepared(prepare()).rgba==withEffects.rgba,"effect-only preparation changed after rebinding or retirement");
    check(renderer.render_prepared(pending).rgba==expected.rgba,"prepared frame changed after retirement or later preparation");
}
int main(int argc,const char** argv){@autoreleasepool {try {
    if(argc>2)throw std::runtime_error("Usage: scene-map-material-tests [FRESH_OUTPUT_DIRECTORY]");
    const fs::path out=argc==2?fs::path(argv[1]):fs::temp_directory_path()/("okage-scene-ui-"+std::string([NSUUID UUID].UUIDString.UTF8String));
    require(!fs::exists(out));require(fs::create_directories(out));
    const Float4 base{.5f,1,.25f,1},ambient{.125f,.25f,.375f,1};
    std::array<Surface,1> one{{{{.75f,.75f,.75f,1}}}};
    const auto untextured=sample(out/"untextured",one,false,false,false,base,ambient);
    const auto selected=sample(out/"selected-tme-off",one,true,false,false,base,ambient);
    require(untextured.rgba==selected.rgba);require(center(selected)==std::array<unsigned,3>{95,191,47});
    const auto back=sample(out/"back-winding",one,true,false,false,base,ambient,false,true);
    require(center(back)==std::array<unsigned,3>{14,18,24});
    require(back.rgba!=untextured.rgba);
    const auto textured=sample(out/"textured",one,true,true,false,base,ambient);
    require(center(textured)==std::array<unsigned,3>{23,95,35});
    one[0].normal=true;
    const auto normal=sample(out/"normal-static",one,false,false,false,base,ambient);
    const auto animated=sample(out/"normal-animated",one,false,false,true,base,ambient);
    require(normal.rgba==animated.rgba);require(center(normal)==std::array<unsigned,3>{23,47,71});
    require(sample(out/"normal-zero-directions",one,false,false,true,base,ambient,true).rgba==normal.rgba);
    one[0].bypass=true;
    require(sample(out/"normal-bypass",one,false,false,true,base,ambient).rgba==untextured.rgba);
    const Surface red{{1,0,0,1},.5f},green{{0,1,0,1}},blue{{0,0,1,1},.1f},yellow{{1,1,0,1}};
    const std::array interleaved{red,green,blue,yellow},ordered{green,yellow,red,blue};
    const auto a=sample(out/"interleaved",interleaved,false,false,false);
    const auto b=sample(out/"ordered",ordered,false,false,false);
    require(a.rgba==b.rgba);
    const auto c=center(a);require(c[0]>=140&&c[0]<=141&&c[1]==13&&c[2]>=114&&c[2]<=115);
    one[0].normal=false;one[0].bypass=false;
    battle_actor_sample(out/"battle-base",one,0x200080,base,ambient);
    battle_actor_sample(out/"battle-static",one,0x200080,base,ambient,false,true);
    one[0].normal=true;
    battle_actor_sample(out/"battle-normal",one,0x2000a0,base,ambient);
    battle_actor_sample(out/"battle-textured",one,0x200080,base,ambient,true);
    battle_actor_sample(out/"battle-static-textured",one,0x200080,base,ambient,true,true);
    one[0].bypass=true;
    battle_actor_sample(out/"battle-bypass",one,0x2000e0,base,ambient);
    battle_actor_sample(out/"battle-translucent",interleaved,0x200080,base,ambient);
    // Public actor descriptor and loader-mode0 rendering select the material's
    // GS blend register, not a special JAM ambient or field-alias path.
    const std::array<Surface,1> additive{{{{.25f,.5f,.75f,1},0,false,false,1}}};
    const std::array<Surface,1> subtractive{{{{.25f,.5f,.75f,1},0,false,false,2}}};
    const auto added=battle_actor_sample(out/"battle-additive",additive,0x2000a0,{1,1,1,1},ambient);
    const auto subtracted=battle_actor_sample(out/"battle-subtractive",subtractive,0x2000a0,{1,1,1,1},ambient);
    require(center(added)==std::array<unsigned,3>{77,145,215});
    require(center(subtracted)==std::array<unsigned,3>{0,0,0});
    const std::array<Surface,1> translucentAdd{{{{.25f,.5f,.75f,1},.5f,false,false,1}}};
    const auto translucentAdded=battle_actor_sample(out/"battle-additive-translucent",translucentAdd,0x2000a0,{1,1,1,1},ambient);
    const auto blended=center(translucentAdded);
    require(blended[0]>=45&&blended[0]<=46&&blended[1]>=81&&blended[1]<=82&&blended[2]>=119&&blended[2]<=120);
    ui_bridge(out/"ui-bridge");
    submitted_frame_tickets();
    retired_instances(out/"retired-instances");
    if(argc==1)fs::remove_all(out);
    std::cout<<"Scene retirement: retained snapshots, detached owners, empty range and effect texture indices passed\n";
    std::cout<<"Scene UI pass: ordered overlay, shared depth, raw alpha, completed validation and recovery passed\n";
    std::cout<<"Scene map material: TME-off selected state, exact GS-unit centers, animated-map stage ambient, bypass and whole-list pass ordering passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
