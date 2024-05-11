#include "startup_session.hpp"
#include "image_metal.hpp"
#include "okage/load_queue.hpp"
#include <algorithm>
#include <climits>
#include <cerrno>
#include <system_error>
#include <unistd.h>

namespace okage {
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
unsigned probe_storage(const std::filesystem::path& directory) {
    check(!directory.empty(),"Native save directory is missing");
    std::filesystem::create_directories(directory);
    std::string pattern=(directory/".okage-write-check-XXXXXX").string();
    const int fd=mkstemp(pattern.data());
    if(fd<0)throw std::system_error(errno,std::generic_category(),"Native save storage create");
    const char marker=0;
    const bool written=write(fd,&marker,1)==1;const int write_error=errno;
    const int closed=close(fd);const int close_error=errno;
    const int removed=unlink(pattern.c_str());const int remove_error=errno;
    if(!written)throw std::system_error(write_error,std::generic_category(),"Native save storage write");
    if(closed)throw std::system_error(close_error,std::generic_category(),"Native save storage close");
    if(removed)throw std::system_error(remove_error,std::generic_category(),"Native save storage probe cleanup");
    const auto available=std::filesystem::space(directory).available;
    check(available!=std::uintmax_t(-1),"Native save storage capacity unavailable");
    return unsigned(std::min<std::uintmax_t>(available/1024,INT_MAX));
}
constexpr ImageMetalBaseline baseline{0x6c00,0x7900,0,639,0,223,0,1,0,1};
}
struct StartupSession::Impl {
    StartupTaskState task{};
    EmptySceneResource resource;
    StartupConstruction construction;
    unsigned storage_state=0,free_units=0;
    bool storage_complete=false,ready=false;
    id<MTLCommandQueue> commands;
    std::array<id<MTLTexture>,2> images{};
    std::array<ImageMetalQuad,2> quads{};
    id<MTLTexture> color,depth;
    std::unique_ptr<ImageMetal> renderer;
    Texture output{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
    NativeLoadQueue jobs;
    Impl(SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,GameOverShared& shared,StartupConstruction c)
        :resource(clear,fade,ui),construction(std::move(c)) {
        check(construction.selector==0||construction.selector==3,"Unsupported startup selector");
        task.common.submode=construction.selector;fade.immediate(0);
        check(bool(construction.stop_music),"Startup music owner missing");construction.stop_music();
        if(construction.selector==0) {
            check(bool(construction.load_image),"Startup logo loader missing");
            auto device=MTLCreateSystemDefaultDevice();check(device,"Startup Metal device");
            commands=[device newCommandQueue];renderer=std::make_unique<ImageMetal>(device);
            for(unsigned i=0;i<2;++i) {
                const auto bytes=construction.load_image(i);const auto image=decode_xim_gs32(bytes);
                quads[i]=axis_aligned_image_quad(image.width,image.height,{0,0,0x3f800000,0x3f800000,0,0x3f800000});
                images[i]=upload_image_gs_source(device,bytes);
            }
            auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
            d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;color=[device newTextureWithDescriptor:d];
            d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;depth=[device newTextureWithDescriptor:d];
            check(commands&&color&&depth,"Startup Metal targets");
        }
        shared.initialize_mode();fade.request(1);fade.immediate(0);
    }
    void storage_tick(const std::function<void(GameModeSelection)>& request) {
        if(storage_state==0) {
            jobs.enqueue([this]{free_units=probe_storage(construction.save_directory);},{0x16c490,0x12b508});
            jobs.resume();storage_state=0x687; //Real asynchronous poll continuation.
        }
        if(storage_state==0x687) {
            jobs.check();if(jobs.pending())return;
            storage_complete=true;
            check(free_units>=150,"Native storage below150KiB: startup low-space UI is unrecovered");
            request({8,0,0});storage_state=0x69a;
        } else check(storage_state==0x69a,"Startup storage continuation");
    }
    void render(std::optional<unsigned> index) {
        auto command=[commands commandBuffer];check(command,"Startup command allocation");
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,64./255.);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;
        pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1;
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];check(encoder,"Startup clear encoder");[encoder endEncoding];
        std::optional<ImageMetalValidation> validation;
        if(index)validation=renderer->encode(command,color,depth,quads.at(*index),baseline,{images.at(*index),{}});
        [command commit];[command waitUntilCompleted];if(validation)validation->validate_completed(command);
        check(command.status==MTLCommandBufferStatusCompleted,"Startup Metal submission failed");
        [color getBytes:output.rgba.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];ready=true;
    }
};
StartupSession::StartupSession(SceneClearColor& c,SceneFade& f,BattleUiShared& u,GameOverShared& g,StartupConstruction s)
    :impl_(std::make_unique<Impl>(c,f,u,g,std::move(s))){}
StartupSession::~StartupSession()=default;
void StartupSession::tick(GameOverPadSample pad,GameOverShared& shared,SceneFade& fade,Animation& camera,
    FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& ui,
    std::shared_ptr<const std::vector<Font>> fonts,const SineTable& sine,GameRandom& random,
    const std::function<void(GameModeSelection)>& request) {
    auto& p=*impl_;check(resource_live(),"Dead startup callback");std::optional<unsigned> image;
    const GameOverUpdateServices services{[&](float v){fade.immediate(v);},request,[&](const GameOverImageCall& call){
        check(call.a0<2&&!image,"Startup image queue identity");image=call.a0;
    }};
    if(p.task.common.submode==3) {
        mode8_update_prefix(p.task.common,shared.input,pad,services);fade.immediate(1);p.storage_tick(request);
    } else startup_logo_update(p.task,shared.input,pad,{services,p.construction.initialize_shared});
    if(p.task.common.submode==0)++shared.image_frame_counter;
    p.resource.tick(camera,projection,effects,ui,std::move(fonts),sine,random);
    if(p.task.common.submode==0)p.render(image);
}
void StartupSession::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& ui) {
    impl_->resource.retire(camera,projection,effects,ui);impl_->ready=false;
}
bool StartupSession::resource_live() const{return impl_->resource.live();}
const Texture* StartupSession::rendered_image() const {
    if(!resource_live())return nullptr;
    return impl_->task.common.submode==3?impl_->resource.rendered_image():impl_->ready?&impl_->output:nullptr;
}
const StartupTaskState& StartupSession::state() const{return impl_->task;}
std::array<std::uint32_t,4> StartupSession::storage_observation() const {
    return {impl_->storage_state,unsigned(impl_->storage_complete),impl_->storage_complete?2u:0u,
        impl_->storage_complete?impl_->free_units:0u};
}
}
