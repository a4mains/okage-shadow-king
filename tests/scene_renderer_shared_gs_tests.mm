// Generated owned assets through the real offscreen SceneRenderer frame API.
#import <Foundation/Foundation.h>
#include "okage/scene_renderer.hpp"
#include "okage/field_projection.hpp"
#include "okage/field.hpp"
#include <cmath>
#include "okage/gs_texture_address.hpp"
#include "okage/render.hpp"
#include "okage/texture_transfer.hpp"
#include <bit>
#include <cstring>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace okage;
using Data=std::vector<std::uint8_t>;
static void require(bool value,const std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("Shared GS scene assertion line "+std::to_string(at.line()));
}
static void put(Data& b,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
static void word(Data& b,std::uint32_t v){const auto at=b.size();b.resize(at+4);put(b,at,v);}
static void floats(Data& b,const Float4& v){for(float f:v)word(b,std::bit_cast<unsigned>(f));}
struct Surface {Float4 color;float transparency=0;bool normal=false,bypass=false;unsigned blend=0,extra_flags=0;float environment_coefficient=0;};
static Data model(std::span<const Surface> surfaces,bool selected,bool textured,bool reverse,const Float4& normal={0,0,1,0}) {
    // One identity bone/node; source color and normal independent per surface.
    Data prefix(28);put(prefix,0,24);put(prefix,4,12);put(prefix,12,1);
    std::memcpy(prefix.data()+16,"t.xim",6);put(prefix,24,1);
    Data b(20+52+8+4+16);put(b,0,0x15);put(b,8,20);put(b,12,72);put(b,16,80);
    put(b,20,1);for(unsigned i=0;i<3;++i)put(b,24+4*i,0x3f800000);
    put(b,72,1);put(b,76,0xffff0000);put(b,80,1);
    word(b,0x0200001c);word(b,0xffff0000);word(b,0x00010001);floats(b,{0,0,1,1});
    word(b,0x03000000|unsigned(4+32*surfaces.size()));
    for(const auto& s:surfaces){floats(b,{0,0,0,s.environment_coefficient});word(b,0);word(b,std::bit_cast<unsigned>(s.transparency));word(b,s.blend);word(b,s.extra_flags|(s.bypass?1:0));}
    word(b,0x04000008);word(b,0);
    word(b,0x0500004c);word(b,1);word(b,4<<16);
    for(const auto& p:std::array{Float4{-2,-2,10,1},Float4{2,-2,10,1},Float4{2,2,10,1},Float4{-2,2,10,1}})floats(b,p);
    word(b,0x0600001c);word(b,1);word(b,1<<16);floats(b,normal);
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
static Data stage(const Float4& base,const Float4& ambient,bool zeroDirections) {
    Data b(32);put(b,0,0x475453);put(b,8,32);put(b,12,zeroDirections?9:6);
    word(b,0x44);floats(b,{1,1,128,1024});
    word(b,0x40);floats(b,base);word(b,0x41);floats(b,ambient);
    for(unsigned i=1;i<=3;++i){word(b,i);floats(b,{0,0,0,0});}
    if(zeroDirections)for(unsigned i=0x11;i<=0x13;++i){word(b,i);floats(b,{0,0,0,0});}
    return b;
}

static ModelFile quad(float x=0,float y=0,TextureState state={0,0,1,1,{0,0,1024,1024}}) {
    const std::array<Surface,1> surfaces{{{{128.f/255,128.f/255,128.f/255,1},0}}};
    auto result=read_xmd(model(surfaces,true,true,false));
    auto& block=result.models.at(0).blocks.at(0);block.textures.at(0)=state;
    for(auto& attribute:block.attributes)if(attribute.kind==8)for(auto& uv:attribute.values)uv={x/1024,y/1024,0,0};
    return result;
}
static Data xim(unsigned psm,unsigned width,unsigned height,std::array<std::uint8_t,4> color,unsigned index=5) {
    const bool indexed=psm>=16;const unsigned entries=(psm==20||psm==36||psm==44)?16:256;
    const unsigned bits=psm==0?32:psm==1?24:(psm==19||psm==27)?8:4;
    const unsigned block=indexed?32+entries*4:16,payload=block+16;
    Data bytes(payload+width*height*bits/8);put(bytes,0,psm<<20);
    if(indexed) {
        put(bytes,16,16+entries*4);put(bytes,24,1);put(bytes,28,entries);
        for(unsigned i=0;i<entries;++i)for(unsigned b=0;b<4;++b)bytes[32+i*4+b]=color[b];
        std::fill(bytes.begin()+payload,bytes.end(),bits==4?index|(index<<4):index);
    } else for(unsigned i=payload;i<bytes.size();++i)bytes[i]=color[(i-payload)%(bits/8)];
    put(bytes,block+8,height);put(bytes,block+12,width);return bytes;
}
struct Owned {
    Ps2TextureMemory memory;
    TextureHandleOwner handles{memory};
    ResourceTextureOwner resources{handles};
    Owned(){memory.initialize_empty_allocator();}
    ~Owned(){handles.retire_all();}
    ResourceTextures load(const ModelFile& model,const Data& bytes) {
        return resources.load_xmd(model.textures,[&](std::string& name)->std::optional<Data>{require(name==model.textures.front()+".xim");return bytes;});
    }
};
struct Scene {
    FieldActor actor;
    SceneRenderer renderer;
    FieldProjection projection;
    explicit Scene(ModelFile model,const std::vector<ResourceTextureSlot>& slots,Owned& owner,
                   std::shared_ptr<TextureBindings> bindings={},TextureAnimation animation={},bool initial_variant=false)
        :actor(make_actor(std::move(model),std::move(bindings),std::move(animation),initial_variant)) {
        if(actor.has_motion()) {actor.animation().set_channel(4,{0,0,0,1});actor.animation().set_channel(5,{0,0,0,1});actor.evaluate();}
        SceneActor binding{&actor,"/nonexistent/shared-gs-fixture/m.xmd",-1,SceneActorDescriptor{0,false}};
        binding.texture_slots=slots;binding.textures_owned=true;binding.shadow_owned=true;
        renderer.set_shared_texture_owner(owner.handles);renderer.add_actor(binding);renderer.set_clear_rgb({14,18,24});
        projection.load_stage(stage({1,1,1,1},{.25f,.25f,.25f,1},false));
        projection.begin_field();projection.begin_effects();projection.begin_deferred();projection.end_effects();
    }
    static FieldActor make_actor(ModelFile model,std::shared_ptr<TextureBindings> bindings,TextureAnimation animation,bool initial_variant) {
        if(!bindings)return FieldActor(std::move(model));
        Data motion(56);put(motion,0,0x544d58);put(motion,4,24);put(motion,16,0xffffffff);
        if(initial_variant)put(motion,32,0x00000134);
        return FieldActor(std::move(model),motion,std::move(animation),false,false,std::move(bindings));
    }
    SceneRenderer::PreparedFrame prepare() {
        static const Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
        return renderer.prepare(identity,field_clip_projection(projection.frame().ordinary),nullptr,{},1024,{},&projection.frame());
    }
    Texture render(){return renderer.render_prepared(prepare());}
    std::string observation()const {std::ostringstream out;renderer.write_texture_sampling_observation(out);return out.str();}
};
static std::array<unsigned,4> center(const Texture& texture) {
    const unsigned at=(112*640+320)*4;return {texture.rgba[at],texture.rgba[at+1],texture.rgba[at+2],texture.rgba[at+3]};
}
static void physical_snapshot_lifetime() {
    auto owner=std::make_unique<Owned>();const auto model=quad();auto resource=owner->load(model,xim(0,8,8,{20,40,60,128}));
    auto scene=std::make_unique<Scene>(model,resource.slots,*owner);
    const auto allocation=*owner->memory.allocation(resource.slots.at(0).image->id);
    const auto address=gs_texel_address(0,allocation.base_block,allocation.buffer_width_64,0,0)->byte_address;
    const auto old=scene->prepare();const auto expected=scene->renderer.render_prepared(old);
    require(center(expected)==std::array<unsigned,4>{20,40,60,255});
    auto held=scene->renderer.submit_prepared(old);auto held_copy=held;
    owner->memory.write_bytes(address,Data{90,100,110,128});
    const auto newer=scene->prepare();const auto newer_image=scene->renderer.render_prepared(newer);
    require(center(newer_image)==std::array<unsigned,4>{90,100,110,255});
    require(scene->renderer.render_prepared(old).rgba==expected.rgba);
    owner->resources.retire_xmd(resource);require(!owner->memory.allocation(allocation.id));
    require(owner->memory.observe_byte(address).value==90); // Retirement preserves physical bytes.
    require(scene->renderer.render_prepared(old).rgba==expected.rgba);
    const auto reused=owner->memory.allocate(0,GsPixelStorage::Psmct32,8,8);
    require(reused&&reused->base_block==allocation.base_block&&reused->serial!=allocation.serial);
    owner->memory.write_bytes(address,Data{150,160,170,128});
    require(scene->renderer.render_prepared(old).rgba==expected.rgba);
    require(scene->renderer.render_prepared(newer).rgba==newer_image.rgba);
    scene.reset();owner.reset();
    require(held.wait().rgba==expected.rgba&&held_copy.readback().rgba==expected.rgba);
}
static std::string rejection(SceneRenderer::SubmittedFrame frame) {
    try{(void)frame.wait();}catch(const std::runtime_error& error){return error.what();}
    throw std::runtime_error("Unknown sample did not reject completion");
}
static void unknown_positive_tap() {
    Owned owner;auto model=quad();auto resource=owner.load(model,xim(0,8,8,{20,40,60,128}));
    const auto image=*owner.memory.allocation(resource.slots.at(0).image->id);
    const auto origin=gs_texel_address(0,image.base_block,image.buffer_width_64,0,0)->byte_address;
    owner.memory.invalidate_owned_nibbles(image.id);owner.memory.write_bytes(origin,Data{20,40,60,128});
    Scene zero(model,resource.slots,owner);
    require(center(zero.render())==std::array<unsigned,4>{20,40,60,255}); // Three zero taps remain unknown.
    Scene positive(quad(.25f,0),resource.slots,owner);
    auto failed=positive.renderer.submit_prepared(positive.prepare());auto copy=failed;
    const auto unknown=gs_texel_address(0,image.base_block,image.buffer_width_64,1,0)->byte_address;
    std::ostringstream expected;expected<<"Unknown shared GS texture nibble address=0x"<<std::hex<<unknown
        <<std::dec<<" nibble=0 owner="<<image.id;
    require(rejection(failed)==expected.str()&&rejection(copy)==expected.str());
    // Filling only the low nibble advances the exact failure to the high half.
    owner.memory.write_nibble(unknown,0,7);
    auto half=positive.renderer.submit_prepared(positive.prepare());
    std::string high=expected.str();high.replace(high.find("nibble=0"),8,"nibble=1");
    require(rejection(half)==high);
    owner.memory.write_bytes(unknown,Data{40,60,80,128});
    require(center(positive.render())==std::array<unsigned,4>{25,45,65,255});
    require(rejection(failed)==expected.str()); // Failed ticket is immutable too.
    owner.resources.retire_xmd(resource);
}

static std::uint64_t observed_register(const std::string& observation,const char* name,unsigned index=0) {
    const std::string key=std::string("\"")+name+"\":\"0x";std::size_t at=0;
    for(unsigned i=0;i<=index;++i){at=observation.find(key,at);require(at!=std::string::npos);at+=key.size();}
    return std::stoull(observation.substr(at),nullptr,16);
}
static std::uint64_t source_tex0(const Owned& owner,int handle,int palette) {
    const auto image=*owner.memory.allocation(owner.handles.image(handle)->id);
    const auto clut=owner.handles.palette(handle,palette);
    const auto cbp=clut?owner.memory.allocation(clut->id)->base_block:0;
    return 0x20000006a8000000ull | image.base_block | (std::uint64_t(image.buffer_width_64)<<14) |
        (std::uint64_t(image.psm)<<20) | (std::uint64_t(cbp)<<37);
}
static void saved_original_and_variant() {
    Owned owner;TextureState state{0,1,0,1,{0,0,1,1}};auto model=quad(0,0,state);
    auto resource=owner.load(model,xim(19,8,8,{20,40,60,128}));
    const int original_handle=resource.slots.front().handle;
    Data palette(256*4);for(unsigned i=0;i<256;++i){palette[i*4]=70;palette[i*4+1]=80;palette[i*4+2]=90;palette[i*4+3]=128;}
    bool uploaded=false;const auto alternate=owner.handles.create_palette(256,palette,uploaded);require(alternate&&uploaded);
    require(owner.handles.bind_palette(original_handle,1,alternate)==PaletteBindResult::Bound);
    const Data sidecar{'0','\n','=','v','\n'};
    require(owner.resources.load_xta(resource,std::make_shared<const Data>(sidecar),[&](std::string& name)->std::optional<Data>{
        require(name=="v");return xim(19,16,8,{110,120,130,128});}));
    auto slots=resource.slots;slots.insert(slots.end(),resource.xta_variants.begin(),resource.xta_variants.end());
    TextureAnimation animation{{0},{"v"}};auto bindings=std::make_shared<TextureBindings>(model,animation);
    Scene scene(model,slots,owner,bindings,animation);
    const auto expected_original=source_tex0(owner,original_handle,1);
    const auto expected_variant=source_tex0(owner,slots.at(1).handle,0);
    const auto clamp=texture_clamp(state,8,8);
    const auto original=scene.prepare();const auto original_image=scene.renderer.render_prepared(original);
    require(center(original_image)==std::array<unsigned,4>{70,80,90,255});
    require(observed_register(scene.observation(),"tex0")==expected_original);
    for(const auto name:{"tex1","clamp","texa"})require(observed_register(scene.observation(),name)==
        (std::string(name)=="tex1"?0x60:std::string(name)=="clamp"?clamp:0x8000008080ull));
    bindings->select(0,1);const auto variant=scene.prepare();
    require(observed_register(scene.observation(),"tex0")==expected_variant);
    require(observed_register(scene.observation(),"clamp")==clamp);
    require(observed_register(scene.observation(),"tex1")==0x60&&observed_register(scene.observation(),"texa")==0x8000008080ull);
    require(center(scene.renderer.render_prepared(variant))==std::array<unsigned,4>{110,120,130,255});
    std::fill(palette.begin(),palette.end(),50);const auto later=owner.handles.create_palette(256,palette,uploaded);require(later&&uploaded);
    require(owner.handles.bind_palette(original_handle,1,later)==PaletteBindResult::Bound);
    require(source_tex0(owner,original_handle,1)!=expected_original);
    scene.renderer.set_shared_texture_owner(owner.handles); // Must not recapture originals.
    bindings->select(0,0);const auto restored=scene.prepare();
    require(observed_register(scene.observation(),"tex0")==expected_original&&observed_register(scene.observation(),"clamp")==clamp);
    require(scene.renderer.render_prepared(restored).rgba==original_image.rgba);
    require(scene.renderer.render_prepared(original).rgba==original_image.rgba);
    owner.resources.retire_xmd(resource);owner.resources.retire_scene_images();
}

static void callback_time_source_packet() {
    Owned owner;const auto model=quad();auto resource=owner.load(model,xim(19,8,8,{20,40,60,128}));
    require(owner.resources.load_xta(resource,std::make_shared<const Data>(Data{'0','\n','=','v','\n'}),[&](std::string&)->std::optional<Data>{return xim(19,8,8,{100,110,120,128});}));
    auto slots=resource.slots;slots.insert(slots.end(),resource.xta_variants.begin(),resource.xta_variants.end());
    TextureAnimation animation{{0},{"v"}};auto packet=std::make_shared<TextureBindings>(model,animation);
    SceneModelAsset asset;asset.model=model;asset.animation=animation;asset.textures=slots;asset.packet_textures=packet;
    configure_scene_model_texture_source(asset,owner.handles);
    // Real actor entry0 opcode34 immediately selects variant1. Configuration
    // precedes construction, so the resolver runs at this exact callback.
    Scene scene(model,slots,owner,packet,animation,true);
    const auto old=scene.prepare();const auto old_image=scene.renderer.render_prepared(old);
    const auto original_variant=observed_register(scene.observation(),"tex0");
    require(center(old_image)==std::array<unsigned,4>{100,110,120,255});
    Data palette(256*4);for(unsigned i=0;i<palette.size();i+=4){palette[i]=150;palette[i+1]=160;palette[i+2]=170;palette[i+3]=128;}
    bool uploaded=false;const auto later=owner.handles.create_palette(256,palette,uploaded);require(later&&uploaded);
    require(owner.handles.bind_palette(slots[1].handle,0,later)==PaletteBindResult::Bound);
    const auto no_callback=scene.prepare();
    require(observed_register(scene.observation(),"tex0")==original_variant);
    require(scene.renderer.render_prepared(no_callback).rgba==old_image.rgba);
    packet->select(0,1);const auto repeated=scene.prepare(); // Same image, new callback.
    require(observed_register(scene.observation(),"tex0")!=original_variant);
    require(center(scene.renderer.render_prepared(repeated))==std::array<unsigned,4>{150,160,170,255});
    require(scene.renderer.render_prepared(old).rgba==old_image.rgba);
    configure_scene_model_texture_source(asset,owner.handles); // Same owner no-op.
    packet->select(0,0);require(center(scene.render())==std::array<unsigned,4>{20,40,60,255});
    // Shared cache instances consume the same register packet, not independent
    // per-renderer derivations. The second constructor has no initial callback.
    packet->select(0,1);
    Scene second(model,slots,owner,packet,animation);
    require(center(second.render())==std::array<unsigned,4>{150,160,170,255});
    require(second.render().rgba==scene.render().rgba);
    packet->restore();require(center(second.render())==std::array<unsigned,4>{20,40,60,255});
    auto missed=std::make_shared<TextureBindings>(model,animation);bool rejected=false;
    try{Scene late(model,slots,owner,missed,animation,true);}catch(const std::runtime_error& e){
        rejected=std::string(e.what())=="Source texture packet configured after selection/restore callback";
    }
    require(rejected);owner.resources.retire_xmd(resource);owner.resources.retire_scene_images();
}

static void direct_high_lane() {
    for(unsigned psm:{36u,44u}) {
        Owned owner;Data pixels(32,0x55);
        const int handle=owner.handles.create_image(static_cast<GsPixelStorage>(psm),8,8,pixels);require(handle>=0);
        Data palette(16*4);for(unsigned i=0;i<16;++i){palette[i*4]=i*5;palette[i*4+1]=i*5+20;palette[i*4+2]=i*5+40;palette[i*4+3]=128;}
        bool uploaded=false;const auto clut=owner.handles.create_palette(16,palette,uploaded);require(clut&&uploaded);
        require(owner.handles.bind_palette(handle,0,clut)==PaletteBindResult::Bound);
        auto slot=owner.resources.describe(handle);
        slot.bytes=std::make_shared<const Data>(xim(20,8,8,{25,45,65,128}));
        const auto image=*owner.memory.allocation(slot.image->id);
        const auto texel=*gs_texel_address(psm,image.base_block,image.buffer_width_64,0,0);
        require(!owner.memory.observe_nibble(texel.byte_address,(texel.shift/4)^1).known);
        Scene scene(quad(),{slot},owner);const auto old=scene.prepare();
        const auto baseline=scene.renderer.render_prepared(old);require(center(baseline)==std::array<unsigned,4>{25,45,65,255});
        require(((observed_register(scene.observation(),"tex0")>>20)&63)==psm);
        const unsigned other_psm=psm==36?44:36;
        const int other=owner.handles.create_image(static_cast<GsPixelStorage>(other_psm),8,8,Data(32,0x99));require(other>=0);
        const auto other_image=*owner.memory.allocation(owner.handles.image(other)->id);
        require(other_image.base_block==image.base_block&&other_image.id!=image.id);
        require(owner.memory.observe_nibble(texel.byte_address,texel.shift/4).allocation_owner==image.id);
        require(owner.memory.observe_nibble(texel.byte_address,(texel.shift/4)^1).allocation_owner==other_image.id);
        owner.memory.invalidate_owned_nibbles(other_image.id);require(scene.render().rgba==baseline.rgba);
        owner.memory.write_nibble(texel.byte_address,texel.shift/4,6);
        require(center(scene.render())==std::array<unsigned,4>{30,50,70,255});
        require(scene.renderer.render_prepared(old).rgba==baseline.rgba);
        owner.memory.invalidate_owned_nibbles(image.id);
        auto failure=scene.renderer.submit_prepared(scene.prepare());std::ostringstream expected;
        expected<<"Unknown shared GS texture nibble address=0x"<<std::hex<<texel.byte_address<<std::dec
            <<" nibble="<<texel.shift/4<<" owner="<<image.id;
        require(rejection(failure)==expected.str());
        require(scene.renderer.render_prepared(old).rgba==baseline.rgba);
    }
}
static void cross_owner_environment() {
    Owned owner;
    auto env_bytes=xim(19,64,64,{20,40,60,128},17);
    for(unsigned i=0;i<256;++i){env_bytes[32+i*4]=i;env_bytes[33+i*4]=i;env_bytes[34+i*4]=i;}
    const int env=owner.handles.load_xim(OwnedRawXim(env_bytes)).handle;
    const int neighbor=owner.handles.load_xim(OwnedRawXim(xim(19,128,128,{200,210,220,128},91))).handle;
    require(env>=0&&neighbor>=0);
    const auto image=*owner.memory.allocation(owner.handles.image(env)->id);
    const auto neighbor_image=*owner.memory.allocation(owner.handles.image(neighbor)->id);
    const auto address=gs_texel_address(19,image.base_block,image.buffer_width_64,36,64)->byte_address;
    require(owner.memory.observe_byte(address).allocation_owner==neighbor_image.id);
    const float nx=(36.5f-32.75f)/32.25f,ny=(64.25f-32.75f)/32.25f;
    const Float4 normal{nx,ny,std::sqrt(1-nx*nx-ny*ny),0};
    const std::array<Surface,1> surfaces{{{{0,0,0,1},0,true,false,0,2,.5f}}};
    const auto model_file=read_xmd(model(surfaces,true,true,false,normal));
    auto resource=owner.load(model_file,xim(0,8,8,{0,0,0,128}));Scene scene(model_file,resource.slots,owner);
    FieldOrdinaryLighting light;light.colors={Float4{},Float4{},Float4{},Float4{1,1,1,1}};
    scene.renderer.set_stage_environment(light,FieldShadowSettings{},{});
    scene.renderer.set_material_environment(decode_xim(env_bytes));scene.renderer.set_shared_texture_owner(owner.handles,env);
    const auto old=scene.prepare();const auto baseline=scene.renderer.render_prepared(old);
    require(observed_register(scene.observation(),"tex0",1)==source_tex0(owner,env,0));
    require(center(baseline)[0]>0&&center(baseline)[0]<100); // ENV indices, not neighbor's200+ palette.
    owner.memory.write_bytes(address,Data{151});const auto changed=scene.render();
    require(changed.rgba!=baseline.rgba&&center(changed)[0]>center(baseline)[0]);
    require(scene.renderer.render_prepared(old).rgba==baseline.rgba);
    owner.memory.invalidate_owned_nibbles(neighbor_image.id);
    const auto failure=rejection(scene.renderer.submit_prepared(scene.prepare()));
    require(failure.find("Unknown shared GS texture nibble address=0x")!=std::string::npos&&
        failure.ends_with("owner="+std::to_string(neighbor_image.id)));
    require(scene.renderer.render_prepared(old).rgba==baseline.rgba);
    owner.resources.retire_xmd(resource);
}

static void owner_guard() {
    Owned owner,other;SceneRenderer renderer;renderer.set_shared_texture_owner(owner.handles);
    bool rejected=false;try{renderer.set_shared_texture_owner(other.handles);}catch(const std::runtime_error& e){rejected=std::string(e.what())=="Shared GS renderer texture owner changed";}
    require(rejected);
}
int main(){@autoreleasepool{try{physical_snapshot_lifetime();unknown_positive_tap();saved_original_and_variant();callback_time_source_packet();direct_high_lane();cross_owner_environment();owner_guard();
    std::cout<<"Shared GS SceneRenderer: physical overwrite, immutable submitted/prepared lifetimes, exact nibble/owner guards, saved TEX0 restoration, direct high lanes, cross-owner ENV36/64 and owner guard passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
