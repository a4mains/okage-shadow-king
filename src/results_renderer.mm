#include "results_renderer.hpp"
#include "ui_metal.hpp"
#include <stdexcept>

namespace okage {
namespace {
void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
}
struct ResultsRenderer::Impl {
    std::shared_ptr<const std::vector<Font>> fonts;
    std::shared_ptr<const Texture> window_atlas;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLTexture> color,depth,window_texture;
    std::vector<id<MTLTexture>> font_textures;
    std::unique_ptr<UiMetal> renderer;
    Texture image{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};

    Impl(std::shared_ptr<const std::vector<Font>> font_owner,
         std::shared_ptr<const Texture> atlas_owner)
        :fonts(std::move(font_owner)),window_atlas(std::move(atlas_owner)) {
        require(fonts&&!fonts->empty()&&window_atlas,"Results renderer requires owned fonts and window atlas");
        device=MTLCreateSystemDefaultDevice();
        require(bool(device),"Results renderer Metal device unavailable");
        queue=[device newCommandQueue];
        require(bool(queue),"Results renderer command queue unavailable");
        renderer=std::make_unique<UiMetal>(device);
        for(const auto& font:*fonts)font_textures.push_back(upload_ui_gs_texture(device,font.atlas));
        window_texture=upload_ui_gs_texture(device,*window_atlas);
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:640 height:224 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;
        descriptor.usage=MTLTextureUsageRenderTarget;
        color=[device newTextureWithDescriptor:descriptor];
        descriptor.pixelFormat=MTLPixelFormatDepth32Float;
        descriptor.storageMode=MTLStorageModePrivate;
        depth=[device newTextureWithDescriptor:descriptor];
        require(color&&depth,"Results renderer target allocation failed");
    }
    const Texture& render(const std::optional<UiDrawList>& draw,const SceneFade& fade) {
        std::optional<UiGeometryList> geometry;
        if(draw)geometry=resolve_ui_geometry(*draw);
        auto command=[queue commandBuffer];
        require(bool(command),"Results renderer command buffer unavailable");
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        Texture background{1,1,0,0,{0,0,0,64}};
        fade.apply(background);
        const auto& rgba=background.rgba;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(
            rgba[0]/255.0,rgba[1]/255.0,rgba[2]/255.0,rgba[3]/255.0);
        pass.depthAttachment.texture=depth;
        pass.depthAttachment.loadAction=MTLLoadActionClear;
        pass.depthAttachment.storeAction=MTLStoreActionStore;
        pass.depthAttachment.clearDepth=1;
        auto clear=[command renderCommandEncoderWithDescriptor:pass];
        require(bool(clear),"Results renderer clear encoder unavailable");
        [clear endEncoding];
        std::optional<UiMetalValidation> validation;
        if(geometry) {
            const UiMetalBaseline baseline{0x6c00,0x7900,!fade.enabled,0,1,0,1,0xdf0000027f0000ull};
            validation=renderer->encode(command,color,depth,*geometry,baseline,
                [this](const UiGeometryTexture& texture)->UiMetalTexture {
                    if(texture.kind==UiGeometryTexture::Kind::Font) {
                        require(texture.fonts==fonts&&texture.font_index<font_textures.size(),
                            "Results renderer font ownership mismatch");
                        return {font_textures[texture.font_index],fonts};
                    }
                    require(texture.window==window_atlas,"Results renderer window ownership mismatch");
                    return {window_texture,window_atlas};
                });
        }
        [command commit];[command waitUntilCompleted];
        require(command.status==MTLCommandBufferStatusCompleted,"Results renderer GPU command failed");
        if(validation)validation->validate_completed(command);
        [color getBytes:image.rgba.data() bytesPerRow:640*4
            fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];
        return image;
    }
};
ResultsRenderer::ResultsRenderer(std::shared_ptr<const std::vector<Font>> fonts,
    std::shared_ptr<const Texture> window_atlas) {
    @autoreleasepool {impl_=std::make_unique<Impl>(std::move(fonts),std::move(window_atlas));}
}
ResultsRenderer::~ResultsRenderer()=default;
const Texture& ResultsRenderer::render(const std::optional<UiDrawList>& draw,const SceneFade& fade) {
    @autoreleasepool {return impl_->render(draw,fade);}
}
}
