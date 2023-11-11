#include "battle_filter_metal.hpp"
#include <array>
#include <stdexcept>

@interface BattleFilterPipelineBundle : NSObject
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLRenderPipelineState> alpha;
@property(nonatomic,strong) id<MTLRenderPipelineState> blur;
@end
@implementation BattleFilterPipelineBundle
@end

namespace okage {
namespace {
constexpr const char* source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Fragment {float4 position [[position]];};
vertex Fragment filter_vertex(uint id [[vertex_id]]) {
    const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return {float4(p[id],0,1)};
}
fragment float4 filter_alpha(Fragment f [[stage_in]],constant int& setting [[buffer(0)]],
    depth2d<float,access::read> depth [[texture(0)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    const uint2 p=uint2(f.position.xy);
    // Scene projection quantizes integer Z before applying 1-Z/2^24.
    const uint z=uint((1.0f-depth.read(p))*16777216.0f);
    const int value=as_type<int>(as_type<uint>(setting)+255u-((z>>8)&255));
    const int a=z>65535u?0:clamp(value>>2,0,128);
    return float4(destination.rgb,float(a)/255.0f);
}
fragment float4 filter_blur(Fragment f [[stage_in]],constant float2& offset [[buffer(0)]],
    texture2d<float,access::read> input [[texture(0)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    const float2 p=floor(f.position.xy),uv=p-offset;
    if(any(uv<float2(0))||any(uv>=float2(640,224)))discard_fragment();
    const int3 s=int3(round(input.read(uint2(floor(uv))).rgb*255.0f));
    const int4 d=int4(round(destination*255.0f));
    const int3 rgb=clamp(((s-d.rgb)*d.a>>7)+d.rgb,0,255);
    return float4(int4(rgb,d.a))/255.0f;
}
)metal";

NSMutableDictionary<NSValue*,BattleFilterPipelineBundle*>* pipeline_cache() {
    static NSMutableDictionary<NSValue*,BattleFilterPipelineBundle*>* cache = [NSMutableDictionary dictionary];
    return cache;
}

BattleFilterPipelineBundle* acquire_pipeline_bundle(id<MTLDevice> device) {
    if(!device||![device supportsFamily:MTLGPUFamilyApple1])
        throw std::runtime_error("Battle filter requires an Apple GPU");
    auto* cache=pipeline_cache();
    auto* key=[NSValue valueWithPointer:(__bridge const void*)device];
    @synchronized(cache) {
        auto* cached=cache[key];
        if(cached)return cached;
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&error];
        if(!library)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto pipeline=[&](NSString* name) {
            auto descriptor=[MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction=[library newFunctionWithName:@"filter_vertex"];
            descriptor.fragmentFunction=[library newFunctionWithName:name];
            descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
            auto result=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            if(!result)throw std::runtime_error(error.localizedDescription.UTF8String);
            return result;
        };
        auto* bundle=[BattleFilterPipelineBundle new];bundle.device=device;
        bundle.alpha=pipeline(@"filter_alpha");bundle.blur=pipeline(@"filter_blur");
        cache[key]=bundle;
        return bundle;
    }
}
}
void BattleFilterMetal::prepare_backend(id<MTLDevice> device) {
    (void)acquire_pipeline_bundle(device);
}
BattleFilterMetal::BattleFilterMetal(id<MTLDevice> device) {
    auto* bundle=acquire_pipeline_bundle(device);
    alpha_=bundle.alpha;blur_=bundle.blur;
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:640 height:224 mipmapped:NO];
    descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead;
    snapshot_=[device newTextureWithDescriptor:descriptor];
    if(!snapshot_)throw std::runtime_error("Battle filter texture allocation failed");
}
void BattleFilterMetal::encode(id<MTLCommandBuffer> command,id<MTLTexture> rgba,
        id<MTLTexture> depth,const BattleFilter6State& state) {
    if(state.selector!=1||state.color!=0x80000000)
        throw std::runtime_error("Unrecovered battle filter6 selector/color");
    if(!command||!rgba||!depth||rgba.width!=640||rgba.height!=224||
       depth.width!=640||depth.height!=224||rgba.pixelFormat!=MTLPixelFormatRGBA8Unorm||
       depth.pixelFormat!=MTLPixelFormatDepth32Float||!(rgba.usage&MTLTextureUsageRenderTarget)||
       !(depth.usage&MTLTextureUsageShaderRead)||rgba.sampleCount!=1||depth.sampleCount!=1||
       rgba.device!=snapshot_.device||depth.device!=snapshot_.device||
       command.commandQueue.device!=snapshot_.device)
        throw std::runtime_error("Invalid battle filter scene surfaces");
    auto begin=[&](id<MTLRenderPipelineState> pipeline) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=rgba;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];
        return encoder;
    };
    auto encoder=begin(alpha_);
    [encoder setFragmentBytes:&state.setting length:sizeof(state.setting) atIndex:0];
    [encoder setFragmentTexture:depth atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    const float half=state.field_nonzero?0:.5f;
    const std::array<std::array<float,2>,4> offsets={{{-3,-half},{2,-half},{0,-1-half},{0,1-half}}};
    for(const auto& offset:offsets) {
        auto blit=[command blitCommandEncoder];
        [blit copyFromTexture:rgba sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(640,224,1) toTexture:snapshot_ destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
        encoder=begin(blur_);
        [encoder setFragmentBytes:offset.data() length:sizeof(offset) atIndex:0];
        [encoder setFragmentTexture:snapshot_ atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }
}
}
