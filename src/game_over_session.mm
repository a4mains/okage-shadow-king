#include "game_over_session.hpp"
#include "image_metal.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
namespace okage {
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
Texture decode_game_over(Bytes bytes) {
    auto image=decode_xim(bytes);
    check(image.width==640&&image.height==448&&image.pixel_format==19&&image.palette_format==0,
        "Game-over image requires recovered640x448 PSMT8/PSMCT32 source");
    constexpr std::size_t payload=16+16+256*4+16;
    check(bytes.size()==payload+640*448,"Game-over source palette extent");
    for(auto index:bytes.subspan(payload))
        check(bytes[32+unsigned(index)*4+3]<=128,"Game-over preview cannot recover source alpha above128");
    return image;
}
constexpr ImageMetalBaseline baseline{0x6c00,0x7900,0,639,0,223,0,1,0,1};
}
struct GameOverSession::Impl {
    //Scheduler allocation clears generic alpha/rate before12c350 runs.
    GameOverTaskState task{4,0,0,0,0,0,0};
    EmptySceneResource resource;
    std::array<std::optional<GameOverImageCall>,512> queue{};
    id<MTLDevice> device;
    id<MTLCommandQueue> commands;
    id<MTLTexture> texture,color,depth;
    std::unique_ptr<ImageMetal> renderer;
    ImageMetalQuad image_quad{};
    Texture output{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
    bool ready=false;
    Impl(SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,GameOverShared& shared,GameOverConstruction services)
        :resource(clear,fade,ui) {
        fade.immediate(0);clear.rgb={0,0,0};
        check(bool(services.stop_music)&&bool(services.load_image),"Game-over constructor services");
        services.stop_music(); //12d470, before SCR.XPF loading.
        const auto bytes=services.load_image();const auto image=decode_game_over(bytes);
        image_quad=axis_aligned_image_quad(image.width,image.height,
            {0,0,0x3f800000,0x3f800000,0,0x3f800000});
        device=MTLCreateSystemDefaultDevice();check(device,"Game-over Metal device");
        commands=[device newCommandQueue];check(commands,"Game-over command queue");
        texture=upload_image_gs_texture(device,image,ImageMetalPreviewAlpha::SourceAtMost128);
        renderer=std::make_unique<ImageMetal>(device);
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
        color=[device newTextureWithDescriptor:descriptor];
        descriptor.pixelFormat=MTLPixelFormatDepth32Float;descriptor.storageMode=MTLStorageModePrivate;
        depth=[device newTextureWithDescriptor:descriptor];check(color&&depth,"Game-over target allocation");
        shared.initialize_mode();
        fade.request(1);fade.immediate(0); //Both authored calls; final target is0.
    }
    void submit(const GameOverImageCall& call) {
        check(call.a0==0&&call.fp_words==std::array<std::uint32_t,6>{0,0,0x3f800000,0x3f800000,0,0x3f800000},
            "Game-over image submission outside recovered descriptor0 transform");
        //1b2d70 first free slot, silent drop on full; these are one-pass entries.
        for(auto& slot:queue)if(!slot){slot=call;return;}
    }
    void render(unsigned count) {
        auto command=[commands commandBuffer];check(command,"Game-over command allocation");
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        //Clear+early0x204 alpha initialization precedes imageOT40d. Empty
        //resource/effect epilogues at40f do not clear the image a second time.
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,64./255.);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;
        pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1;
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];check(encoder,"Game-over clear encoder");[encoder endEncoding];
        std::vector<ImageMetalValidation> validations;
        for(unsigned i=0;i<count;++i)validations.push_back(renderer->encode(command,color,depth,image_quad,baseline,{texture,{}}));
        [command commit];[command waitUntilCompleted];
        check(command.status==MTLCommandBufferStatusCompleted,"Game-over rendering failed");
        for(const auto& validation:validations)validation.validate_completed(command);
        [color getBytes:output.rgba.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];ready=true;
        //Global layer2 fade is applied by Session AFTER this image, as OT40e.
    }
};
GameOverSession::GameOverSession(SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,GameOverShared& shared,GameOverConstruction services)
    :impl_(std::make_unique<Impl>(clear,fade,ui,shared,std::move(services))){}
GameOverSession::~GameOverSession()=default;
void GameOverSession::tick(GameOverPadSample pad,GameOverShared& shared,SceneFade& fade,Animation& camera,
    FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& ui,
    std::shared_ptr<const std::vector<Font>> fonts,const SineTable& sine,GameRandom& random,
    const std::function<void(GameModeSelection)>& request) {
    check(impl_->resource.live(),"Dead game-over task callback");
    game_over_submode4_update(impl_->task,shared.input,pad,{[&](float value){fade.immediate(value);},request,
        [&](const auto& call){impl_->submit(call);}});
    ++shared.image_frame_counter; //1b3450 prologue; wrapping source ADDIU.
    unsigned count=0;for(auto& slot:impl_->queue)if(slot){++count;slot.reset();}
    impl_->resource.tick(camera,projection,effects,ui,std::move(fonts),sine,random);
    impl_->render(count);
}
void GameOverSession::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& ui) {
    impl_->resource.retire(camera,projection,effects,ui);
    impl_->queue={};impl_->texture=nil;impl_->ready=false;
    //12c8d8 has no task-local action for submode4; child resource is distinct.
}
bool GameOverSession::resource_live() const{return impl_->resource.live();}
const Texture* GameOverSession::rendered_image() const{return resource_live()&&impl_->ready?&impl_->output:nullptr;}
const GameOverTaskState& GameOverSession::state() const{return impl_->task;}
}
