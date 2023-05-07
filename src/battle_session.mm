#include "battle_session.hpp"
#include "battle_filter_metal.hpp"
#include "ui_metal.hpp"
#include "okage/battle_data.hpp"
#include "okage/battle_ui.hpp"
#include "okage/load_queue.hpp"
#include "okage/ruw.hpp"
#include "okage/map_objects.hpp"
#include "okage/collision.hpp"
#include "okage/event.hpp"
#include "okage/animation.hpp"
#include "okage/scene_renderer.hpp"
#include "okage/battle_actor.hpp"
#include "okage/battle_enemy_commands.hpp"
#include "okage/battle_command_queue.hpp"
#include "okage/battle_ui_diagnostic.hpp"
#include "okage/ui_resource.hpp"
#include "okage/ui_draw_diagnostic.hpp"
#include "okage/stage_settings.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cfenv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace fs=std::filesystem;
namespace {
std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size=fs::file_size(path);
    if(size>128*1024*1024)throw std::runtime_error("Battle asset size limit");
    std::vector<std::uint8_t> result(size);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(result.data()),result.size()))
        throw std::runtime_error("Cannot read battle asset: "+path.string());
    return result;
}
std::uint32_t word(Bytes b,unsigned at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Battle asset word bounds");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
void put(std::span<std::uint8_t> b,unsigned at,std::uint32_t value) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Battle scalar bounds");
    for(unsigned j=0;j<4;++j)b[at+j]=std::uint8_t(value>>(j*8));
}
fs::path archive_path(const fs::path& root,std::string path) {
    if(path.empty()||path.front()=='/'||path.find("..")!=std::string::npos||path.find('\\')!=std::string::npos)
        throw std::runtime_error("Unsafe battle archive path");
    std::transform(path.begin(),path.end(),path.begin(),[](unsigned char c){return char(std::toupper(c));});
    return root/path;
}
struct StageScratch {
    std::array<std::uint8_t,0x278> first{};
    std::array<std::array<std::uint8_t,0x348>,3> other{};
    std::array<std::uint32_t,10> selectors{0x22,0x1c};
    std::string name="LOAD_DEFAULT";
    unsigned camera_settings_count=0;
    StageScratch() {
        first[0]='S';first[1]='T';first[2]='G';first[4]=0x20;first[8]=0x20;
    }
};
struct UiTextureOwner {id<MTLTexture> texture;};
}
struct BattleSession::Impl {
    unsigned formation;
    int stage;
    fs::path input,decoded;
    bool alternate_interface;
    BattleState root;
    StageScratch scratch;
    ShadowScreenParameters ui_projection=initial_ui_projection();
    std::array<std::shared_ptr<RuwResource>,16> ui_resources;
    std::optional<BattleUiData> ui_definitions;
    std::optional<BattleHudData> hud_definitions;
    std::optional<BattleUiCommandData> command_definitions;
    std::unique_ptr<BattleInterface> ui;
    UiResourceCoordinator ui_resource;
    std::optional<UiDrawList> ui_draw;
    std::optional<UiGeometryList> ui_geometry;
    std::shared_ptr<UiTextureOwner> window_owner;
    std::shared_ptr<const std::vector<Font>> font_owner;
    std::array<id<MTLTexture>,3> font_textures{};
    bool reported_active_resource=false;
    bool saved_model_range=true;
    std::uint32_t stage_mask=0x18000000,camera_mask=0x18000000;
    std::uint32_t party_mask=0x18000000,enemy_mask=0x18000000;
    unsigned party_phase=0,enemy_phase=0;
    // The fresh UI registries have no owners until 11f5f8 constructs them.
    std::array<std::uint32_t,16> ui_registry_a{},ui_registry_b{};
    std::array<std::uint32_t,9> ui_controllers{};
    std::atomic<unsigned> load_step{0};
    std::atomic<bool> suppress_effects{false};
    std::atomic<bool> resources_prepared{false};
    enum class Construction {Preparing,Constructing,Complete,Failed};
    Construction construction=Construction::Preparing; //Main thread only.
    std::exception_ptr construction_failure;
    unsigned phase=0;
    enum class ParentCallback {Intro,Combat,CommandMenu,CommandAction};
    ParentCallback parent_callback=ParentCallback::Intro;
    std::optional<ParentCallback> previous_parent_callback;
    unsigned live_trace_rows=0,live_trace_calls=0;
    std::size_t live_trace_bytes=0;
    bool loader_resumed=false;
    id<MTLDevice> device;
    id<MTLTexture> atlas,environment;
    Float4 environment_uv{};
    std::vector<std::uint8_t> held_stage;
    std::optional<StageData> stage_data;
    std::optional<StageSettings> stage_settings;
    bool stage_committed=false;
    std::unique_ptr<MapObjects> objects;
    std::optional<CollisionMap> collision;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<BattleFilterMetal> filter_renderer;
    std::optional<Texture> rendered_image;
    Animation& camera_channels;
    Float4 resource_camera_position{},resource_camera_rotation{};
    std::optional<BattleCameraMotion> camera;
    std::optional<BattleCameraData> camera_data;
    std::optional<BattleCameraSequence> camera_sequence;
    std::optional<BattleCameraSequence> after_intro_sequence;
    std::optional<BattleData> battle_data;
    std::optional<BattleActionData> action_data;
    std::optional<BattleEnemyCommands> enemy_commands;
    BattleRoster roster{root};
    // Last member: its destructor joins before any job-captured owners die.
    NativeLoadQueue jobs;
    Impl(unsigned f,int s,fs::path i,fs::path d,Bytes variables,BattleEntryContext& entry,Animation& channels):
        formation(f),stage(s),input(std::move(i)),decoded(std::move(d)),
        alternate_interface(variables.size()>12&&variables[12]!=0),camera_channels(channels) {
        if(variables.size()<0xd78)throw std::runtime_error("Battle shared variable extent");
        if(event_variable(variables,0x40005)!=0)
            throw std::runtime_error("Battle wrapper bypass branch is unrecovered");
        root.initialize_root({1.f,formation,stage});entry.clear_prefix();
        stage_settings.emplace(SceneClearColor{{0,0,0}}); //100ef8 fresh resource clear.
        jobs.enqueue([this]{load();});
    }
    id<MTLTexture> upload(const Texture& image) {
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:image.width height:image.height mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        auto texture=[device newTextureWithDescriptor:descriptor];
        if(!texture)throw std::runtime_error("Battle texture allocation failed");
        [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0
            withBytes:image.rgba.data() bytesPerRow:image.width*4];
        return texture;
    }
    void load() {
        @autoreleasepool {
            ui_resources[0]=std::make_shared<RuwResource>(read(input/"BTL.RUW"));
            ui_definitions.emplace(read(input/"battle-ui.bin"));
            hud_definitions.emplace(read(input/"battle-hud.bin"));
            command_definitions.emplace(read(input/"battle-ui-commands.bin"));
            action_data.emplace(read(input/"battle-actions.bin"));
            enemy_commands.emplace(read(input/"battle-enemy-commands.bin"));
            load_step.store(1);
            device=MTLCreateSystemDefaultDevice();
            if(!device)throw std::runtime_error("Battle loader requires Metal");
            filter_renderer=std::make_unique<BattleFilterMetal>(device);
            const auto main=decoded/"CMNDATA/2D/MAIN.XPF";
            auto atlas_bytes=read(main/(alternate_interface?"bm_ifj.xim":"bm_if.xim"));
            // 18f500 sets image selector bits20..25 before image/CLUT upload.
            auto header=word(atlas_bytes,0);header=(header&0xfc0fffff)|(0x1bu<<20);
            for(unsigned j=0;j<4;++j)atlas_bytes[j]=header>>(8*j);
            atlas=upload_ui_gs_texture(device,decode_xim(atlas_bytes));
            auto env_bytes=read(main/"texenv.xim");
            const auto env_image=decode_xim(env_bytes);environment=upload(env_image);
            environment_uv={.5f/1024,.5f/1024,(float(env_image.width)+.5f)/1024,
                (float(env_image.height)+.5f)/1024};
            load_step.store(2);
            battle_data.emplace(read(input/"battle-data.bin"));
            (void)battle_data->formation(int(formation));
            const auto package=archive_path(decoded,battle_data->stage_path(stage));
            suppress_effects.store(true);
            if(fs::exists(package/"add.afd"))
                throw std::runtime_error("Battle stage additional effects are unrecovered");
            const auto map=package/"a.xmp";
            const auto map_data=read_xmp(read(map));
            if(std::any_of(map_data.definitions.begin(),map_data.definitions.end(),[](const auto& d){return d.words[0x80/4]!=0;}))
                throw std::runtime_error("Battle stage model collision-definition ownership is unrecovered");
            objects=std::make_unique<MapObjects>(map);
            renderer=std::make_unique<SceneRenderer>(map,std::span<const SceneActor>{},objects.get());
            held_stage=read(package/"a.stg");
            collision=read_xcl(read(package/"a.xcl"));
            load_step.store(3);
            camera_data.emplace(read(input/"battle-camera.bin"));
            camera_sequence=camera_data->sequence(stage);
            after_intro_sequence=camera_data->sequence(stage,true);
            resources_prepared.store(true,std::memory_order_release);
        }
    }
    void commit_stage(FieldProjection& projection,SceneClearColor& clear,unsigned frame,const fs::path& path) {
        if(!path.empty()&&(fs::exists(path)||fs::is_symlink(path)))
            throw std::runtime_error("Refusing existing battle stage report");
        if(held_stage.empty()||!renderer||!suppress_effects.load())
            throw std::runtime_error("Battle stage commit ownership order");
        stage_settings->clear=clear;
        stage_settings->clear_word=unsigned(clear.rgb[0])|(unsigned(clear.rgb[1])<<8)|(unsigned(clear.rgb[2])<<16);
        std::vector<std::string> snapshots;
        const auto array=[](std::ostream& out,const auto& values) {
            out<<'[';bool comma=false;for(auto v:values){if(comma)out<<',';comma=true;out<<v;}out<<']';
        };
        const auto float_bits=[](const auto& values) {
            std::vector<std::uint32_t> result;for(float v:values)result.push_back(std::bit_cast<std::uint32_t>(v));return result;
        };
        const auto snapshot=[&](const char* event,unsigned index,const StageSettingRecord* record=nullptr) {
            if(path.empty())return;
            const auto& s=*stage_settings;std::ostringstream out;
            out<<"{\"event\":"<<std::quoted(event)<<",\"setting_index\":"<<index;
            if(record){out<<",\"kind\":"<<record->kind<<",\"value_bits\":";array(out,record->value_bits);}
            out<<",\"projection\":";array(out,float_bits(field_projection_words(projection.current())));
            out<<",\"effects\":";array(out,float_bits(projection.effect_parameters()));
            out<<",\"base\":";array(out,float_bits(s.lighting.base));
            std::vector<std::uint32_t> colors,directions;
            for(const auto& v:s.lighting.colors)for(float f:v)colors.push_back(std::bit_cast<std::uint32_t>(f));
            for(const auto& v:s.lighting.directions)for(float f:v)directions.push_back(std::bit_cast<std::uint32_t>(f));
            out<<",\"colors\":";array(out,colors);out<<",\"directions\":";array(out,directions);
            out<<",\"sky\":";array(out,float_bits(s.sky));
            out<<",\"shadow_direction\":";array(out,float_bits(s.shadows.direction));
            out<<",\"shadow_tint\":";array(out,s.shadows.tint);
            out<<",\"shadow_threshold\":"<<s.shadows.threshold<<",\"clear_word\":"<<s.clear_word
               <<",\"fog_rgb\":"<<s.fog_rgb<<",\"signed_setting\":"<<s.signed_setting
               <<",\"bucket_shift\":"<<s.bucket_shift<<",\"bucket_base\":"<<s.bucket_base<<",\"selector\":"<<s.selector;
            out<<",\"cached_depth\":";array(out,float_bits(std::array{s.cached_depth_min,s.cached_depth_max}));
            out<<",\"filters\":[";for(unsigned i=0;i<s.filters.size();++i){if(i)out<<',';if(s.filters[i])array(out,*s.filters[i]);else out<<"null";}
            out<<"],\"held\":"<<(!held_stage.empty()?"true":"false")
               <<",\"suppression\":"<<(suppress_effects.load()?"true":"false")<<'}';
            snapshots.push_back(out.str());
        };
        std::exception_ptr failure;std::string error;
        try {
            snapshot("commit_entry",0);
            stage_data.emplace(StageData::parse(held_stage));
            std::copy(stage_data->header().begin(),stage_data->header().end(),scratch.first.begin());
            const auto copy_records=[](const auto& records,auto& bytes) {
                unsigned at=0;for(const auto& record:records)for(auto w:record.words)
                    for(unsigned c=0;c<4;++c)bytes.at(at++)=std::uint8_t(w>>(c*8));
            };
            copy_records(stage_data->camera_records(),scratch.other[0]);
            copy_records(stage_data->entrances(),scratch.other[1]);
            copy_records(stage_data->camera_settings(),scratch.other[2]);
            scratch.camera_settings_count=stage_data->camera_settings().size();
            unsigned index=0;
            for(const auto& setting:stage_data->settings()) {
                snapshot("setting_entry",index,&setting);
                stage_settings->apply(setting,projection);
                clear=stage_settings->clear;
                renderer->set_clear_rgb(clear.rgb);
                renderer->set_stage_environment(stage_settings->lighting,stage_settings->shadows,stage_settings->sky);
                snapshot("setting_return",index,&setting);++index;
            }
            snapshot("settings_applied_before_free",index);
            std::vector<std::uint8_t>().swap(held_stage);
            snapshot("effect_projection_sync_entry",index);
            projection.synchronize_stage_effects();
            suppress_effects.store(false);stage_committed=true;
            snapshot("commit_complete_before_ui",index);
        } catch(const std::exception& e){failure=std::current_exception();error=e.what();}
        if(!path.empty()) {
            if(!path.parent_path().empty())fs::create_directories(path.parent_path());
            std::ofstream out(path);
            const auto hex=[](Bytes b){std::ostringstream s;for(auto v:b)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return s.str();};
            out<<"{\"schema\":\"okage-native-battle-stage-v1\",\"frame\":"<<frame
               <<",\"complete\":"<<(stage_committed?"true":"false")<<",\"error\":"<<std::quoted(error)
               <<",\"owned_stage_hex\":"<<std::quoted(hex(stage_data?stage_data->bytes():Bytes(held_stage)))
               <<",\"header_hex\":"<<std::quoted(stage_data?hex(scratch.first):"");
            const auto records=[&](const char* name,const auto& values){
                out<<",\""<<name<<"\":[";bool comma=false;
                for(const auto& r:values){if(comma)out<<',';comma=true;array(out,r.words);}out<<']';
            };
            if(stage_data){records("cameras",stage_data->camera_records());records("camera_settings",stage_data->camera_settings());records("entrances",stage_data->entrances());}
            out<<",\"snapshots\":[";for(unsigned i=0;i<snapshots.size();++i){if(i)out<<',';out<<snapshots[i];}
            out<<"]}\n";if(!out)throw std::runtime_error("Cannot write battle stage report");
        }
        if(failure)std::rethrow_exception(failure);
    }
    void prepare_resource_ui(BattleUiContext context) {
        std::vector<UiResourceSlotBinding> bindings;
        if(ui)for(const auto handle:ui->handles()) {
            if(handle<0)throw std::runtime_error("Battle UI lost its registry slot");
            bindings.push_back({unsigned(handle),0});
        }
        ui_draw=ui_resource.advance_resource(context.shared.registry,bindings,
            context.shared.resource_accumulators,context.fonts,context.sine,
            context.random,ui_projection);
        ui_geometry=ui_draw?std::optional(resolve_ui_geometry(*ui_draw)):std::nullopt;
        if(ui_geometry&&!font_owner) {
            if(!context.fonts||context.fonts->size()!=3)throw std::runtime_error("Battle font owner extent");
            font_owner=context.fonts;
            for(unsigned i=0;i<3;++i)font_textures[i]=upload_ui_gs_texture(device,font_owner->at(i).atlas);
        }
        if(font_owner&&font_owner!=context.fonts)throw std::runtime_error("Battle font owner changed");
    }
    void construct_ui(BattleUiContext context,unsigned frame,const fs::path& stage_report) {
        saved_model_range=false;stage_mask&=~0x18000000u;camera_mask&=~0x18000000u;
        const int selected=std::bit_cast<std::int32_t>(word(root.bytes,0x81ac));
        if(!stage_report.empty()) {
            if(selected||std::any_of(context.party.counts().begin(),context.party.counts().end(),[](auto n){return n!=0;}))
                throw std::runtime_error("UI diagnostic requires the first-battle zero selection cache");
            for(const auto& row:context.party.rows())if(std::any_of(row.begin(),row.end(),[](auto n){return n!=0;}))
                throw std::runtime_error("UI diagnostic requires zero incoming selection rows");
        }
        window_owner=std::make_shared<UiTextureOwner>(UiTextureOwner{atlas});
        ui=std::make_unique<BattleInterface>(context.shared,*ui_definitions,window_owner,word(root.bytes,0x1c),[&](unsigned type){
            if(type==2)context.shared.hud.initialize_type2(*ui_resources[0],*hud_definitions,context.tables,
                context.variables,context.party,3,selected);
            else if(type==3)context.shared.hud.initialize_type3(*ui_resources[0],context.tables);
            else throw std::runtime_error("Unrecovered battle layer constructor");
        });
        if(stage_report.empty())return;
        auto hud_path=stage_report;hud_path.replace_extension(".hud.json");
        auto ui_path=stage_report;ui_path.replace_extension(".ui.json");
        for(const auto& path:{hud_path,ui_path})if(fs::exists(path)||fs::is_symlink(path))
            throw std::runtime_error("Refusing existing battle interface report");
        std::ofstream hud_out(hud_path);hud_out<<context.shared.hud.diagnostic(*ui_resources[0],context.variables,"native-session");
        if(!hud_out)throw std::runtime_error("Cannot write battle HUD report");
        std::ofstream out(ui_path);
        out<<"{\"schema\":\"okage-native-battle-interface-v1\",\"frame\":"<<frame
           <<",\"scope\":\"11f5f8 constructor before child activation; no callbacks or rendering\",\"manager_words\":[";
        for(unsigned i=0;i<ui->words().size();++i){if(i)out<<',';out<<ui->words()[i];}
        out<<"],\"current_stream\":"<<context.shared.current_stream<<",\"stream_cursor\":"<<context.shared.stream_cursor
           <<",\"atlas_dirty\":"<<(context.shared.atlas_dirty?"true":"false")<<",\"windows\":[";
        for(unsigned i=0;i<2;++i) {
            if(i)out<<',';out<<"{\"slot\":"<<ui->handles()[i]<<",\"words\":[";
            for(unsigned at=0;at<UiWindow::scalar_bytes;at+=4){if(at)out<<',';const auto v=ui->window(i).known_word(at);if(v)out<<*v;else out<<"null";}
            out<<"]}";
        }
        out<<"]}\n";if(!out)throw std::runtime_error("Cannot write battle interface report");
    }
    void inspect_ui_updates(BattleUiContext context,unsigned frame,const fs::path& path) {
        if(path.empty())return;
        if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing UI-update report");
        if(!ui||word(root.bytes,0x81ac)!=0)throw std::runtime_error("UI-update inspection requires constructed first interface");
        auto r=std::make_shared<RuwResource>(*ui_resources[0]);
        BattleUiShared copy;
        copy.hud=context.shared.hud;copy.command=context.shared.command;
        copy.formation=context.shared.formation;
        copy.selected_alpha_step=context.shared.selected_alpha_step;
        copy.secondary_pulse=context.shared.secondary_pulse;copy.secondary_pulse_step=context.shared.secondary_pulse_step;
        copy.cost_pulse=context.shared.cost_pulse;copy.hp_cost_gate=context.shared.hp_cost_gate;
        copy.target_count=context.shared.target_count;
        copy.pending_action=context.shared.pending_action;
        copy.resource_accumulators=context.shared.resource_accumulators;
        BattleInterface isolated(copy,*ui_definitions,std::make_shared<UiTextureOwner>(UiTextureOwner{atlas}),
            word(root.bytes,0x1c),[](unsigned){});
        if(isolated.words()!=ui->words())throw std::runtime_error("UI diagnostic clone manager mismatch");
        for(unsigned i=0;i<2;++i)for(unsigned at=0;at<UiWindow::scalar_bytes;at+=4)
            if(isolated.window(i).known_word(at)!=ui->window(i).known_word(at))
                throw std::runtime_error("UI diagnostic clone window mismatch");
        std::vector<std::uint8_t> vars(context.variables.begin(),context.variables.end());
        auto party=context.party;
        const BattleUiCommandData commands(read(input/"battle-ui-commands.bin"));
        BattleUiPad pad;
        isolated.first_update(r,pad,{},60);
        const auto before=battle_ui_update_snapshot(isolated,copy,*r,vars,party);
        std::vector<const BattleActor*> enemies;
        for(const auto& actor:roster.actors())if(actor->enemy)enemies.push_back(actor.get());
        isolated.second_update(*r,*hud_definitions,commands,
            {vars,context.tables,party,root,*battle_data,roster.entities(),enemies,3,&context.entry});
        const auto after=battle_ui_update_snapshot(isolated,copy,*r,vars,party);
        std::ofstream out(path);
        out<<"{\"schema\":\"okage-native-ui-second-update-v1\",\"scope\":\"isolated first and second dispatchers on copies of native Session construction state; no parent activation, resource clock, layout or rendering\",\"frame\":"<<frame
           <<",\"snapshots\":{\"before\":"<<before<<",\"after\":"<<after<<"}}\n";
        if(!out)throw std::runtime_error("Cannot write UI-update inspection report");
    }
    void activate(BattleUiContext context,FieldProjection& projection) {
        if(phase!=0||word(root.bytes,0x24)!=0||!ui||!stage_committed)
            throw std::runtime_error("Battle parent activation lifetime");
        party_mask&=~0x18000000u;enemy_mask&=~0x18000000u;
        context.release_capture(); //114df8, before the stage selectors.
        const auto setting=[&](unsigned kind,float value) {
            stage_settings->apply({kind,{std::bit_cast<std::uint32_t>(value),0,0,0}},projection);
        };
        setting(0x47,110.f);setting(0x4d,64.f);
        std::fenv_t saved;
        if(std::fegetenv(&saved)||std::fesetround(FE_TOWARDZERO))
            throw std::runtime_error("Battle activation rounding unavailable");
        const float scale=std::bit_cast<float>(word(root.bytes,0x2c));
        volatile float factor=.05f;
        const float step=scale*factor;
        std::fesetenv(&saved);
        if(!std::isfinite(step)||step<=0||std::fpclassify(step)==FP_SUBNORMAL)
            throw std::runtime_error("Battle activation fade timestep unsupported");
        context.fade.step=step;context.fade.request(1.f);
        ++phase;put(root.bytes,0x24,phase);
    }
    bool combat_end_check() {
        const auto entities=roster.entities();
        const auto count=word(root.bytes,0x50);
        if(count>battle_entity_count||entities.size()!=count||count<4)
            throw std::runtime_error("Battle end-check entity ownership mismatch");
        const auto state=word(root.bytes,0x81b8);
        if(state==0) {
            bool enemies_inactive=true;
            for(unsigned i=4;i<count;++i)
                if(!(word(root.entity(i),0x1fc)&0x80000000u))enemies_inactive=false;
            if(enemies_inactive) {
                put(root.bytes,0x81b8,1);put(root.bytes,0x81b4,1);
                return true;
            }
            auto* hero=entities[0].actor;
            bool defeat=!hero;
            if(hero) {
                if(hero->stat_bindings[1].empty())throw std::runtime_error("Battle end-check missing HERO status");
                const auto status=std::bit_cast<std::int8_t>(hero->stat_bindings[1][0]);
                defeat=status==6||(status==4&&battle_actor_incapacitated(hero)&&
                    battle_actor_incapacitated(entities[1].actor)&&battle_actor_incapacitated(entities[2].actor));
            }
            if(defeat)throw std::runtime_error("Battle defeat14a460 outer mode request and14a9c0 outcome are unrecovered");
            return false;
        }
        if(state!=1)return false;
        bool ready=true;
        for(unsigned i=0;i<3;++i)if(auto* actor=entities[i].actor)
            if(!battle_actor_incapacitated(actor)&&word(actor->storage,0x21c)!=100)ready=false;
        if(ready)throw std::runtime_error("Battle victory14a780 transition/effect cleanup is unrecovered");
        return true;
    }
    void combat_parent(BattleUiContext& context,std::vector<std::string>& events) {
        battle_refresh_entities(root,roster.entities()); //14a508 begins with151290.
        if(combat_end_check()){events.push_back("parent.end-check.return");return;}
        if(auto* hero=roster.entities()[0].actor) {
            const auto denominator=std::bit_cast<std::int32_t>(word(hero->stat_bindings[0],0));
            if(!denominator)throw std::runtime_error("Battle music ratio denominator is zero (source trap7)");
            const auto raw=std::uint16_t(hero->storage[0x164]|(unsigned(hero->storage[0x165])<<8));
            const auto numerator=std::bit_cast<std::int16_t>(raw);
            const unsigned alternate=(std::int64_t(numerator)/denominator)>=10;
            if(word(root.bytes,0x81c0)!=alternate) {
                put(root.bytes,0x81c0,alternate);context.music_variant(alternate!=0);
            }
        }
        auto owner=std::bit_cast<std::int32_t>(word(root.bytes,0x81ac));
        const auto command=battle_command_pop(root,roster.entities(),owner);
        put(root.bytes,0x81ac,std::bit_cast<std::uint32_t>(owner));
        if(command==0||command==1) {
            party_mask|=0x10000000u;enemy_mask|=0x10000000u;
            previous_parent_callback=parent_callback;
            parent_callback=command==0?ParentCallback::CommandMenu:ParentCallback::CommandAction;
            phase=0;put(root.bytes,0x24,0);
            put(root.bytes,0x30,command==0?0:std::bit_cast<std::uint32_t>(owner));
            events.push_back(command==0?"parent.command-menu.transition":"parent.command-action.transition");
        } else if(command==3||command==4) {
            throw std::runtime_error("Battle parent special command3/4 transition and effect cleanup are unrecovered");
        }
        events.push_back("parent.combat.return");
    }
    void scheduled_callbacks(BattleUiContext context,FieldProjection& projection,unsigned frame,const fs::path& stage_report) {
        fs::path report_path,ui_path;
        if(!stage_report.empty()) {
            report_path=stage_report;report_path.replace_extension(".callbacks.json");
            ui_path=stage_report;ui_path.replace_extension(".scheduled-ui.json");
            for(const auto& p:{report_path,ui_path})if(fs::exists(p)||fs::is_symlink(p))
                throw std::runtime_error("Refusing existing battle callback report");
        }
        std::vector<std::string> events,actor_snapshots;
        const auto hex=[](Bytes b){std::ostringstream out;for(auto v:b)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return out.str();};
        const auto callback_code=[&] {
            switch(parent_callback) {
            case ParentCallback::Intro:return 0x14a248u;
            case ParentCallback::Combat:return 0x14a508u;
            case ParentCallback::CommandMenu:return 0x1519f8u;
            case ParentCallback::CommandAction:return 0x151e68u;
            }
            throw std::runtime_error("Unknown native battle parent identity");
        };
        const auto trace=[&](const char* kind,const char* edge,unsigned call,unsigned entry) {
            if(!context.live_trace)return;
            const auto count=word(root.bytes,0x50);
            if(count>battle_entity_count||count!=roster.entities().size())throw std::runtime_error("Native battle-live entity ownership");
            std::ostringstream row;
            row<<"{\"event\":\"callback\",\"kind\":"<<std::quoted(kind)<<",\"edge\":"<<std::quoted(edge)
               <<",\"frame\":"<<frame<<",\"sequence\":"<<live_trace_rows<<",\"call_id\":"<<call
               <<",\"entry_pc\":"<<entry<<",\"parent_callback\":"<<callback_code()
               <<",\"parent_phase\":"<<phase<<",\"party_phase\":"<<party_phase<<",\"enemy_phase\":"<<enemy_phase
               <<",\"party_mask\":"<<party_mask<<",\"enemy_mask\":"<<enemy_mask
               <<",\"root_prefix_hex\":"<<std::quoted(hex(Bytes(root.bytes).first(0x60)))
               <<",\"root_tail_queue_hex\":"<<std::quoted(hex(Bytes(root.bytes).subspan(0x8060)))
               <<",\"registered_entities_hex\":"<<std::quoted(hex(Bytes(root.bytes).subspan(0x60,count*0x200)))
               <<",\"entry_hex\":"<<std::quoted(hex(context.entry.bytes()))
               <<",\"shared_variables_574\":"<<word(context.variables,0x574)
               <<",\"rng_state_word\":"<<context.random.state
               <<",\"pad\":["<<context.input.held<<','<<context.input.pressed<<','<<context.input.released<<','<<context.input.repeat
               <<"],\"action_graph_links\":[";
            for(unsigned i=0;i<count;++i) {
                if(i)row<<',';row<<'[';
                for(unsigned j=0;j<64;++j) {
                    if(j)row<<',';
                    if(root.action_links[i][j])row<<unsigned(*root.action_links[i][j]);else row<<"null";
                }
                row<<']';
            }
            row<<"],\"actors\":[";
            bool comma=false;
            for(const auto& actor:roster.actors()) {
                if(comma)row<<',';comma=true;
                row<<"{\"entity_id\":"<<actor->entity_id<<",\"enemy\":"<<(actor->enemy?"true":"false")
                   <<",\"task_hex\":"<<std::quoted(hex(Bytes(actor->storage).first(actor->enemy?0x3e0:0x3d0)))
                   <<",\"record_hex\":"<<std::quoted(hex(actor->record));
                row<<",\"action_record_id\":";
                if(actor->action_record_id)row<<*actor->action_record_id;else row<<"null";
                row<<",\"action_motion_id\":";
                if(actor->action_motion)row<<actor->action_motion->identity;else row<<"null";
                row<<",\"action_secondary_id\":";
                if(actor->action_secondary)row<<actor->action_secondary->identity;else row<<"null";
                row<<",\"stat_bindings\":[";
                for(unsigned j=0;j<actor->stat_bindings.size();++j) {
                    if(j)row<<',';
                    if(actor->stat_bindings[j].empty())row<<"null";
                    else row<<std::quoted(hex(actor->stat_bindings[j]));
                }
                row<<"],\"world_channel_bits\":[";
                const auto channels=actor->model->animation().channel_outputs();
                if(channels.size()<3)throw std::runtime_error("Native battle-live missing model channels");
                for(unsigned j=0;j<std::min<std::size_t>(4,channels.size());++j) {
                    if(j)row<<',';row<<'[';
                    for(unsigned k=0;k<4;++k){if(k)row<<',';row<<std::bit_cast<std::uint32_t>(channels[j][k]);}
                    row<<']';
                }
                row<<"]}";
            }
            row<<"]}\n";const auto text=row.str();
            if(live_trace_rows>=65536||text.size()>512*1024||text.size()>1024*1024*1024-live_trace_bytes)
                throw std::runtime_error("Native battle-live output bound");
            ++live_trace_rows;live_trace_bytes+=text.size();*context.live_trace<<text;context.live_trace->flush();
            if(!*context.live_trace)throw std::runtime_error("Native battle-live write failed");
        };
        const auto actor_snapshot=[&](const BattleActor& actor,const char* boundary) {
            if(report_path.empty())return;
            std::ostringstream out;
            out<<"{\"entity_id\":"<<actor.entity_id<<",\"boundary\":"<<std::quoted(boundary)
               <<",\"enemy\":"<<(actor.enemy?"true":"false")
               <<",\"task_hex\":"<<std::quoted(hex(Bytes(actor.storage).first(actor.enemy?0x3e0:0x3d0)))
               <<",\"record_hex\":"<<std::quoted(hex(actor.record))<<",\"channels\":[";
            bool comma=false;for(const auto& channel:actor.model->animation().channel_outputs())for(float f:channel){if(comma)out<<',';comma=true;out<<std::bit_cast<std::uint32_t>(f);}
            out<<"]}";actor_snapshots.push_back(out.str());
        };
        std::string before,after,error;
        std::exception_ptr failure;
        try {
            const auto parent_call=++live_trace_calls,parent_entry=callback_code();
            trace("parent","entry",parent_call,parent_entry);
            if(parent_callback==ParentCallback::Combat) {
                combat_parent(context,events);
            } else if(parent_callback==ParentCallback::CommandMenu) {
                if(phase==0) {
                    ui->open_layer(0);ui->fade_layer(1,-128);
                    //151098 is an empty source function; no camera reset.
                    ++phase;put(root.bytes,0x24,phase);
                    events.push_back("parent.command-menu.open");
                } else if(phase==1||phase==2) {
                    if(phase==1) {
                        const auto selected=std::bit_cast<std::int32_t>(word(root.bytes,0x81ac));
                        if(selected<0||selected>=3||!camera_data)
                            throw std::runtime_error("Battle command menu camera selection ownership");
                        camera->restart_sequence(camera_data->sequence_at_offset(stage,unsigned(selected)+2));
                        ++phase;put(root.bytes,0x24,phase);
                    }
                    //11fc40(layer0,7), then121a58/154418 on the ready list.
                    const auto selected_ordinal=std::bit_cast<std::int32_t>(ui->words()[0xc8/4]);
                    if(selected_ordinal>=0&&selected_ordinal<3) {
                        const auto ready=battle_ui_ready_party(roster.entities());
                        if(ready[unsigned(selected_ordinal)]>=0) {
                            put(root.bytes,0x81ac,std::uint32_t(ready[unsigned(selected_ordinal)]));
                            phase=1;put(root.bytes,0x24,phase);
                        }
                    }
                    switch(std::bit_cast<std::int32_t>(ui->words()[0xa4/4])) { //11fc70(layer0)
                    case 0:phase=1;put(root.bytes,0x24,phase);break;
                    case 1:case 7:case 9:case 12: {
                        const auto selected=std::bit_cast<std::int32_t>(word(root.bytes,0x81ac));
                        if(selected<0||selected>=3)throw std::runtime_error("Battle command camera selected entity extent");
                        const auto* actor=roster.entities()[unsigned(selected)].actor;
                        if(!actor||!actor->model)
                            throw std::runtime_error("Battle command camera absent-body channel1 fallback is unrecovered");
                        camera->select_command_camera(unsigned(selected),actor->model->animation().value(1));
                        break;
                    }
                    case 4: {
                        put(root.bytes,0x81a8,1);
                        party_mask&=~0x10000000u;enemy_mask&=~0x10000000u;
                        battle_refresh_entities(root,roster.entities());
                        ui->reveal_layer(1,16);
                        context.shared.hud.reset_type3(*ui_resources[0]);
                        if(!after_intro_sequence)throw std::runtime_error("Missing owned return-to-combat camera sequence");
                        camera->restart_sequence(*after_intro_sequence);
                        put(root.bytes,0x34,0);phase=0;put(root.bytes,0x24,0);
                        if(!previous_parent_callback)throw std::runtime_error("Battle command handoff has no saved parent");
                        parent_callback=*previous_parent_callback;
                        const auto& action=context.shared.pending_action;
                        if(action[0]!=11) {
                            const auto selected=std::bit_cast<std::int32_t>(word(root.bytes,0x81ac));
                            if(selected<0||std::size_t(selected)>=roster.entities().size()||
                               !roster.entities()[unsigned(selected)].actor)
                                throw std::runtime_error("Battle command handoff selected actor ownership");
                            auto& storage=roster.entities()[unsigned(selected)].actor->storage;
                            const auto half=[&](unsigned at,std::uint32_t value) {
                                if(at>=storage.size()||storage.size()-at<2)
                                    throw std::runtime_error("Battle action descriptor extent");
                                storage[at]=std::uint8_t(value);storage[at+1]=std::uint8_t(value>>8);
                            };
                            half(0x220,3);
                            for(unsigned j=0;j<3;++j)half(0x222+j*2,action[j]);
                            if(action[0]==2)half(0x226,202);
                        }
                        events.push_back("parent.command-menu.action-handoff");
                        break;
                    }
                    default:break;
                    }
                    events.push_back("parent.command-menu.selection");
                } else if(phase==3)throw std::runtime_error("Battle command-menu1519f8 cancellation lifecycle is unrecovered");
                //Other source phases return without mutation.
            } else if(parent_callback==ParentCallback::CommandAction) {
                throw std::runtime_error("Battle command-action151e68 camera/action lifecycle is unrecovered");
            } else if(phase==0) {
                events.push_back("parent.activation.entry");activate(context,projection);
                events.push_back("parent.activation.return");
            } else if(phase==1) {
                if(camera->ready()) {
                    if(!after_intro_sequence)throw std::runtime_error("Missing owned after-intro camera sequence");
                    ui->reveal_layer(1,16);
                    context.shared.hud.reset_type3(*ui_resources[0]);
                    camera->restart_sequence(*after_intro_sequence);
                    put(root.bytes,0x30,0);
                    previous_parent_callback=parent_callback;
                    parent_callback=ParentCallback::Combat;
                    phase=0;put(root.bytes,0x24,0);put(root.bytes,0x81b0,2);
                    events.push_back("parent.camera-ready.transition");
                    std::cout<<"Native battle intro complete frame="<<frame<<"; status layer revealed; authored camera restarted\n";
                } else events.push_back("parent.camera-wait.return");
            } else throw std::runtime_error("Battle parent phase is unrecovered");
            trace("parent","return",parent_call,parent_entry);
            //152330 sees its constructor's zero command;152368 is empty.
            if(stage_mask&0x18000000u)throw std::runtime_error("Battle stage remains masked");
            events.push_back("stage.first.return");events.push_back("stage.second.return");
            if(camera_mask&0x18000000u)throw std::runtime_error("Battle camera remains masked");
            camera->tick(camera_channels,[&](float value) {
                stage_settings->apply({0x44,{0x3f800000,0x3ef0a3d7,
                    std::bit_cast<std::uint32_t>(value),0x43e10000}},projection);
            });
            events.push_back("camera.first.return");events.push_back("camera.second.return"); //14f270 empty.
            for(bool enemy:{false,true}) {
                auto& child_phase=enemy?enemy_phase:party_phase;
                if((enemy?enemy_mask:party_mask)&0x18000000u)continue;
                const auto roster_call=++live_trace_calls,roster_entry=enemy?0x15afa0u:0x1524e0u;
                trace(enemy?"enemy_roster":"party_roster","entry",roster_call,roster_entry);
                if(child_phase==0&&word(root.bytes,0x81b0)!=0) {
                    for(const auto& actor:roster.actors())if(actor->enemy==enemy)put(actor->storage,0x21c,1);
                    ++child_phase;
                } else if(child_phase==1) {
                    BattleRosterUpdateContext update{root,context.entry,context.variables,roster.entities(),
                        context.input.pressed,BattleUiPad::selection_buttons,context.sound,
                        [&]{return context.random.next();},&context.tables,action_data?&*action_data:nullptr,enemy_commands?&*enemy_commands:nullptr};
                    std::vector<BattleActor*> actors;
                    BattleActor* stan=nullptr;
                    for(const auto& actor:roster.actors())if(actor->enemy==enemy) {
                        if(!enemy&&actor->entity_id==3)stan=actor.get();else actors.push_back(actor.get());
                    }
                    if(enemy)battle_enemy_roster_update(actors,update);
                    else {
                        if(!stan)throw std::runtime_error("Battle party roster lacks its Stan owner");
                        battle_party_roster_update(actors,*stan,update);
                    }
                } else if(child_phase!=0) {
                    throw std::runtime_error("Battle roster parent phase is unrecovered");
                }
                events.push_back(enemy?"enemy.parent.return":"party.parent.return");
                trace(enemy?"enemy_roster":"party_roster","return",roster_call,roster_entry);
                for(const auto& actor:roster.actors())if(actor->enemy==enemy) {
                    actor_snapshot(*actor,"before");
                    if(enemy)battle_enemy_first_update(*actor,root);else battle_party_first_update(*actor,root);
                    actor_snapshot(*actor,"after-first");
                    if(enemy)battle_enemy_second_update(*actor,root);else battle_party_second_update(*actor,root);
                    actor_snapshot(*actor,"after-second");
                    events.push_back((enemy?"enemy.child.":"party.child.")+std::to_string(actor->entity_id)+".return");
                }
            }
            const BattleUiFirstContext first_context{3,std::bit_cast<std::int32_t>(word(root.bytes,0x81ac)),roster.entities(),
                context.variables,&context.tables,&context.party,context.menu_sound,[&]{return context.random.next();}};
            ui->first_update(ui_resources[0],context.pad,context.input,60,&first_context);
            events.push_back("ui.first.return");
            if(!ui_path.empty())before=battle_ui_update_snapshot(*ui,context.shared,*ui_resources[0],context.variables,context.party,true);
            std::vector<const BattleActor*> enemies;
            for(const auto& actor:roster.actors())if(actor->enemy)enemies.push_back(actor.get());
            ui->second_update(*ui_resources[0],*hud_definitions,*command_definitions,
                {context.variables,context.tables,context.party,root,*battle_data,roster.entities(),enemies,3,&context.entry});
            events.push_back("ui.second.return");
            if(!ui_path.empty())after=battle_ui_update_snapshot(*ui,context.shared,*ui_resources[0],context.variables,context.party,true);
        }catch(const std::exception& e){error=e.what();failure=std::current_exception();}
        if(!report_path.empty()) {
            const auto array=[](std::ostream& out,const auto& values){out<<'[';bool comma=false;for(auto v:values){if(comma)out<<',';comma=true;out<<v;}out<<']';};
            std::ofstream out(report_path);
            out<<"{\"schema\":\"okage-native-battle-callbacks-v1\",\"frame\":"<<frame
               <<",\"complete\":"<<(failure?"false":"true")<<",\"error\":"<<std::quoted(error)
               <<",\"scope\":\"first eligible native parent and child traversal; no resource layout, rendering or combat\",\"root_hex\":"<<std::quoted(hex(root.bytes))
               <<",\"camera\":";array(out,camera->observation());
            out<<",\"fade\":";array(out,std::array{std::bit_cast<std::uint32_t>(context.fade.current),std::bit_cast<std::uint32_t>(context.fade.target),std::bit_cast<std::uint32_t>(context.fade.step),unsigned(context.fade.enabled),unsigned(context.fade.hold)});
            out<<",\"projection\":";std::vector<std::uint32_t> projection_bits;for(float f:field_projection_words(projection.current()))projection_bits.push_back(std::bit_cast<std::uint32_t>(f));array(out,projection_bits);
            out<<",\"stage_selector\":"<<stage_settings->selector<<",\"stage_signed_setting\":"<<stage_settings->signed_setting
               <<",\"roster_phases\":["<<party_phase<<','<<enemy_phase<<"],\"events\":[";
            for(unsigned i=0;i<events.size();++i){if(i)out<<',';out<<std::quoted(events[i]);}
            out<<"],\"actors\":[";for(unsigned i=0;i<actor_snapshots.size();++i){if(i)out<<',';out<<actor_snapshots[i];}out<<"]}\n";
            if(!out)throw std::runtime_error("Cannot write battle callbacks report");
        }
        if(!ui_path.empty()&&!after.empty()) {
            std::ofstream out(ui_path);
            out<<"{\"schema\":\"okage-native-ui-scheduled-update-v1\",\"scope\":\"first scheduled UI dispatchers after native parent, camera, party and enemy callbacks; no resource layout or rendering\",\"frame\":"<<frame
               <<",\"snapshots\":{\"before\":"<<before<<",\"after\":"<<after<<"}}\n";
            if(!out)throw std::runtime_error("Cannot write scheduled battle UI report");
        }
        if(failure)std::rethrow_exception(failure);
    }
    void report(const fs::path& path,unsigned frame,std::uint32_t rng_before,
                const BattleConstructionContext& context,const std::string& error) const {
        if(path.empty())return;
        if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing battle construction report");
        if(!path.parent_path().empty())fs::create_directories(path.parent_path());
        std::ofstream out(path);
        const auto hex=[](Bytes b){std::ostringstream s;for(auto v:b)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return s.str();};
        out<<"{\"schema\":\"okage-native-battle-construction-v1\",\"frame\":"<<frame
           <<",\"complete\":"<<(construction==Construction::Complete?"true":"false")
           <<",\"error\":"<<std::quoted(error)<<",\"rng_before\":"<<rng_before<<",\"rng_after\":"<<context.random.state
           <<",\"root_hex\":\""<<hex(root.bytes)<<"\",\"entry_hex\":\""<<hex(context.entry.bytes())<<"\",\"actors\":[";
        bool comma=false;
        const auto write_model=[&](const FieldActor* a){
            if(!a){out<<"null";return;}
            out<<"{\"has_motion\":"<<(a->has_motion()?"true":"false")<<",\"motion\":"<<a->motion()<<",\"pc\":"<<a->animation().pc()<<",\"script_state\":"<<a->animation().script_state()
               <<",\"flags\":"<<a->render_flags<<",\"visible\":"<<(a->visible?"true":"false")<<",\"channels\":[";
            bool separate=false;for(const auto& v:a->animation().channel_outputs())for(float f:v){if(separate)out<<',';separate=true;out<<std::bit_cast<std::uint32_t>(f);}
            out<<"]}";
        };
        for(const auto& owner:roster.actors()) {
            const auto& a=*owner;if(comma)out<<',';comma=true;
            out<<"{\"enemy\":"<<(a.enemy?"true":"false")<<",\"entity_id\":"<<a.entity_id
               <<",\"model_path\":"<<std::quoted(a.model_path.string())<<",\"shadow_bound\":"<<(a.bound_shadow?"true":"false")
               <<",\"child_hex\":\""<<hex(Bytes(a.storage).first(a.enemy?0x3e0:0x3d0))
               <<"\",\"record_before_hex\":\""<<hex(a.record_before)<<"\",\"record_hex\":\""<<hex(a.record)<<"\",\"random_draws\":[";
            for(unsigned j=0;j<a.constructor_random_draws.size();++j){if(j)out<<',';out<<a.constructor_random_draws[j];}
            out<<"],\"model\":";write_model(a.model.get());out<<",\"bag\":";write_model(a.bag.get());out<<",\"box\":";write_model(a.box.get());out<<'}';
        }
        out<<"],\"entities\":[";comma=false;
        for(const auto& entity:roster.entities()){
            if(comma)out<<',';comma=true;out<<"{\"kind\":"<<entity.kind<<",\"actor\":";
            if(entity.actor)out<<entity.actor->entity_id;else out<<"null";
            out<<",\"record_bytes\":"<<entity.record.size()<<'}';
        }
        out<<"]}\n";if(!out)throw std::runtime_error("Cannot write battle construction report");
    }
};
BattleSession::BattleSession(unsigned formation,int stage,const fs::path& input,
                            const fs::path& decoded,Bytes variables,BattleEntryContext& entry,Animation& camera):
    impl_(std::make_unique<Impl>(formation,stage,input,decoded,variables,entry,camera)) {}
