// Synthetic source assets through public SceneRenderer/MapObjects APIs, offscreen.
#import <Foundation/Foundation.h>
#include "okage/scene_renderer.hpp"
#include "okage/map_objects.hpp"
#include "ui_metal.hpp"
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace okage;
namespace fs=std::filesystem;
using Data=std::vector<std::uint8_t>;
static void require(bool v,unsigned at=__builtin_LINE()) {
    if(!v)throw std::runtime_error("Scene map material assertion line "+std::to_string(at));
}
static void put(Data& b,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
static void word(Data& b,std::uint32_t v){const auto at=b.size();b.resize(at+4);put(b,at,v);}
static void floats(Data& b,const Float4& v){for(float f:v)word(b,__builtin_bit_cast(unsigned, (f)));}
static void write(const fs::path& p,const Data& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("Fixture write");}
struct Surface {Float4 color;float transparency=0;bool normal=false,bypass=false;};
static Data model(std::span<const Surface> surfaces,bool selected,bool textured,bool reverse) {
    // One identity bone/node; source color and normal independent per surface.
    Data prefix(28);put(prefix,0,24);put(prefix,4,12);put(prefix,12,1);
    std::memcpy(prefix.data()+16,"t.xim",6);put(prefix,24,1);
    Data b(20+52+8+4+16);put(b,0,0x15);put(b,8,20);put(b,12,72);put(b,16,80);
    put(b,20,1);for(unsigned i=0;i<3;++i)put(b,24+4*i,0x3f800000);
    put(b,72,1);put(b,76,0xffff0000);put(b,80,1);
    word(b,0x0200001c);word(b,0xffff0000);word(b,0x00010001);floats(b,{0,0,1,1});
    word(b,0x03000000|unsigned(4+32*surfaces.size()));
    for(const auto& s:surfaces){floats(b,{0,0,0,0});word(b,0);word(b,__builtin_bit_cast(unsigned, (s.transparency)));word(b,0);word(b,s.bypass?1:0);}
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
        const Float4& base,const Float4& ambient,bool textured=false) {
    const auto expected=sample(dir,surfaces,true,textured,false,base,ambient);
    auto hidden=map(false);put(hidden,64+12,0x10000);write(dir/"a.xmp",hidden);
    Data motion(56);put(motion,0,0x544d58);put(motion,4,24);put(motion,16,0xffffffff);
    FieldActor actor(read_xmd(model(surfaces,true,textured,false)),motion,{});
    actor.animation().set_channel(4,{0,0,0,1});actor.animation().set_channel(5,{0,0,0,1});
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
    actor.visible=true;actor.render_flags=flags;
    require(render().rgba!=image.rgba);
    require(renderer.render_prepared(snapshot).rgba==image.rgba);
    actor.animation().set_channel(1,{0,0,0,1});actor.evaluate();renderer.set_clear_rgb({14,18,24});renderer.set_sort_depth_shift(16);
    actor.animation().set_channel(3,{.9f,.8f,.7f,.6f});actor.evaluate();require(render().rgba==image.rgba);
    actor.render_flags=0x300080;bool caught=false;try{render();}catch(const std::runtime_error& e){caught=std::string(e.what())=="Explicit actor flags changed after binding";}require(caught);
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
    one[0].normal=true;
    battle_actor_sample(out/"battle-normal",one,0x2000a0,base,ambient);
    battle_actor_sample(out/"battle-textured",one,0x200080,base,ambient,true);
    one[0].bypass=true;
    battle_actor_sample(out/"battle-bypass",one,0x2000e0,base,ambient);
    battle_actor_sample(out/"battle-translucent",interleaved,0x200080,base,ambient);
    ui_bridge(out/"ui-bridge");
    if(argc==1)fs::remove_all(out);
    std::cout<<"Scene UI pass: ordered overlay, shared depth, raw alpha, completed validation and recovery passed\n";
    std::cout<<"Scene map material: TME-off selected state, exact GS-unit centers, animated-map stage ambient, bypass and whole-list pass ordering passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
