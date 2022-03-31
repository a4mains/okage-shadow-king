#pragma once
#import <Metal/Metal.h>
#include "okage/battle_capture.hpp"
#include "okage/assets.hpp"

namespace okage {
class BattleCaptureMetal {
public:
    explicit BattleCaptureMetal(id<MTLDevice>);
    void encode(id<MTLCommandBuffer>,id<MTLTexture> destination,
        id<MTLTexture> capture,id<MTLTexture> history,const BattleCaptureDraw&);
private:
    id<MTLRenderPipelineState> pipeline_;
};
class BattleCaptureSurface {
public:
    BattleCaptureSurface();
    void apply(Texture& destination,const Texture& capture,const Texture& history,
        const BattleCaptureDraw&);
private:
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    BattleCaptureMetal renderer_;
    id<MTLTexture> target_,capture_,history_;
};
}