BattleSession::~BattleSession()=default;
void BattleSession::tick(RetiringSceneEffects* retiring,FieldProjection& projection,SceneClearColor& clear,
                         BattleUiContext context,unsigned frame,const fs::path& stage_report,const fs::path& ui_update_report) {
    impl_->jobs.check();
    if(impl_->construction_failure)std::rethrow_exception(impl_->construction_failure);
    const bool active=!impl_->saved_model_range;
    if(active) {
        if(!impl_->objects->bindings().empty())throw std::runtime_error("Battle animated map resource ordering is unrecovered");
        for(const auto& actor:impl_->roster.actors())
            for(auto* model:{actor->model.get(),actor->bag.get(),actor->box.get()})if(model)model->tick_script();
    }
    impl_->camera_channels.tick_channels();
    impl_->resource_camera_position=impl_->camera_channels.value(0);
    impl_->resource_camera_rotation=impl_->camera_channels.value(1);
    const bool effect_pass=!impl_->suppress_effects.load();
    if(active) {
        projection.begin_field(impl_->stage_settings->cached_depth_min,impl_->stage_settings->cached_depth_max);
        for(const auto& actor:impl_->roster.actors())
            for(auto* model:{actor->model.get(),actor->bag.get(),actor->box.get()})if(model&&model->visible) {
                model->evaluate_bones();model->evaluate_world();
            }
        if(!effect_pass)throw std::runtime_error("Active battle resource has unexpected effect suppression");
        projection.begin_effects();projection.begin_deferred();projection.end_effects();
    } else projection.submit_empty_resource_frame(effect_pass,
        impl_->stage_settings->cached_depth_min,impl_->stage_settings->cached_depth_max);
    if(effect_pass&&retiring)retiring->tick();
    if(active&&retiring&&(std::any_of(retiring->map.begin(),retiring->map.end(),[](const auto& p){return bool(p);})||
                         std::any_of(retiring->attached.begin(),retiring->attached.end(),[](const auto& p){return bool(p);})))
        throw std::runtime_error("Battle resource retains effects requiring native submission");
    impl_->prepare_resource_ui(context);
    if(impl_->construction!=Impl::Construction::Complete)return;
    if(!active&&impl_->ui_draw)throw std::runtime_error("Saved battle resource unexpectedly has queued UI");
    impl_->renderer->set_sort_depth_shift(impl_->stage_settings->bucket_base);
    const auto& p=impl_->resource_camera_position;const auto& r=impl_->resource_camera_rotation;
    const Float4 rotation{-r[0],-r[1],-r[2],0};
    const auto view=camera_matrix({-p[0],-p[1],-p[2],1},rotation,{1,1,1,0},context.sine);
    const auto& screen=projection.frame().ordinary;
    const auto prepared=active?impl_->renderer->prepare(view,field_clip_projection(screen),nullptr,rotation,
        screen.clip_far,p,&projection.frame(),1,false):impl_->renderer->prepare_empty(projection.frame());
    if(!impl_->stage_committed) {
        try {impl_->commit_stage(projection,clear,frame,stage_report);}
        catch(...) {impl_->construction_failure=std::current_exception();throw;}
    }
    if(!impl_->ui) {
        try {impl_->construct_ui(context,frame,stage_report);}
        catch(...) {impl_->construction_failure=std::current_exception();throw;}
    }
    if(active&&!impl_->reported_active_resource&&impl_->ui_draw&&!stage_report.empty()) {
        auto path=stage_report;path.replace_extension(".first-active.draw.json");
        if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing actual battle draw report");
        std::ofstream out(path);
        write_ui_draw_diagnostic(out,*impl_->ui_draw,frame,104376,127,"actual-session-first-resource-after-callbacks");
        if(!out)throw std::runtime_error("Cannot write actual battle draw report");
    }
    if(!active)impl_->inspect_ui_updates(context,frame,ui_update_report);
    impl_->scheduled_callbacks(context,projection,frame,active?fs::path{}:stage_report);
    for(unsigned slot=3;slot<impl_->stage_settings->filters.size();++slot) {
        const auto& filter=impl_->stage_settings->filters[slot];
        if(slot!=6&&filter&&(*filter)[0])throw std::runtime_error("Unrecovered battle framebuffer filter slot");
    }
    SceneGpuPass postprocess{[&](id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth) {
        const auto& filter=impl_->stage_settings->filters[6];
        if(filter&&(*filter)[0]) {
            impl_->filter_renderer->encode(command,color,depth,{(*filter)[1],(*filter)[2],
                std::bit_cast<std::int32_t>(impl_->stage_settings->signed_setting),true});
        }
        return std::function<void()>([]{});
    }};
    const UiMetalBaseline baseline{0x6c00,0x7900,true,0,1,0,1};
    const UiMetalTextureResolver textures=[&](const UiGeometryTexture& texture) {
        if(texture.kind==UiGeometryTexture::Kind::Font) {
            if(texture.fonts!=impl_->font_owner||texture.font_index>=3)throw std::runtime_error("Battle UI font binding mismatch");
            return UiMetalTexture{impl_->font_textures[texture.font_index],impl_->font_owner};
        }
        if(texture.window!=impl_->window_owner)throw std::runtime_error("Battle UI window binding mismatch");
        return UiMetalTexture{impl_->atlas,impl_->window_owner};
    };
    std::optional<SceneUiPass> ui_pass;
    if(impl_->ui_geometry)ui_pass.emplace(SceneUiPass{*impl_->ui_geometry,baseline,textures});
    impl_->rendered_image=impl_->renderer->render_prepared(prepared,ui_pass?&*ui_pass:nullptr,&postprocess,
        std::array<std::uint8_t,4>{clear.rgb[0],clear.rgb[1],clear.rgb[2],0});
    if(!stage_report.empty()&&(!active||!impl_->reported_active_resource)) {
        auto path=stage_report;path.replace_extension(active?".first-active.render.rgba":".render.rgba");
        if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing battle resource image");
        std::ofstream out(path,std::ios::binary);const auto& rgba=impl_->rendered_image->rgba;
        out.write(reinterpret_cast<const char*>(rgba.data()),rgba.size());out.close();
        if(!out)throw std::runtime_error("Cannot write battle resource image");
        std::cout<<"Native battle resource/filter completed frame="<<frame<<" raw_rgba="<<path<<'\n';
    }
    if(active)impl_->reported_active_resource=true;
}
void BattleSession::service_loader_after_frame(BattleConstructionContext context,unsigned frame,const fs::path& report){
    impl_->jobs.check();
    if(!impl_->loader_resumed){impl_->jobs.resume();impl_->loader_resumed=true;return;}
    if(impl_->construction_failure)std::rethrow_exception(impl_->construction_failure);
    if(impl_->construction!=Impl::Construction::Preparing||!impl_->resources_prepared.load(std::memory_order_acquire))return;
    if(impl_->jobs.pending()!=0)return;
    impl_->construction=Impl::Construction::Constructing;
    const auto rng_before=context.random.state;
    std::string error;
    try {
        impl_->camera_channels.set_channel_mode(0,0x10);
        impl_->camera.emplace(*impl_->camera_sequence,1.f);impl_->load_step.store(4);
        const auto random=[&context]{return context.random.next();};
        impl_->roster.construct_party(impl_->decoded,context.variables,context.tables,context.entry,random);
        impl_->load_step.store(5);
        impl_->roster.construct_enemies(impl_->decoded,impl_->formation,impl_->battle_data->formation(impl_->formation),context.tables,
            std::bit_cast<std::int16_t>(context.entry.parameters().argument2),random);
        unsigned alias=0;
        for(const auto& actor:impl_->roster.actors()) {
            const auto bind=[&](const std::unique_ptr<FieldActor>& model,const fs::path& path) {
                if(model)impl_->renderer->add_actor({model.get(),path,int(alias++),SceneActorDescriptor{model->render_flags,false}});
            };
            bind(actor->model,actor->model_path);bind(actor->bag,actor->bag_path);bind(actor->box,actor->box_path);
        }
        impl_->load_step.store(6);impl_->construction=Impl::Construction::Complete;
    }catch(const std::exception& e){
        impl_->construction=Impl::Construction::Failed;impl_->construction_failure=std::current_exception();error=e.what();
    }
    impl_->report(report,frame,rng_before,context,error);
    if(impl_->construction_failure)std::rethrow_exception(impl_->construction_failure);
}
const BattleState& BattleSession::state() const{return impl_->root;}
const Texture* BattleSession::rendered_image() const{return impl_->rendered_image?&*impl_->rendered_image:nullptr;}
std::array<std::uint32_t,7> BattleSession::observation() const {
    return {impl_->phase,unsigned(impl_->construction!=Impl::Construction::Complete),impl_->load_step.load(),
        unsigned(impl_->suppress_effects.load()),1,0x40d,0x40c};
}
}
