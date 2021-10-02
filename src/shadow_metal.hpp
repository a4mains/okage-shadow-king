#pragma once
#import <Metal/Metal.h>
#include "okage/shadow.hpp"

namespace okage {
struct ShadowRasterMapping {
    float offset_x=1728,offset_y=1936;
    float depth_scale=-1.f/16777216.f,depth_offset=1;
};
struct ShadowRasterDraw {
    ShadowPrism prism;
    std::array<std::uint32_t,3> color{};
};
class ShadowMetal {
public:
    explicit ShadowMetal(id<MTLDevice>);
    void encode(id<MTLCommandBuffer>,id<MTLTexture> color,id<MTLTexture> depth,
        std::span<const ShadowRasterDraw>,const ShadowRasterMapping&,
        bool restore_opaque_alpha=true);
private:
    id<MTLDevice> device_;
    id<MTLRenderPipelineState> pipeline_,alpha_pipeline_;
    id<MTLDepthStencilState> depth_,always_;
};
}
