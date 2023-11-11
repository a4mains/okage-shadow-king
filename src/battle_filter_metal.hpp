#pragma once
#import <Metal/Metal.h>
#include <cstdint>

namespace okage {
struct BattleFilter6State {
    std::uint32_t selector=1,color=0x80000000;
    std::int32_t setting=0;
    bool field_nonzero=true;
};
class BattleFilterMetal {
public:
    static void prepare_backend(id<MTLDevice>);
    explicit BattleFilterMetal(id<MTLDevice>);
    void encode(id<MTLCommandBuffer>,id<MTLTexture> rgba,id<MTLTexture> depth,
        const BattleFilter6State&);
private:
    id<MTLRenderPipelineState> alpha_,blur_;
    id<MTLTexture> snapshot_;
};
}
