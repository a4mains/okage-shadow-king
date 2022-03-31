#include "battle_capture_metal.hpp"
#include <stdexcept>

namespace okage {
namespace {
constexpr const char* source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex {float4 position;float4 uv;};
struct Fragment {float4 position [[position]];float2 uv [[center_no_perspective]];};
struct State {uint4 rgba;uint history;};
vertex Fragment capture_vertex(uint id [[vertex_id]],device const Vertex* v [[buffer(0)]]) {
    return {v[id].position,v[id].uv.xy};
}
float4 texel(texture2d<float,access::read> t,int2 p) {
    p=clamp(p,int2(0),int2(t.get_width()-1,t.get_height()-1));
    return round(t.read(uint2(p))*255.0f);
}
fragment float4 capture_fragment(Fragment v [[stage_in]],constant State& s [[buffer(0)]],
    texture2d<float,access::read> input [[texture(0)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    const float2 p=v.uv-0.5f;
    const int2 lo=int2(floor(p));
    const float2 fraction=fract(p);
    const float4 a=texel(input,lo),b=texel(input,lo+int2(1,0));
    const float4 c=texel(input,lo+int2(0,1)),d=texel(input,lo+int2(1,1));
    const float4 top=a+(b-a)*fraction.x,bottom=c+(d-c)*fraction.x;
    const float4 sampled=top+(bottom-top)*fraction.y;
    int4 source=int4(clamp(sampled*float4(s.rgba)/128.0f,0.0f,255.0f));
    if(!s.history)source.a=int(s.rgba.a); // First TEX0 is RGB only (TCC0).
    int4 dest=int4(round(destination*255.0f));
    const int alpha=s.history?124:source.a;
    const int3 rgb=clamp(((source.rgb-dest.rgb)*alpha >> 7)+dest.rgb,0,255);
    return float4(int4(rgb,source.a))/255.0f;
}
)metal";
struct Vertex {Float4 position,uv;};
struct State {std::array<std::uint32_t,4> rgba;std::uint32_t history;};
}
BattleCaptureMetal::BattleCaptureMetal(id<MTLDevice> device) {
    if(!device||![device supportsFamily:MTLGPUFamilyApple1])
        throw std::runtime_error("Native capture blending requires an Apple GPU");
    NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
    auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&error];
    if(!library)throw std::runtime_error(error.localizedDescription.UTF8String);
    auto descriptor=[MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction=[library newFunctionWithName:@"capture_vertex"];
    descriptor.fragmentFunction=[library newFunctionWithName:@"capture_fragment"];
    descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    pipeline_=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if(!pipeline_)throw std::runtime_error(error.localizedDescription.UTF8String);
}
void BattleCaptureMetal::encode(id<MTLCommandBuffer> command,id<MTLTexture> target,
        id<MTLTexture> capture,id<MTLTexture> history,const BattleCaptureDraw& draw) {
    if(!command||!target||!capture||!history||target==capture||target==history||
       target.width!=640||target.height!=224||target.pixelFormat!=MTLPixelFormatRGBA8Unorm||
       (draw.count!=2&&draw.count!=4))throw std::runtime_error("Invalid native battle capture target/geometry");
    for(auto texture:{capture,history})
        if(texture.width!=640||texture.height<224||texture.height>1024||texture.pixelFormat!=MTLPixelFormatRGBA8Unorm)
            throw std::runtime_error("Invalid native battle capture texture extent");
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    auto encoder=[command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline_];[encoder setCullMode:MTLCullModeNone];
    auto submit=[&](const std::array<BattleCaptureVertex,4>& packed,unsigned count,id<MTLTexture> texture,std::uint32_t rgba,bool feedback) {
        std::array<Vertex,4> vertices;
        for(unsigned i=0;i<count;++i) {
            const auto& p=packed[i];
            const float x=float(p.xyz&0xffff)/16.f-1728+.5f,y=float((p.xyz>>16)&0xffff)/16.f-1936+.5f;
            vertices[i]={{2*x/640.f-1,1-2*y/224.f,0,1},
                {float(p.uv&0x3fff)/16.f,float((p.uv>>16)&0x3fff)/16.f,0,0}};
        }
        if(count==2) {
            const auto first=vertices[0],last=vertices[1];
            vertices[1]=last;vertices[1].position[1]=first.position[1];vertices[1].uv[1]=first.uv[1];
            vertices[2]=last;vertices[3]=first;vertices[3].position[1]=last.position[1];vertices[3].uv[1]=last.uv[1];
        }
        const std::array<Vertex,6> triangles={vertices[0],vertices[1],vertices[2],vertices[0],vertices[2],vertices[3]};
        const State state{{rgba&255,(rgba>>8)&255,(rgba>>16)&255,rgba>>24},unsigned(feedback)};
        [encoder setVertexBytes:triangles.data() length:sizeof(triangles) atIndex:0];
        [encoder setFragmentBytes:&state length:sizeof(state) atIndex:0];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    };
    submit(draw.vertices,draw.count,capture,draw.rgba,false);
    const std::array<BattleCaptureVertex,4> previous={BattleCaptureVertex{0,0xffff78f86bf0},
        BattleCaptureVertex{0x0e002800,0xffff87009400}};
    submit(previous,2,history,0x80808080,true);
    [encoder endEncoding];
}
BattleCaptureSurface::BattleCaptureSurface():device_(MTLCreateSystemDefaultDevice()),
    queue_([device_ newCommandQueue]),renderer_(device_) {
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:640 height:224 mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
    capture_=[device_ newTextureWithDescriptor:descriptor];history_=[device_ newTextureWithDescriptor:descriptor];
    descriptor.usage=MTLTextureUsageRenderTarget;target_=[device_ newTextureWithDescriptor:descriptor];
    if(!queue_||!target_||!capture_||!history_)throw std::runtime_error("Native capture surface allocation failed");
}
void BattleCaptureSurface::apply(Texture& destination,const Texture& capture,const Texture& history,
        const BattleCaptureDraw& draw) {
    for(const auto* image:std::array<const Texture*,3>{&destination,&capture,&history})
        if(image->width!=640||image->height!=224||image->rgba.size()!=640*224*4)
            throw std::runtime_error("Native capture requires complete 640x224 game surfaces");
    const auto region=MTLRegionMake2D(0,0,640,224);
    [target_ replaceRegion:region mipmapLevel:0 withBytes:destination.rgba.data() bytesPerRow:640*4];
    [capture_ replaceRegion:region mipmapLevel:0 withBytes:capture.rgba.data() bytesPerRow:640*4];
    [history_ replaceRegion:region mipmapLevel:0 withBytes:history.rgba.data() bytesPerRow:640*4];
    auto command=[queue_ commandBuffer];renderer_.encode(command,target_,capture_,history_,draw);
    [command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)
        throw std::runtime_error(command.error.localizedDescription.UTF8String);
    [target_ getBytes:destination.rgba.data() bytesPerRow:640*4 fromRegion:region mipmapLevel:0];
}
}
