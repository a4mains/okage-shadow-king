#include "shadow_metal.hpp"
#include <cmath>
#include <stdexcept>
#include <vector>

@interface ShadowPipelineBundle : NSObject
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic,strong) id<MTLRenderPipelineState> alphaPipeline;
@property(nonatomic,strong) id<MTLDepthStencilState> depth;
@property(nonatomic,strong) id<MTLDepthStencilState> always;
@end
@implementation ShadowPipelineBundle
@end

namespace okage {
namespace {
// GS PRIM4/0x44 has neither perspective color interpolation nor fog enabled.
// Keep depth in GS units until the fragment's integer depth comparison.
constexpr const char* source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex {float4 position;};
struct Varying {float4 position [[position]];float z [[center_no_perspective]];};
struct State {uint4 color_stage;float4 depth_mapping;};
struct Output {float4 color [[color(0),raster_order_group(0)]];float depth [[depth(any)]];};
vertex Varying shadow_vertex(uint id [[vertex_id]],device const Vertex* v [[buffer(0)]]) {
    return {float4(v[id].position.xy,0,1),v[id].position.z};
}
fragment Output shadow_fragment(Varying v [[stage_in]],constant State& s [[buffer(0)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    int4 d=int4(round(destination*255.0f));uint stage=s.color_stage.w;
    if(stage==0)d.a|=128;
    else if(stage==2)d.a&=127;
    else {
        if(d.a&128)discard_fragment();
        d.rgb=clamp(((int3(s.color_stage.xyz)-d.rgb)*d.a >> 7)+d.rgb,0,255);
        d.a=0;
    }
    return {float4(d)/255.0f,floor(v.z)*s.depth_mapping.x+s.depth_mapping.y};
}
vertex float4 alpha_vertex(uint id [[vertex_id]]) {
    return float4(id==1?3.0f:-1.0f,id==2?3.0f:-1.0f,0,1);
}
fragment float4 alpha_fragment(constant uint& alpha [[buffer(0)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    return float4(destination.rgb,float(alpha)/255.0f);
}
)metal";
struct State {std::array<std::uint32_t,4> color_stage;Float4 depth_mapping;};

NSMutableDictionary<NSValue*,ShadowPipelineBundle*>* pipeline_cache() {
    static NSMutableDictionary<NSValue*,ShadowPipelineBundle*>* cache = [NSMutableDictionary dictionary];
    return cache;
}

ShadowPipelineBundle* acquire_pipeline_bundle(id<MTLDevice> device) {
    if(!device||![device supportsFamily:MTLGPUFamilyApple1])
        throw std::runtime_error("Native shadow framebuffer blending requires an Apple GPU");
    auto* cache=pipeline_cache();
    auto* key=[NSValue valueWithPointer:(__bridge const void*)device];
    @synchronized(cache) {
        auto* cached=cache[key];
        if(cached)return cached;
        NSError* error=nil;
        auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
        if(!library)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"shadow_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"shadow_fragment"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        auto* pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!pipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.vertexFunction=[library newFunctionWithName:@"alpha_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"alpha_fragment"];
        auto* alpha_pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!alpha_pipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto depth=[MTLDepthStencilDescriptor new];
        depth.depthCompareFunction=MTLCompareFunctionLessEqual;depth.depthWriteEnabled=NO;
        auto* depth_state=[device newDepthStencilStateWithDescriptor:depth];
        depth.depthCompareFunction=MTLCompareFunctionAlways;
        auto* always_state=[device newDepthStencilStateWithDescriptor:depth];
        if(!depth_state||!always_state)throw std::runtime_error("Shadow depth state allocation failed");
        auto* bundle=[ShadowPipelineBundle new];bundle.device=device;bundle.pipeline=pipeline;
        bundle.alphaPipeline=alpha_pipeline;bundle.depth=depth_state;bundle.always=always_state;
        cache[key]=bundle;
        return bundle;
    }
}
}
void ShadowMetal::prepare_backend(id<MTLDevice> device) {
    (void)acquire_pipeline_bundle(device);
}
ShadowMetal::ShadowMetal(id<MTLDevice> device):device_(device) {
    auto* bundle=acquire_pipeline_bundle(device);
    pipeline_=bundle.pipeline;alpha_pipeline_=bundle.alphaPipeline;
    depth_=bundle.depth;always_=bundle.always;
}
void ShadowMetal::encode(id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth,
        std::span<const ShadowRasterDraw> draws,const ShadowRasterMapping& mapping,bool restore) {
    if(!command||!color||!depth||color.pixelFormat!=MTLPixelFormatRGBA8Unorm||depth.pixelFormat!=MTLPixelFormatDepth32Float||
       color.width!=depth.width||color.height!=depth.height||!color.width||!color.height)
        throw std::runtime_error("Invalid native shadow render targets");
    for(float f:{mapping.offset_x,mapping.offset_y,mapping.depth_scale,mapping.depth_offset})
        if(!std::isfinite(f))throw std::runtime_error("Nonfinite shadow raster mapping");
    if(!(mapping.depth_scale<0))throw std::runtime_error("Shadow depth mapping must preserve reversed GS comparison");
    struct Submission {unsigned first,count;State state;};
    std::vector<Float4> vertices;std::vector<Submission> submissions;
    for(const auto& draw:draws) {
        if(!draw.prism.emitted)continue;
        for(auto c:draw.color)if(c>255)throw std::runtime_error("Shadow raster color overflow");
        for(unsigned pass=0;pass<3;++pass) {
            const auto& strip=draw.prism.passes[pass];
            Submission s{static_cast<unsigned>(vertices.size()),0,{{draw.color[0],draw.color[1],draw.color[2],pass},
                {mapping.depth_scale,mapping.depth_offset,0,0}}};
            for(unsigned end=2;end<strip.size();++end) {
                if(strip[end][3]&0x8000)continue;
                for(unsigned index:{end-2,end-1,end}) {
                    const auto& v=strip[index];
                    // GS raster samples integer pixel centers. Metal samples half centers.
                    const float x=float(v[0]&0xffff)/16.f-mapping.offset_x+.5f;
                    const float y=float(v[1]&0xffff)/16.f-mapping.offset_y+.5f;
                    const float z=float((v[2]&0x0ffffff0)>>4);
                    vertices.push_back({2*x/float(color.width)-1,1-2*y/float(color.height),z,1});
                }
            }
            s.count=static_cast<unsigned>(vertices.size())-s.first;
            if(s.count)submissions.push_back(s);
        }
    }
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;
    pass.depthAttachment.storeAction=MTLStoreActionStore;
    auto encoder=[command renderCommandEncoderWithDescriptor:pass];
    [encoder setCullMode:MTLCullModeNone];
    auto alpha=[&](std::uint32_t value) {
        [encoder setRenderPipelineState:alpha_pipeline_];[encoder setDepthStencilState:always_];
        [encoder setFragmentBytes:&value length:sizeof(value) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    };
    alpha(64);
    if(!vertices.empty()) {
        auto buffer=[device_ newBufferWithBytes:vertices.data() length:vertices.size()*sizeof(Float4) options:MTLResourceStorageModeShared];
        if(!buffer)throw std::runtime_error("Shadow raster vertex allocation failed");
        [encoder setRenderPipelineState:pipeline_];[encoder setDepthStencilState:depth_];
        [encoder setVertexBuffer:buffer offset:0 atIndex:0];
        for(const auto& s:submissions) {
            [encoder setFragmentBytes:&s.state length:sizeof(State) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first vertexCount:s.count];
        }
    }
    if(restore)alpha(255);
    [encoder endEncoding];
}
}
