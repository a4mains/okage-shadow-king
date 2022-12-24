#pragma once
#import <Metal/Metal.h>
#include "okage/assets.hpp"
#include "okage/ui_geometry.hpp"
#include <functional>

namespace okage {
struct UiMetalBaseline {
    std::uint32_t offset_x16,offset_y16;
    bool depth_write;
    std::uint32_t framebuffer_mask,colclamp,dither,prmodecont;
};
struct UiMetalTexture {
    id<MTLTexture> texture;
    std::shared_ptr<const void> owner;
};
using UiMetalTextureResolver=std::function<UiMetalTexture(const UiGeometryTexture&)>;
id<MTLTexture> upload_ui_gs_texture(id<MTLDevice>,const Texture&);
class UiMetalValidation {
public:
    void validate_completed(id<MTLCommandBuffer>) const;
private:
    friend class UiMetal;
    UiMetalValidation(id<MTLCommandBuffer>,id<MTLBuffer>,std::vector<std::string>);
    id<MTLCommandBuffer> command_;
    id<MTLBuffer> errors_;
    std::vector<std::string> primitive_labels_;
};
class UiMetal {
public:
    explicit UiMetal(id<MTLDevice>);
    [[nodiscard]] UiMetalValidation encode(id<MTLCommandBuffer>,id<MTLTexture> color,id<MTLTexture> depth,
        const UiGeometryList&,const UiMetalBaseline&,const UiMetalTextureResolver&);
private:
    id<MTLDevice> device_;
    id<MTLTexture> fallback_;
    id<MTLRenderPipelineState> pipeline_;
    id<MTLDepthStencilState> always_write_,always_keep_,gequal_write_,gequal_keep_;
};
}
