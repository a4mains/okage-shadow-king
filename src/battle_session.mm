#include "battle_session.hpp"
#include "okage/battle_data.hpp"
#include "okage/load_queue.hpp"
#include "okage/ruw.hpp"
#include "okage/map_objects.hpp"
#include "okage/collision.hpp"
#include "okage/event.hpp"
#include "okage/animation.hpp"
#include "okage/scene_renderer.hpp"
#include "okage/battle_actor.hpp"
#include "okage/stage_settings.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

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
}
struct BattleSession::Impl {
    unsigned formation;
    int stage;
    fs::path input,decoded;
    bool alternate_interface;
    BattleState root;
    StageScratch scratch;
    ShadowScreenParameters ui_projection=initial_ui_projection();
    std::array<std::unique_ptr<RuwResource>,16> ui_resources;
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
    Animation& camera_channels;
    std::optional<BattleCameraMotion> camera;
    std::optional<BattleCameraSequence> camera_sequence;
    std::optional<BattleData> battle_data;
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
            ui_resources[0]=std::make_unique<RuwResource>(read(input/"BTL.RUW"));
            load_step.store(1);
            device=MTLCreateSystemDefaultDevice();
            if(!device)throw std::runtime_error("Battle loader requires Metal");
            const auto main=decoded/"CMNDATA/2D/MAIN.XPF";
            auto atlas_bytes=read(main/(alternate_interface?"bm_ifj.xim":"bm_if.xim"));
            // 18f500 sets image selector bits20..25 before image/CLUT upload.
            auto header=word(atlas_bytes,0);header=(header&0xfc0fffff)|(0x1bu<<20);
            for(unsigned j=0;j<4;++j)atlas_bytes[j]=header>>(8*j);
            atlas=upload(decode_xim(atlas_bytes));
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
            const BattleCameraData camera_data(read(input/"battle-camera.bin"));
            camera_sequence=camera_data.sequence(stage);
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
            std::vector<std::uint32_t> result;for(float v:values)result.push_back(__builtin_bit_cast(std::uint32_t, (v)));return result;
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
            for(const auto& v:s.lighting.colors)for(float f:v)colors.push_back(__builtin_bit_cast(std::uint32_t, (f)));
            for(const auto& v:s.lighting.directions)for(float f:v)directions.push_back(__builtin_bit_cast(std::uint32_t, (f)));
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
            bool separate=false;for(const auto& v:a->animation().channel_outputs())for(float f:v){if(separate)out<<',';separate=true;out<<__builtin_bit_cast(std::uint32_t, (f));}
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
                         unsigned frame,const fs::path& stage_report) {
    impl_->jobs.check();
    if(impl_->construction_failure)std::rethrow_exception(impl_->construction_failure);
    const bool effect_pass=!impl_->suppress_effects.load();
    projection.submit_empty_resource_frame(effect_pass,
        impl_->stage_settings->cached_depth_min,impl_->stage_settings->cached_depth_max);
    if(effect_pass&&retiring)retiring->tick();
    if(impl_->construction!=Impl::Construction::Complete)return;
    if(!impl_->stage_committed) {
        try {impl_->commit_stage(projection,clear,frame,stage_report);}
        catch(...) {impl_->construction_failure=std::current_exception();throw;}
    }
    throw std::runtime_error("Battle UI constructors, child activation and framebuffer filter rendering are unrecovered");
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
            __builtin_bit_cast(std::int16_t, (context.entry.parameters().argument2)),random);
        impl_->load_step.store(6);impl_->construction=Impl::Construction::Complete;
    }catch(const std::exception& e){
        impl_->construction=Impl::Construction::Failed;impl_->construction_failure=std::current_exception();error=e.what();
    }
    impl_->report(report,frame,rng_before,context,error);
    if(impl_->construction_failure)std::rethrow_exception(impl_->construction_failure);
}
const BattleState& BattleSession::state() const{return impl_->root;}
std::array<std::uint32_t,7> BattleSession::observation() const {
    return {impl_->phase,unsigned(impl_->construction!=Impl::Construction::Complete),impl_->load_step.load(),
        unsigned(impl_->suppress_effects.load()),1,0x40d,0x40c};
}
}
