#include "ui_metal.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

@interface UiPipelineBundle : NSObject
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic,strong) id<MTLDepthStencilState> alwaysWrite;
@property(nonatomic,strong) id<MTLDepthStencilState> alwaysKeep;
@property(nonatomic,strong) id<MTLDepthStencilState> gequalWrite;
@property(nonatomic,strong) id<MTLDepthStencilState> gequalKeep;
@end
@implementation UiPipelineBundle
@end

namespace okage {
namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
constexpr const char* source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex {float4 position;float4 color;float4 stq;};
struct Fragment {float4 position [[position]];float4 color [[center_no_perspective]];
    float3 stq [[center_no_perspective]];float z [[center_no_perspective]];};
struct State {uint textured;uint alpha_positive;uint primitive;uint padding;};
struct Errors {atomic_uint count;atomic_uint first_primitive;};
struct Output {float4 color [[color(0),raster_order_group(0)]];float depth [[depth(any)]];};
vertex Fragment ui_vertex(uint id [[vertex_id]],device const Vertex* v [[buffer(0)]]) {
    return {float4(v[id].position.xy,0,1),v[id].color,v[id].stq.xyz,v[id].position.z};
}
bool texel(texture2d<float,access::read> t,int2 p,thread float4& value) {
    p=clamp(p,int2(0),int2(1023)); // Authored CLAMP is logical1024.
    if(any(p>=int2(t.get_width(),t.get_height())))return false;
    value=round(t.read(uint2(p))*255.0f);return true;
}
bool sample_ui(texture2d<float,access::read> t,float3 stq,thread float4& value) {
    if(!all(isfinite(stq))||stq.z<=0)return false;
    float2 coordinate=stq.xy/stq.z*1024.0f;
    if(!all(isfinite(coordinate)))return false;
    float2 at=clamp(coordinate,float2(.5f),float2(1023.5f))-.5f;
    int2 lo=int2(floor(at));float2 f=fract(at);
    float4 a,b,c,d;
    if(!texel(t,lo,a))return false;
    if(f.x>0){if(!texel(t,lo+int2(1,0),b))return false;}else b=a;
    if(f.y>0){if(!texel(t,lo+int2(0,1),c))return false;}else c=a;
    if(f.x>0&&f.y>0){if(!texel(t,lo+int2(1,1),d))return false;}
    else d=f.x>0?b:c;
    float4 top=a+(b-a)*f.x,bottom=c+(d-c)*f.x;
    value=top+(bottom-top)*f.y;return true;
}
fragment Output ui_fragment(Fragment v [[stage_in]],constant State& state [[buffer(0)]],
    texture2d<float,access::read> t [[texture(0)]],device Errors* errors [[buffer(1)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    float4 modulated=v.color;
    if(state.textured) {
        float4 sampled=0;
        if(!sample_ui(t,v.stq,sampled)){
            atomic_fetch_add_explicit(&errors->count,1u,memory_order_relaxed);
            atomic_fetch_min_explicit(&errors->first_primitive,state.primitive,memory_order_relaxed);
            discard_fragment();
        }
        modulated=sampled*v.color/128.0f;
    }
    int4 s=int4(clamp(modulated,0.0f,255.0f));
    if(state.alpha_positive&&s.a==0)discard_fragment();
    int4 d=int4(round(destination*255.0f));
    int3 rgb=clamp(((s.rgb*s.a)>>7)+d.rgb,0,255);
    return {float4(int4(rgb,s.a))/255.0f,1.0f-floor(v.z)/16777216.0f};
}
)metal";
struct Vertex {Float4 position,color,stq;};
struct ShaderState {std::uint32_t textured,alpha_positive,primitive,padding;};
struct Submission {
    unsigned first,count;
    ShaderState state;
    MTLScissorRect scissor;
    id<MTLDepthStencilState> depth;
    UiMetalTexture texture;
};
bool same_binding(const UiGeometryTexture& a,const UiGeometryTexture& b){
    return a.kind==b.kind&&a.font_index==b.font_index&&a.tex0==b.tex0&&a.fonts==b.fonts&&a.window==b.window;
}

NSMutableDictionary<NSValue*,UiPipelineBundle*>* pipeline_cache() {
    static NSMutableDictionary<NSValue*,UiPipelineBundle*>* cache = [NSMutableDictionary dictionary];
    return cache;
}

UiPipelineBundle* acquire_pipeline_bundle(id<MTLDevice> device) {
    check(device&&[device supportsFamily:MTLGPUFamilyApple1],"UI programmable blending requires Apple GPU");
    auto* cache=pipeline_cache();
    auto* key=[NSValue valueWithPointer:(__bridge const void*)device];
    @synchronized(cache) {
        auto* cached=cache[key];
        if(cached)return cached;
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&error];
        if(!library)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=[library newFunctionWithName:@"ui_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"ui_fragment"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        auto* pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!pipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=MTLCompareFunctionAlways;depth.depthWriteEnabled=YES;
        auto* always_write=[device newDepthStencilStateWithDescriptor:depth];depth.depthWriteEnabled=NO;
        auto* always_keep=[device newDepthStencilStateWithDescriptor:depth];
        depth.depthCompareFunction=MTLCompareFunctionLessEqual;
        auto* gequal_keep=[device newDepthStencilStateWithDescriptor:depth];depth.depthWriteEnabled=YES;
        auto* gequal_write=[device newDepthStencilStateWithDescriptor:depth];
        if(!always_write||!always_keep||!gequal_keep||!gequal_write)
            throw std::runtime_error("UI depth state allocation failed");
        auto* bundle=[UiPipelineBundle new];bundle.device=device;bundle.pipeline=pipeline;
        bundle.alwaysWrite=always_write;bundle.alwaysKeep=always_keep;
        bundle.gequalWrite=gequal_write;bundle.gequalKeep=gequal_keep;
        cache[key]=bundle;
        return bundle;
    }
}
}
UiMetalValidation::UiMetalValidation(id<MTLCommandBuffer> command,id<MTLBuffer> errors,std::vector<std::string> labels)
    :command_(command),errors_(errors),primitive_labels_(std::move(labels)){}
void UiMetalValidation::validate_completed(id<MTLCommandBuffer> command) const {
    check(command&&command==command_,"UI validation token command identity mismatch");
    check(command.status==MTLCommandBufferStatusCompleted,"UI validation requires successful command completion");
    const auto* errors=static_cast<const std::uint32_t*>(errors_.contents);
    if(errors[0]||errors[1]!=std::numeric_limits<std::uint32_t>::max()){
        const auto first=errors[1];const auto label=first<primitive_labels_.size()?primitive_labels_[first]:"unavailable primitive";
        throw std::runtime_error("UI unsupported sampled texture footprint: fragment count="+std::to_string(errors[0])+", first "+label);
    }
}
id<MTLTexture> upload_ui_gs_texture(id<MTLDevice> device,const Texture& image){
    check(device&&image.width&&image.height&&image.width<=1024&&image.height<=1024&&
        image.rgba.size()==std::size_t(image.width)*image.height*4,"UI texture upload extent");
    auto bytes=image.rgba;
    for(std::size_t i=3;i<bytes.size();i+=4){check(bytes[i]==255||!(bytes[i]&1),"UI texture alpha is not reversibly expanded GS alpha");bytes[i]=bytes[i]==255?128:bytes[i]/2;}
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image.width height:image.height mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    auto texture=[device newTextureWithDescriptor:d];check(texture,"UI texture allocation");
    [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:bytes.data() bytesPerRow:image.width*4];return texture;
}
void UiMetal::prepare_backend(id<MTLDevice> device) {
    (void)acquire_pipeline_bundle(device);
}
UiMetal::UiMetal(id<MTLDevice> device):device_(device){
    auto* bundle=acquire_pipeline_bundle(device);
    pipeline_=bundle.pipeline;always_write_=bundle.alwaysWrite;always_keep_=bundle.alwaysKeep;
    gequal_write_=bundle.gequalWrite;gequal_keep_=bundle.gequalKeep;
    Texture fallback{1,1,0,0,{255,255,255,255}};fallback_=upload_ui_gs_texture(device,fallback);
}
UiMetalValidation UiMetal::encode(id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth,
        const UiGeometryList& geometry,const UiMetalBaseline& baseline,const UiMetalTextureResolver& resolver){
    check(command&&command.device==device_&&command.retainedReferences&&color&&depth&&color!=depth&&color.device==device_&&depth.device==device_&&
        color.sampleCount==1&&depth.sampleCount==1&&color.textureType==MTLTextureType2D&&depth.textureType==MTLTextureType2D&&color.pixelFormat==MTLPixelFormatRGBA8Unorm&&depth.pixelFormat==MTLPixelFormatDepth32Float&&
        color.width&&color.height&&color.width<=2048&&color.height<=2048&&color.width==depth.width&&color.height==depth.height,
        "UI render target format/extent");
    check(baseline.offset_x16<=65535&&baseline.offset_y16<=65535&&baseline.framebuffer_mask==0&&baseline.colclamp==1&&
        baseline.dither==0&&baseline.prmodecont==1,"UI unsupported inherited framebuffer state");
    // All state necessary for a primitive must be explicitly emitted or supplied.
    std::optional<std::uint64_t> test,alpha,scissor,tex1,clamp,texa,pabe,fba;
    std::optional<UiGeometryTexture> binding;UiMetalTexture texture{};bool framebuffer=false;
    std::vector<Vertex> vertices;std::vector<Submission> submissions;std::vector<std::string> labels;
    for(const auto& commandValue:geometry.commands){
        if(const auto* r=std::get_if<UiDrawRegister>(&commandValue)){
            switch(r->index){
            case 0x47:check(r->value==0x30003||r->value==0x5000d,"UI unsupported TEST");test=r->value;break;
            case 0x42:check(r->value==0x48,"UI unsupported ALPHA");alpha=r->value;break;
            case 0x40:check(!(r->value&0xf800f800f800f800ull),"UI invalid SCISSOR");scissor=r->value;break;
            case 0x14:check(r->value==0x60,"UI unsupported TEX1");tex1=r->value;break;
            case 8:check(r->value==5,"UI unsupported CLAMP");clamp=r->value;break;
            case 0x3b:check(r->value==0x100008080ull,"UI unsupported TEXA");texa=r->value;break;
            case 0x49:check(r->value==0,"UI unsupported PABE");pabe=r->value;break;
            case 0x4a:check(r->value==0,"UI unsupported FBA");fba=r->value;break;
            case 0x3f:check(r->value==0,"UI unsupported TEXFLUSH value");break;
            default:throw std::runtime_error("UI unsupported GS register");
            }
        }else if(std::holds_alternative<UiDrawCurrentFramebuffer>(commandValue))framebuffer=true;
        else if(const auto* t=std::get_if<UiGeometryTexture>(&commandValue)){
            check(t->tex0==0x20000006a8000000ull&&bool(resolver),"UI unsupported texture binding");texture=resolver(*t);binding=*t;
            check(texture.texture&&texture.texture.device==device_&&texture.texture.sampleCount==1&&texture.texture.textureType==MTLTextureType2D&&
                texture.texture!=color&&texture.texture.pixelFormat==MTLPixelFormatRGBA8Unorm&&
                texture.texture.width&&texture.texture.height&&texture.texture.width<=1024&&texture.texture.height<=1024,
                "UI texture must contain raw GS bytes in linear RGBA8 backing");
        }else if(const auto* p=std::get_if<UiResolvedPrimitive>(&commandValue)){
            check(p->quad.emitted&&(p->prim==0x4d||p->prim==0x5d),"UI unsupported primitive/topology");
            check(framebuffer&&test&&alpha&&scissor&&pabe&&fba,"UI missing source GS state");
            check(labels.size()<std::numeric_limits<std::uint32_t>::max(),"UI primitive ordinal overflow");
            const auto ordinal=std::uint32_t(labels.size());const auto& src=p->source;
            labels.push_back("primitive "+std::to_string(ordinal)+" window "+std::to_string(src.window)+
                " family "+std::to_string(unsigned(src.family))+" group "+std::to_string(src.group)+
                " element "+std::to_string(src.element)+" side "+std::to_string(src.side)+
                " history "+std::to_string(src.history)+" pass "+std::to_string(src.pass));
            const bool textured=p->prim&0x10;
            if(textured)check(tex1&&clamp&&texa&&binding&&p->texture&&same_binding(*binding,*p->texture),"UI missing/current texture state mismatch");
            else check(!p->texture,"UI untextured primitive has unexpected texture");
            const unsigned x0=*scissor&2047,x1=(*scissor>>16)&2047,y0=(*scissor>>32)&2047,y1=(*scissor>>48)&2047;
            check(x0<=x1&&y0<=y1,"UI inverted SCISSOR");
            MTLScissorRect rect{std::min<NSUInteger>(x0,color.width),std::min<NSUInteger>(y0,color.height),0,0};
            rect.width=std::min<NSUInteger>(x1+1,color.width)-rect.x;rect.height=std::min<NSUInteger>(y1+1,color.height)-rect.y;
            std::array<Vertex,4> quad;
            for(unsigned i=0;i<4;++i){const auto& v=p->quad.vertices[i];
                const float x=float(v.xyzf[0]&65535)/16.f-float(baseline.offset_x16)/16.f+.5f;
                const float y=float(v.xyzf[1]&65535)/16.f-float(baseline.offset_y16)/16.f+.5f;
                quad[i].position={2*x/float(color.width)-1,1-2*y/float(color.height),float((v.xyzf[2]&0x0ffffff0u)>>4),1};
                for(unsigned j=0;j<4;++j)quad[i].color[j]=float(v.rgba[j]&255);
                quad[i].stq={v.stq[0],v.stq[1],v.stq[2],0};
                if(textured){check(std::isfinite(v.stq[0])&&std::isfinite(v.stq[1])&&std::isfinite(v.stq[2])&&v.stq[2]>0,"UI invalid STQ");
                }
            }
            const unsigned first=unsigned(vertices.size());for(unsigned i:{0u,1u,2u,0u,2u,3u})vertices.push_back(quad[i]);
            const bool gequal=*test==0x5000d;
            auto z=gequal?(baseline.depth_write?gequal_write_:gequal_keep_):(baseline.depth_write?always_write_:always_keep_);
            if(rect.width&&rect.height)submissions.push_back({first,6,{unsigned(textured),unsigned(gequal),ordinal,0},rect,z,textured?texture:UiMetalTexture{}});
        }else if(const auto* control=std::get_if<UiDrawControl>(&commandValue)){
            check(*control>=UiDrawControl::Begin&&*control<=UiDrawControl::QueueTextState,"UI invalid control");
        }
    }
    const std::array<std::uint32_t,4> initial_errors{0,std::numeric_limits<std::uint32_t>::max(),0,0};
    auto errors=[device_ newBufferWithBytes:initial_errors.data() length:sizeof(initial_errors) options:MTLResourceStorageModeShared];
    check(errors,"UI validation buffer allocation");
    UiMetalValidation validation(command,errors,std::move(labels));
    if(submissions.empty())return validation;
    auto buffer=[device_ newBufferWithBytes:vertices.data() length:vertices.size()*sizeof(Vertex) options:MTLResourceStorageModeShared];check(buffer,"UI vertex allocation");
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
    pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;
    auto encoder=[command renderCommandEncoderWithDescriptor:pass];check(encoder,"UI render encoder");
    [encoder setRenderPipelineState:pipeline_];[encoder setCullMode:MTLCullModeNone];[encoder setVertexBuffer:buffer offset:0 atIndex:0];
    [encoder setFragmentBuffer:errors offset:0 atIndex:1];
    for(const auto& s:submissions){[encoder setDepthStencilState:s.depth];[encoder setScissorRect:s.scissor];
        [encoder setFragmentTexture:s.texture.texture?s.texture.texture:fallback_ atIndex:0];[encoder setFragmentBytes:&s.state length:sizeof(s.state) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first vertexCount:s.count];}
    [encoder endEncoding];
    return validation;
}
}
