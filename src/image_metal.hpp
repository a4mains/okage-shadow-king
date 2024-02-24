#pragma once
#import <Metal/Metal.h>
#include "okage/assets.hpp"
#include "okage/image_quad.hpp"
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
id<MTLTexture> upload_image_gs_texture(id<MTLDevice>,const Texture&,ImageMetalPreviewAlpha);
class ImageMetalValidation {
public:
    void validate_completed(id<MTLCommandBuffer>) const;
private:
    friend class ImageMetal;
    ImageMetalValidation(id<MTLCommandBuffer>,id<MTLBuffer>,ImageMetalTexture);
    id<MTLCommandBuffer> command_;
    id<MTLBuffer> errors_;
    ImageMetalTexture texture_;
};
class ImageMetal {
public:
    explicit ImageMetal(id<MTLDevice>);
    [[nodiscard]] ImageMetalValidation encode(id<MTLCommandBuffer>,id<MTLTexture> color,id<MTLTexture> depth,
        const ImageMetalQuad&,const ImageMetalBaseline&,ImageMetalTexture);
private:
    id<MTLDevice> device_;
    id<MTLRenderPipelineState> pipeline_;
    id<MTLDepthStencilState> no_depth_;
};
}
