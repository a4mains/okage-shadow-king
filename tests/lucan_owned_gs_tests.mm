#import <Foundation/Foundation.h>
#include "okage/startup_inputs.hpp"
#include "okage/startup_textures.hpp"
#include "okage/scene_renderer.hpp"
#include "okage/scene_model_asset.hpp"
#include "okage/battle_actor.hpp"
#include "okage/battle_camera.hpp"
#include "okage/battle_data.hpp"
#include "okage/field.hpp"
#include "okage/field_projection.hpp"
#include "okage/stage_settings.hpp"
#include "okage/map_objects.hpp"
#include "okage/gs_texture_address.hpp"
#include "okage/texture_transfer.hpp"
#include "okage/sha256.hpp"
#include "okage/render.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace okage;
namespace fs=std::filesystem;
namespace {
using Data=std::vector<std::uint8_t>;
void check(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
NSString* str(const std::string& value){return [NSString stringWithUTF8String:value.c_str()];}
void json(const fs::path& path,id document){NSError* error=nil;auto data=[NSJSONSerialization dataWithJSONObject:document options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:&error];
    check(data&&error==nil,"JSON encode");check([data writeToFile:str(path.string()) atomically:YES],"JSON output");}
void raw_image(const fs::path& path,const Texture& image){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(image.rgba.data()),image.rgba.size());check(bool(out),"Raw Metal RGBA output");}
NSDictionary* parse(const std::string& source){NSError* error=nil;auto data=[NSData dataWithBytes:source.data() length:source.size()];id value=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];check(value&&error==nil,"JSON parse");return value;}
struct Inputs {
    fs::path root,battle,output;std::map<std::string,std::string> hashes;
    Data read(const fs::path& path){std::ifstream in(path,std::ios::binary);check(bool(in),"Missing owned input: "+path.string());
        Data bytes(std::istreambuf_iterator<char>(in),{});hashes[path.string()]=sha256_hex(bytes);return bytes;}
    ImmutableTextureBytes owned(const fs::path& path){return std::make_shared<const Data>(read(path));}
    TextureHandleOwner::FileReader reader(const fs::path& directory){return [this,directory](std::string& name)->std::optional<Data>{
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
        const auto path=directory/name;if(!fs::exists(path))return std::nullopt;return read(path);};}
    ImmutableTextureBytes override27(const fs::path& path){auto bytes=read(path);check(bytes.size()>=4,"XIM header");unsigned w=0;for(unsigned b=0;b<4;++b)w|=unsigned(bytes[b])<<(8*b);w=(w&0xfc0fffff)|(27u<<20);for(unsigned b=0;b<4;++b)bytes[b]=w>>(8*b);return std::make_shared<const Data>(std::move(bytes));}
    void report()const{NSMutableDictionary* pins=[NSMutableDictionary dictionary];for(const auto& [path,hash]:hashes)pins[str(path)]=str(hash);json(output/"owned-inputs.json",pins);}
};
struct Resources {
    StartupTextureOwner owner;Inputs& input;GameRandom random{1};
    std::map<std::string,std::shared_ptr<SceneModelAsset>> actors;
    std::vector<std::pair<const SceneModelAsset*,unsigned>> instances;
    std::vector<EffectDefinition> effect_definitions;EffectTextureSizes effect_sizes{};MapEffects map_effects{};
    explicit Resources(Inputs& in):input(in){}
    std::shared_ptr<SceneModelAsset> load(const fs::path& path,bool motion,unsigned flags=0,bool actor=false){
        const auto key=path.parent_path().stem().string();
        if(actor)if(const auto found=actors.find(key);found!=actors.end()){
            const auto instance=owner.models().allocate_instance(*found->second->resource_group,found->second->shadow_group,flags);
            instances.push_back({found->second.get(),instance});return found->second;
        }
        auto asset=std::make_shared<SceneModelAsset>();asset->model=read_xmd(input.read(path));
        auto xmt=path,xta=path;xmt.replace_extension(".xmt");xta.replace_extension(".xta");
        if(motion&&fs::exists(xmt)){asset->motion=input.owned(xmt);if(fs::exists(xta)){asset->sidecar=input.owned(xta);asset->animation=read_xta({reinterpret_cast<const char*>(asset->sidecar->data()),asset->sidecar->size()});}}
        asset->packet_textures=std::make_shared<TextureBindings>(asset->model,asset->animation);
        const auto reader=input.reader(path.parent_path());
        const auto& loaded=owner.load_field_model(path.filename().string(),asset->model.textures,reader,{},unsigned(asset->model.models.size()));
        asset->resource_group=loaded.range;asset->textures=loaded.textures.slots;
        if(asset->motion){owner.load_field_sidecar(asset->sidecar,reader,{});for(const auto& slot:loaded.textures.xta_variants)asset->textures.push_back(slot);}
        configure_scene_model_texture_source(*asset,owner.handles());
        if(actor){
            const auto shadow=path.parent_path()/"shadow.xmd";
            if(((flags>>5)&3)&&fs::exists(shadow)){asset->shadow=std::make_shared<const ModelFile>(read_xmd(input.read(shadow)));check(asset->shadow->textures.empty(),"Textured shadow remains guarded");
                asset->shadow_group=owner.models().insert("shadow.xmd",unsigned(asset->shadow->models.size()),{},false).range;}
            owner.models().register_actor(key,{*asset->resource_group,asset->shadow_group});actors.emplace(key,asset);
            const auto instance=owner.models().allocate_instance(*asset->resource_group,asset->shadow_group,flags);instances.push_back({asset.get(),instance});
        }
        return asset;
    }
    SceneModelAssets map(const fs::path& path){const auto map=read_xmp(input.read(path));SceneModelAssets assets(map.definitions.size());
        for(unsigned i=0;i<map.definitions.size();++i){const auto& d=map.definitions[i];if(d.name=="effect$$.xmd")continue;assets[i]=load(path.parent_path()/d.name,bool(d.flags()&8));}
        const auto ids=map_instance_ids(map);std::vector<unsigned> counts(map.definitions.size());
        for(unsigned i=0;i<map.placements.size();++i){const auto definition=map.placements[i].definition();++counts.at(definition);
            if(map.placements[i].handler()){
                check(map.placements[i].handler()==1,"Unrecovered map placement handler");const auto id=map.placements[i].handler_argument()&127;
                const auto found=std::find_if(effect_definitions.begin(),effect_definitions.end(),[&](const auto& effect){return effect.id==id;});check(found!=effect_definitions.end(),"Owned effect definition");
                unsigned slot=0;while(slot<map_effects.size()&&map_effects[slot])++slot;check(slot<map_effects.size(),"Source effect pool capacity");
                const auto transform=map_transform(map.placements[i],map.definitions[definition]);map_effects[slot]=std::make_unique<MapEffect>(*found,transform.position,transform.rotation,transform.scale,effect_sizes,[&]{return random.next();});
            }
            if(ids[i]>=0)check(owner.models().allocate_instance(*assets.at(definition)->resource_group,{},map_transform(map.placements[i],map.definitions[definition]).flags)==unsigned(ids[i]),"Map source instance order");}
        for(unsigned i=0;i<counts.size();++i)if(!counts[i]&&assets[i])owner.models().retire(*assets[i]->resource_group);
        return assets;
    }
    void startup(){const auto verified=load_startup_inputs(input.root);
        const auto manifest_bytes=input.read(input.root/"NATIVE_STARTUP/startup-inputs.json");NSDictionary* manifest=parse(std::string(manifest_bytes.begin(),manifest_bytes.end()));
        for(NSString* section in @[@"payload",@"inputs"])for(NSString* path in manifest[section]){const auto bytes=input.read(input.root/path.UTF8String);check(str(sha256_hex(bytes))&&[str(sha256_hex(bytes)) isEqual:manifest[section][path]],"Startup imported byte hash");}
        StartupTextureInputs in;in.font_palette=verified.font_palette;in.fonts=verified.fonts;in.record_observations=true;
        const auto& names=startup_effect_names();in.read_effect=[&](const std::string& name)->std::optional<Data>{for(unsigned i=0;i<names.size();++i)if(name==names[i])return verified.effects[i];return std::nullopt;};
        effect_definitions=read_efd(verified.efd);std::shared_ptr<const EffectTemplatePreparation> preparation;
        owner.run_startup(in,random,[&](StartupRandomStream& stream){for(unsigned i=0;i<effect_sizes.size();++i){const auto image=decode_xim(verified.effects[i]);effect_sizes[i]={image.width,image.height};}
            EffectPreparationTrace trace;trace.grid_complete=[&]{stream.enter(StartupRngPhase::EffectTemplate);};
            preparation=std::make_shared<const EffectTemplatePreparation>(effect_definitions,effect_sizes,make_sine_table(),[&]{return stream.draw();},&trace);});
        check(owner.rng_draws()==9504,"Pinned startup RNG draws");random.observe=[](void* context,std::uint32_t state){static_cast<Resources*>(context)->owner.note_session_draw(state,0);};random.observe_context=this;effect_definitions=resolve_effect_references(std::move(effect_definitions));owner.begin_scheduled_frames();owner.handles().set_reservation_class(1);
        const auto scr=input.root/"CMNDATA/2D/SCR.XPF";owner.begin_logo_construction();owner.append_image(input.read(scr/"scea.xim"));owner.append_image(input.read(scr/"zener.xim"));owner.retire_image_list();
        owner.begin_title_construction();for(const auto* name:{"title_sample4.xim","push.xim","new.xim","restore.xim","maru_w.xim","maru_b.xim","sp.xim","hanten.xim"})owner.append_image(input.read(scr/name));
        const auto night=input.root/"CMNDATA/2D/NIGHT_V.XPF/a.xmd";const auto model=read_xmd(input.read(night));check(owner.load_and_bind_model("NIGHT_V/a.xmd",model.textures,input.reader(night.parent_path())).first>=0,"Title source load");
        owner.finish_title_construction();owner.begin_title_updates();owner.request_title_successor(9);owner.retire_title_resources();owner.destroy_title_mode();owner.free_title_resource_node();owner.free_title_wrappers();owner.enter_successor(9);
        owner.construct_name_entry(input.read(input.root/"CMNDATA/2D/MAIN.XPF/bm_if.xim"));owner.name_manager_death();owner.name_entry_requested(2,72,0);owner.retire_name_entry_resources();owner.release_name_atlas();owner.free_name_entry_resource_node();owner.free_name_entry_wrappers();
        owner.construct_chapter(1,input.read(input.root/"CMNDATA/2D/CHP_01.XIM"));owner.chapter_requested(2,72,0);owner.retire_chapter_resources();owner.destroy_chapter_mode();owner.free_chapter_resource_node();owner.free_chapter_wrappers();owner.enter_field_constructor(72,0);
    }
    void first_field(std::span<std::uint8_t> variables){owner.construct_field_mode(72,0);owner.enter_field_worker();const auto main=input.root/"CMNDATA/2D/MAIN.XPF";
        owner.load_field_atlas(input.override27(main/"bm_if.xim"));owner.bind_field_atlas();owner.load_field_texenv(input.owned(main/"texenv.xim"));owner.load_field_ruw(input.owned(input.battle/"FLD.RUW"));owner.publish_field_ui();
        auto definition=read_field_definition(input.read(input.root/"FLDDATA/FLDSET.XDB"),72);
        const auto script=input.read(input.root/"FLDDATA/EVENT/EVDATA.XPF"/(definition.event+".xmt"));definition=prepare_field_load(script,variables,std::move(definition),0);owner.begin_field_world();
        const auto path=input.root/"FLDDATA"/definition.room_directory/(definition.room+".XPF")/"a.xmp";auto retained=map(path);
        for(const auto& npc:definition.npcs)load(input.root/"FLDDATA/OBJ"/(npc.model+".XPF")/"a.xmd",true,0xe0,true);
        load(input.root/"FLDDATA/OBJ/STUN.XPF/a.xmd",true,0xa0,true);load(input.root/"FLDDATA/OBJ/HERO.XPF/a.xmd",true,0xc0,true);
        owner.complete_field_world();owner.commit_field_map();owner.activate_field();
        write_title_checkpoint_report(owner,input.output/"targeted-first-field", "targeted-first-field",random.state,CheckpointSchema::FieldWorld);
        owner.retire_field_resources();owner.retire_field_manager();owner.free_field_resource_node();owner.free_field_wrappers();actors.clear();instances.clear();for(auto& effect:map_effects)effect.reset();owner.handles().set_reservation_class(0);
    }
};
NSDictionary* document(const SceneRenderer::SubmittedFrame& frame){std::ostringstream s;frame.write_texture_sampling_observation(s);return parse(s.str());}
}
int main(int argc,char** argv){@autoreleasepool{try{
    if(argc!=4){std::cerr<<"Usage: lucan-owned-gs-tests OWNED_STAGE_ROOT OWNED_BATTLE_INPUT FRESH_REPORT_DIR\n";return 77;}
    Inputs input{argv[1],argv[2],argv[3],{}};check(!fs::exists(input.output),"Report directory must be fresh");fs::create_directories(input.output);
    const GameDataTables tables(input.read(input.root/"CMNDATA/XDB/BKDATA.XDB"),input.read(input.root/"CMNDATA/XDB/ITEM.XDB"));
    std::array<std::uint8_t,0xd78> variables{};initialize_new_game_variables(variables,tables);
    auto resource=std::make_unique<Resources>(input);resource->startup();resource->first_field(variables);
    resource->owner.handles().set_reservation_class(1);const auto main=input.root/"CMNDATA/2D/MAIN.XPF";
    const auto env_bytes=input.owned(main/"texenv.xim");resource->owner.begin_battle(4,2,input.owned(input.battle/"BTL.RUW"),input.override27(main/"bm_if.xim"),env_bytes);
    const BattleData battle_data(input.read(input.battle/"battle-data.bin"));const auto package=input.root/battle_data.stage_path(2);const auto map_path=package/"a.xmp";
    auto map_assets=resource->map(map_path);MapObjects objects(map_path,map_assets);
    BattleState root;root.initialize_root({1,4,2});BattleEntryContext entry;BattleRoster roster(root);
    const BattleModelAssetProvider provider=[&](const fs::path& path,unsigned flags){return resource->load(path,true,flags,true);};
    roster.construct_party(input.root,variables,tables,entry,[&]{return resource->random.next();},provider);
    roster.construct_enemies(input.root,4,battle_data.formation(4),tables,0,[&]{return resource->random.next();},provider);
    resource->owner.complete_battle();resource->owner.handles().set_reservation_class(0);
    const BattleActor* lucan=nullptr;std::vector<SceneActor> bindings;unsigned alias=0;
    for(const auto& actor:roster.actors()){
        if(actor->model_path.parent_path().stem()=="LUCAN")lucan=actor.get();
        const auto bind=[&](const std::unique_ptr<FieldActor>& model,const fs::path& path,const std::shared_ptr<const SceneModelAsset>& asset){if(!model)return;
            SceneActor scene{model.get(),path,int(alias++),SceneActorDescriptor{model->render_flags,false}};scene.texture_slots=asset->textures;scene.textures_owned=true;scene.shadow_model=asset->shadow;scene.shadow_owned=true;bindings.push_back(std::move(scene));};
        bind(actor->model,actor->model_path,actor->model_asset);bind(actor->bag,actor->bag_path,actor->bag_asset);bind(actor->box,actor->box_path,actor->box_asset);
    }
    check(lucan,"Owned initial roster did not select LUCAN; no replacement permitted");
    const auto& sword=lucan->model_asset->model.models.at(0).blocks.at(1);check(sword.materials.at(2).words[7]==2&&sword.polygons.size()>0,"Retail LUCAN sword material changed");
    auto renderer=std::make_unique<SceneRenderer>(map_path,bindings,&objects,map_assets);renderer->set_material_environment(decode_xim(*env_bytes));const int env=resource->owner.battle_environment_handle();renderer->set_shared_texture_owner(resource->owner.handles(),env);renderer->set_texture_sampling_audit(std::array<std::uint32_t,2>{36,64});
    FieldProjection projection;StageSettings settings;const auto stage_data=StageData::parse(input.read(package/"a.stg"));for(const auto& setting:stage_data.settings())settings.apply(setting,projection);projection.synchronize_stage_effects();
    renderer->set_stage_environment(settings.lighting,settings.shadows,settings.sky);renderer->set_sort_depth_shift(settings.bucket_base);renderer->set_clear_rgb(settings.clear.rgb);
    const auto image=*resource->owner.memory().allocation(resource->owner.handles().image(env)->id);const auto a=*gs_texel_address(19,image.base_block,image.buffer_width_64,36,64);const auto physical=resource->owner.memory().observe_byte(a.byte_address);
    check(physical.value.has_value(),"Modeled ENV36/64 physical dependency is unknown; no padding permitted");
    const auto supplying=physical.allocation_owner?resource->owner.memory().allocation(*physical.allocation_owner):std::nullopt;
    json(input.output/"dependency.json",@{@"scope":@"targeted owned startup-HOME1C-retire-formation4-stage2 resource case; no natural-route/original-address parity",@"environment_handle":@(env),@"environment_allocation":@(image.id),@"environment_serial":@(image.serial),@"environment_base":@(image.base_block),@"environment_tbw":@(image.buffer_width_64),@"target":@[@36,@64],@"physical_address":@(a.byte_address),@"raw_index":@(*physical.value),@"supplying_allocation":supplying?@(supplying->id):(id)[NSNull null],@"supplying_serial":supplying?@(supplying->serial):(id)[NSNull null]});
    write_title_checkpoint_report(resource->owner,input.output/"targeted-battle", "targeted-battle",resource->random.state,CheckpointSchema::FieldWorld);input.report();
    BattleCameraData camera_data(input.read(input.battle/"battle-camera.bin"));BattleCameraMotion camera(camera_data.sequence(2),1);const auto sine=make_sine_table();
    fs::path supplying_path;ImmutableTextureBytes supplying_bytes;int supplying_handle=-1;
    for(const auto& asset:map_assets)if(asset)for(unsigned i=0;i<asset->model.textures.size();++i){const auto& slot=asset->textures.at(i);
        if(slot.image&&supplying&&slot.image->id==supplying->id){auto name=fs::path(asset->model.textures[i]);if(name.extension().empty())name+=".xim";supplying_path=package/name;supplying_bytes=slot.bytes;supplying_handle=slot.handle;}}
    check(supplying&&supplying_bytes&&supplying_handle>=0,"Supplying image must trace to an actual retained authored upload");
    check(*supplying_bytes==input.read(supplying_path),"Supplier retained bytes must equal owned source file");
    const OwnedRawXim supplier_xim(*supplying_bytes);check(supplier_xim.layout().pixel_format==19,"Supplying source image format");
    unsigned source_x=0,source_y=0,matches=0;for(unsigned y=0;y<supplying->height;++y)for(unsigned x=0;x<supplying->width;++x)
        if(gs_texel_address(19,supplying->base_block,supplying->buffer_width_64,x,y)->byte_address==a.byte_address){source_x=x;source_y=y;++matches;}
    check(matches==1&&supplier_xim.image()[source_y*supplying->width+source_x]==*physical.value,"Physical neighbor index must equal its authored payload, not ENV padding");
    const auto clut=*resource->owner.memory().allocation(resource->owner.handles().palette(env,0)->id);const auto clut_word=clut_scratch_word_index(*physical.value);
    const auto clut_address=gs_texel_address(0,clut.base_block,1,clut_word%16,clut_word/16)->byte_address;
    unsigned rgba_word=0;const OwnedRawXim environment_xim(*env_bytes);const auto env_palette=environment_xim.palette_level(0);
    for(unsigned c=0;c<4;++c){const auto value=resource->owner.memory().observe_byte(clut_address+c).value;check(value&&*value==env_palette[*physical.value*4+c],"CPU CLUT physical/upload mapping");rgba_word|=unsigned(*value)<<(8*c);}
    json(input.output/"source-upload-chain.json",@{@"supplying_source":str(supplying_path.string()),@"supplying_source_sha256":str(sha256_hex(*supplying_bytes)),@"supplying_handle":@(supplying_handle),@"supplying_dimensions":@[@(supplying->width),@(supplying->height)],@"supplying_source_xy":@[@(source_x),@(source_y)],@"environment_clut_allocation":@(clut.id),@"environment_clut_serial":@(clut.serial),@"cpu_clut_address":@(clut_address),@"cpu_raw_RGBA_word":@(rgba_word),@"image_size":@[@640,@224],@"rgba_file_encoding":@"RGBA8 rawGS/displayrenderer output; rowmajor640x224"});
    bool found=false;unsigned frames=0;NSMutableArray* trials=[NSMutableArray array];
    for(unsigned tick=0;tick<5&&!found;++tick){camera.tick();if(const auto value=camera.take_projection_request())projection.apply_stage_setting(0x44,{1,1,*value,1024},settings.cached_depth_min,settings.cached_depth_max);
        const auto p=camera.position(),r=camera.rotation();const Float4 rotation{-r[0],-r[1],-r[2],0};const auto view=camera_matrix({-p[0],-p[1],-p[2],1},rotation,{1,1,1,0},sine);
        projection.begin_field(settings.cached_depth_min,settings.cached_depth_max);projection.begin_effects();projection.begin_deferred();projection.end_effects();
        const auto& screen=projection.frame().ordinary;const auto prepared=renderer->prepare(view,field_clip_projection(screen),nullptr,rotation,screen.clip_far,p,&projection.frame(),1,false);auto ticket=renderer->submit_prepared(prepared);
        try{const auto rendered=ticket.wait();(void)rendered;}catch(const std::exception& error){NSMutableDictionary* record=[document(ticket) mutableCopy];record[@"failure"]=str(error.what());json(input.output/("sample-failed-"+std::to_string(tick)+".json"),record);throw;}
        const auto audit=document(ticket);unsigned hits=0;NSDictionary* positive=nil;for(NSDictionary* draw in audit[@"sampling_audit"][@"draws"]){
            if([draw[@"model_path"] isEqual:str(lucan->model_path.string())]&&[draw[@"part"] unsignedIntValue]==0&&[draw[@"block"] unsignedIntValue]==1&&[draw[@"material"] intValue]==2){hits+=[draw[@"successful_nonzero_taps"] unsignedIntValue];if([draw[@"successful_nonzero_taps"] unsignedIntValue])positive=draw;}}
        [trials addObject:@{@"source_camera_tick":@(tick),@"LUCAN_sword_36_64_positive_taps":@(hits)}];++frames;
        if(hits){check(tick==4&&positive&&[positive[@"texture_address"] unsignedIntValue]==a.byte_address&&[positive[@"selected_index"] unsignedIntValue]==*physical.value&&
            [positive[@"clut_address"] unsignedIntValue]==clut_address&&[positive[@"rgba_word"] unsignedIntValue]==rgba_word,"Actual shader fetch must match independently derived upload chain");
            GsTextureRegisters regs{std::stoull([positive[@"tex0"] UTF8String],nullptr,16),0x60,2ull|(2ull<<2)|(1023ull<<14)|(1023ull<<34),0x8000008080ull};
            const auto cpu=sample_gs_texture(resource->owner.memory().snapshot(),regs,36.5f/1024,64.5f/1024);
            NSMutableArray* cpu_rgba=[NSMutableArray array];for(unsigned c=0;c<4;++c){check(cpu.rgba[c]==float((rgba_word>>(8*c))&255),"CPU center sample must equal the actual shader tap raw CLUT word");[cpu_rgba addObject:@(cpu.rgba[c])];}
            NSMutableArray* channels=[NSMutableArray array];for(const auto& channel:lucan->model->channels()){NSMutableArray* bits=[NSMutableArray array];for(float value:channel)[bits addObject:@(std::bit_cast<unsigned>(value))];[channels addObject:bits];}
            json(input.output/"source-camera-pose.json",@{@"camera_sequence":@"owned stage2 initial sequence, first five sequential ticks only",@"camera_tick":@(tick),@"position":@[@(p[0]),@(p[1]),@(p[2]),@(p[3])],@"rotation":@[@(r[0]),@(r[1]),@(r[2]),@(r[3])],@"LUCAN_motion":@(lucan->model->motion()),@"LUCAN_channels_float_bits":channels,@"cpu_texel_center_raw_RGBA":cpu_rgba});
            json(input.output/"actual-owned-lucan-witness.json",audit);const auto before=ticket.readback();raw_image(input.output/"actual-owned-lucan-before.rgba",before);
            auto retained=renderer->submit_prepared(prepared);
            SceneActor lucan_binding{lucan->model.get(),lucan->model_path,0,SceneActorDescriptor{lucan->model->render_flags,false}};
            lucan_binding.texture_slots=lucan->model_asset->textures;lucan_binding.textures_owned=true;lucan_binding.shadow_model=lucan->model_asset->shadow;lucan_binding.shadow_owned=true;
            auto isolated=std::make_unique<SceneRenderer>();isolated->set_shared_texture_owner(resource->owner.handles(),env);isolated->set_material_environment(decode_xim(*env_bytes));
            isolated->set_stage_environment(settings.lighting,settings.shadows,settings.sky);isolated->set_sort_depth_shift(settings.bucket_base);isolated->set_clear_rgb(settings.clear.rgb);isolated->set_texture_sampling_audit(std::array<std::uint32_t,2>{36,64});isolated->add_actor(lucan_binding);
            const auto isolated_prepared=isolated->prepare(view,field_clip_projection(screen),nullptr,rotation,screen.clip_far,p,&projection.frame(),1,false);auto isolated_ticket=isolated->submit_prepared(isolated_prepared);const auto isolated_before=isolated_ticket.wait();
            json(input.output/"lucan-only-before-control.json",document(isolated_ticket));raw_image(input.output/"lucan-only-before-control.rgba",isolated_before);
            check(supplying&&supplying->id!=image.id,"Expected actual cross-owner dependency");
            check(resource->owner.memory().invalidate_owned_nibbles(supplying->id)>0,"Supplying owner invalidation");
            auto failed=isolated->submit_prepared(isolated->prepare(view,field_clip_projection(screen),nullptr,rotation,screen.clip_far,p,&projection.frame(),1,false));bool rejected=false;
            try{(void)failed.wait();}catch(const std::exception& error){rejected=std::string(error.what()).find("Unknown shared GS texture nibble address=0x")!=std::string::npos&&std::string(error.what()).ends_with("owner="+std::to_string(supplying->id));json(input.output/"supplying-owner-invalidated.json",@{@"failure":str(error.what()),@"sampling":document(failed)});}
            check(rejected,"New real LUCAN draw ignored supplying owner invalidation");check(renderer->render_prepared(prepared).rgba==before.rgba,"Old real LUCAN prepared snapshot changed");
            check(isolated->render_prepared(isolated_prepared).rgba==isolated_before.rgba,"Old isolated LUCAN snapshot changed");
            unsigned retirement=0;for(const auto& actor:roster.actors()){resource->owner.retire_battle_actor(resource->instances.at(retirement).second);retirement+=actor->enemy?3:1;}
            resource->owner.retire_battle_root();resource->owner.retire_battle_resources();resource->owner.free_battle_resource_node();resource->owner.free_battle_wrappers();
            isolated.reset();renderer.reset();resource.reset();const auto retained_image=retained.wait();check(retained_image.rgba==before.rgba,"Actual Metal frame lost bytes after free/owner/renderer destruction");
            raw_image(input.output/"actual-owned-lucan-retained-after-destruction.rgba",retained_image);found=true;
        }
        if(camera.ready())break;
    }
    json(input.output/"trials.json",trials);input.report();
    json(input.output/"result.json",@{@"actual_owned_LUCAN_sword_target_reached":@(found),@"rendered_source_camera_ticks":@(frames),@"natural_route_claim":@NO,@"original_address_or_index_claim":@NO});
    check(found,"No actual retail LUCAN sword36/64 positive tap in bounded authored camera sequence; dependency remains incomplete");
    std::cout<<"Actual owned LUCAN sword shared GS36/64 dependency and supplying-owner negative control passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}}
