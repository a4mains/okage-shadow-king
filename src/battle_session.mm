#include "battle_session.hpp"
#include "okage/battle_data.hpp"
#include "okage/load_queue.hpp"
#include "okage/ruw.hpp"
#include "okage/map_objects.hpp"
#include "okage/collision.hpp"
#include "okage/event.hpp"
#include "okage/animation.hpp"
#include "okage/scene_renderer.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <fstream>
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
    unsigned phase=0;
    bool loader_resumed=false;
    id<MTLDevice> device;
    id<MTLTexture> atlas,environment;
    Float4 environment_uv{};
    std::vector<std::uint8_t> held_stage;
    std::unique_ptr<MapObjects> objects;
    std::optional<CollisionMap> collision;
    std::unique_ptr<SceneRenderer> renderer;
    Animation& camera_channels;
    std::optional<BattleCameraMotion> camera;
    // Last member: its destructor joins before any job-captured owners die.
    NativeLoadQueue jobs;
    Impl(unsigned f,int s,fs::path i,fs::path d,Bytes variables,BattleEntryContext& entry,Animation& channels):
        formation(f),stage(s),input(std::move(i)),decoded(std::move(d)),
        alternate_interface(variables.size()>12&&variables[12]!=0),camera_channels(channels) {
        if(variables.size()<0xd78)throw std::runtime_error("Battle shared variable extent");
        if(event_variable(variables,0x40005)!=0)
            throw std::runtime_error("Battle wrapper bypass branch is unrecovered");
        root.initialize_root({1.f,formation,stage});entry.clear_prefix();
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
            const BattleData data(read(input/"battle-data.bin"));
            (void)data.formation(int(formation));
            const auto package=archive_path(decoded,data.stage_path(stage));
            suppress_effects.store(true);
            if(fs::exists(package/"add.afd"))
                throw std::runtime_error("Battle stage additional effects are unrecovered");
            const auto map=package/"a.xmp";
            objects=std::make_unique<MapObjects>(map);
            renderer=std::make_unique<SceneRenderer>(map,std::span<const SceneActor>{},objects.get());
            held_stage=read(package/"a.stg");
            collision=read_xcl(read(package/"a.xcl"));
            load_step.store(3);
            camera_channels.set_channel_mode(0,0x10);
            const BattleCameraData camera_data(read(input/"battle-camera.bin"));
            camera.emplace(camera_data.sequence(stage),1.f);
            load_step.store(4);
            throw std::runtime_error("Native battle party constructor 0x153618 is unrecovered; UI, stage and camera resources loaded, job incomplete");
        }
    }
};
BattleSession::BattleSession(unsigned formation,int stage,const fs::path& input,
                            const fs::path& decoded,Bytes variables,BattleEntryContext& entry,Animation& camera):
    impl_(std::make_unique<Impl>(formation,stage,input,decoded,variables,entry,camera)) {}
BattleSession::~BattleSession()=default;
void BattleSession::tick(RetiringSceneEffects* retiring) {
    impl_->jobs.check();
    if(!impl_->suppress_effects.load()&&retiring)retiring->tick();
    if(impl_->jobs.pending())return;
    // No successful empty party/enemy or UI object may open this gate.
    throw std::runtime_error("Battle loading commit/UI constructors are unrecovered");
}
void BattleSession::resume_loader(){
    if(!impl_->loader_resumed){impl_->jobs.resume();impl_->loader_resumed=true;}
}
const BattleState& BattleSession::state() const{return impl_->root;}
std::array<std::uint32_t,7> BattleSession::observation() const {
    return {impl_->phase,unsigned(impl_->jobs.pending()),impl_->load_step.load(),
        unsigned(impl_->suppress_effects.load()),1,0x40d,0x40c};
}
}
