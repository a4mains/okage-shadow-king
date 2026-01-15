#pragma once
#import <Metal/Metal.h>
#include "okage/assets.hpp"
#include "okage/image_quad.hpp"
#include "okage/gs_texture_sampling.hpp"
#include <array>
#include <memory>

namespace okage {
using ImageMetalQuad=ImageQuad;
// Inherited source environment; SCISSOR endpoints are inclusive GS pixels.
// TEST0x0f disables depth test/writes regardless of the inherited ZBUF mask.
struct ImageMetalBaseline {
    std::uint32_t offset_x16,offset_y16;
    std::uint32_t scissor_x0,scissor_x1,scissor_y0,scissor_y1;
    std::uint32_t framebuffer_mask,colclamp,dither,prmodecont;
};
struct ImageMetalTexture {
    id<MTLTexture> texture;
    std::shared_ptr<const void> owner;
};
enum class ImageMetalPreviewAlpha { SourceAtMost128 };
struct ImageMetalGsTexture {
    Ps2TextureSnapshot snapshot;
    GsTextureRegisters registers;
    std::shared_ptr<const void> owner;
};
struct ImageMetalGsSnapshot;
id<MTLTexture> upload_image_gs_texture(id<MTLDevice>,const Texture&,ImageMetalPreviewAlpha);
id<MTLTexture> upload_image_gs_source(id<MTLDevice>,Bytes);
class ImageMetalValidation {
public:
    void validate_completed(id<MTLCommandBuffer>) const;
private:
    friend class ImageMetal;
    ImageMetalValidation(id<MTLCommandBuffer>,id<MTLBuffer>,ImageMetalTexture,std::shared_ptr<const ImageMetalGsSnapshot> = {});
    id<MTLCommandBuffer> command_;
    id<MTLBuffer> errors_;
    ImageMetalTexture texture_;
    std::shared_ptr<const ImageMetalGsSnapshot> gs_;
};
class ImageMetal {
public:
    explicit ImageMetal(id<MTLDevice>);
    [[nodiscard]] ImageMetalValidation encode(id<MTLCommandBuffer>,id<MTLTexture> color,id<MTLTexture> depth,
        const ImageMetalQuad&,const ImageMetalBaseline&,ImageMetalTexture);
    [[nodiscard]] ImageMetalValidation encode(id<MTLCommandBuffer>,id<MTLTexture> color,id<MTLTexture> depth,
        const ImageMetalQuad&,const ImageMetalBaseline&,ImageMetalGsTexture);
private:
    id<MTLDevice> device_;
    id<MTLRenderPipelineState> pipeline_,gs_pipeline_;
    std::shared_ptr<const ImageMetalGsSnapshot> cached_gs_;
    id<MTLDepthStencilState> no_depth_;
};
}
