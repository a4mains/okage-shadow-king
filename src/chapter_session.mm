#include "chapter_session.hpp"
#include "image_metal.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
namespace okage {
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
SceneClearColor& initialize(std::span<std::uint8_t> variables,
    const std::array<std::uint32_t,3>& command,GameModeSelection destination,
    SceneClearColor& clear,SceneFade& fade) {
    check(command[0]==14&&command[1]>=1&&command[1]<=7&&command[2]==0&&destination[0]==2,
        "Chapter constructor requires command14 and owned chapter1..7");
    check(variables.size()>=0x110,"Chapter constructor variable bank0 extent");
    //125ad0 precedes114c80 and allocation of resource221980.
    std::fill(variables.begin()+0x10,variables.begin()+0x110,0);
    fade.immediate(0);return clear;
}
constexpr ImageMetalBaseline baseline{0x6c00,0x7900,0,639,0,223,0,1,0,1};
}
struct ChapterSession::Impl {
    ChapterState task;
    EmptySceneResource resource;
    id<MTLDevice> device;
    id<MTLCommandQueue> commands;
    id<MTLTexture> texture,color,depth;
    std::unique_ptr<ImageMetal> renderer;
    ImageMetalQuad quad{};
    Texture output{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
    bool ready=false;
    Impl(std::span<std::uint8_t> variables,const std::array<std::uint32_t,3>& command,
        GameModeSelection destination,SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,
        const std::function<std::vector<std::uint8_t>(unsigned)>& load)
        :task(command[1],destination[1],destination[2]),
         resource(initialize(variables,command,destination,clear,fade),fade,ui) {
        check(bool(load),"Chapter image loader missing");
        const auto bytes=load(command[1]);const auto image=decode_xim_gs32(bytes);
        check(image.width==512&&image.height==192&&image.pixel_format==19&&image.palette_format==0,
            "Chapter requires authored512x192 PSMT8/PSMCT32 image");
        quad=axis_aligned_image_quad(image.width,image.height,{0,0,0x3f800000,0x3f800000,0,0x3f800000});
        device=MTLCreateSystemDefaultDevice();check(device,"Chapter Metal device");
        commands=[device newCommandQueue];check(commands,"Chapter command queue");
        texture=upload_image_gs_source(device,bytes);renderer=std::make_unique<ImageMetal>(device);
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;color=[device newTextureWithDescriptor:d];
        d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;depth=[device newTextureWithDescriptor:d];
        check(color&&depth,"Chapter target allocation");
        clear.rgb={0,0,0};fade.request(1);
    }
    void render() {
        auto command=[commands commandBuffer];check(command,"Chapter command allocation");
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,64./255.);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;
        pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1;
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];check(encoder,"Chapter clear encoder");[encoder endEncoding];
        auto validation=renderer->encode(command,color,depth,quad,baseline,{texture,{}});
        [command commit];[command waitUntilCompleted];validation.validate_completed(command);
        [color getBytes:output.rgba.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];ready=true;
        //Global layer2 fade is applied by the Session after this image.
    }
};
ChapterSession::ChapterSession(std::span<std::uint8_t> variables,const std::array<std::uint32_t,3>& command,
    GameModeSelection destination,SceneClearColor& clear,SceneFade& fade,BattleUiShared& ui,
    const std::function<std::vector<std::uint8_t>(unsigned)>& load)
    :impl_(std::make_unique<Impl>(variables,command,destination,clear,fade,ui,load)){}
ChapterSession::~ChapterSession()=default;
void ChapterSession::tick(std::array<std::uint32_t,3>& command,SceneFade& fade,Animation& camera,
    FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& ui,
    std::shared_ptr<const std::vector<Font>> fonts,const SineTable& sine,GameRandom& random,
    std::uint32_t& image_frame_counter,const std::function<void(GameModeSelection)>& request,
    const std::function<void()>& updated) {
    check(resource_live(),"Dead chapter callback");
    unsigned submitted=0;
    chapter_update(impl_->task,{[&](const GameOverImageCall& call){
        check(call.a0==0&&call.fp_words==std::array<std::uint32_t,6>{0,0,0x3f800000,0x3f800000,0,0x3f800000},
            "Chapter unrecovered image transform");++submitted;
    },[&](float value){fade.request(value);},[&]{return fade.current!=fade.target;},[&]{command={};},request});
    check(submitted==1,"Chapter image queue ownership");
    if(updated)updated();
    //100f90 -> 100800 -> 1b5ee0 -> 1b3450: 1009e0 set resource+0x24 when CHP_01 loaded.
    ++image_frame_counter;
    impl_->resource.tick(camera,projection,effects,ui,std::move(fonts),sine,random);
    impl_->render();
}
void ChapterSession::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,
    BattleUiShared& ui,SceneFade& fade) {
    impl_->resource.retire(camera,projection,effects,ui);impl_->texture=nil;impl_->ready=false;
    //133a38 flag1 skips ordinary field releases, then restores fade rate.
    fade.step=std::bit_cast<float>(0x3ca3d70au);
}
bool ChapterSession::resource_live() const{return impl_->resource.live();}
const Texture* ChapterSession::rendered_image() const{return resource_live()&&impl_->ready?&impl_->output:nullptr;}
const ChapterState& ChapterSession::state() const{return impl_->task;}
}
