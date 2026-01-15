#include "image_metal.hpp"
#include "gs_texture_sampling_metal.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace okage {
struct ImageMetalGsSnapshot {
    Ps2TextureSnapshot snapshot;
    id<MTLBuffer> bytes,known;
};
namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
// Source: 1b3450 TEST=0xf ALPHA=0x44, 1b2830 TEX1=0x60 CLAMP=5,
// 1b29b8 PRIM=0x55. Logical1024 filtering and GS lattice follow UiMetal;
// blending deliberately differs from its additive ALPHA=0x48 path.
constexpr const char* source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex {float4 position;float4 st;};
struct Fragment {float4 position [[position]];float2 st [[center_no_perspective]];};
struct Output {float4 color [[color(0),raster_order_group(0)]];};
vertex Fragment image_vertex(uint id [[vertex_id]],device const Vertex* v [[buffer(0)]]) {
    return {v[id].position,v[id].st.xy};
}
bool texel(texture2d<float,access::read> t,int2 p,thread float4& value) {
    p=clamp(p,int2(0),int2(1023));
    if(any(p>=int2(t.get_width(),t.get_height())))return false;
    value=round(t.read(uint2(p))*255.0f);return true;
}
bool sample_image(texture2d<float,access::read> t,float2 st,thread float4& value) {
    float2 coordinate=st*1024.0f;
    if(!all(isfinite(coordinate)))return false;
    float2 at=clamp(coordinate,float2(.5f),float2(1023.5f))-.5f;
    int2 lo=int2(floor(at));float2 f=fract(at);float4 a,b,c,d;
    if(!texel(t,lo,a))return false;
    if(f.x>0){if(!texel(t,lo+int2(1,0),b))return false;}else b=a;
    if(f.y>0){if(!texel(t,lo+int2(0,1),c))return false;}else c=a;
    if(f.x>0&&f.y>0){if(!texel(t,lo+int2(1,1),d))return false;}
    else d=f.x>0?b:c;
    float4 top=a+(b-a)*f.x,bottom=c+(d-c)*f.x;
    value=top+(bottom-top)*f.y;return true;
}
fragment Output image_fragment(Fragment v [[stage_in]],constant uint4& rgba [[buffer(0)]],
    device atomic_uint* errors [[buffer(1)]],texture2d<float,access::read> t [[texture(0)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    float4 sampled=0;
    if(!sample_image(t,v.st,sampled)){
        atomic_fetch_add_explicit(errors,1u,memory_order_relaxed);discard_fragment();
    }
    int4 s=int4(clamp(sampled*float4(rgba)/128.0f,0.0f,255.0f));
    int4 d=int4(round(destination*255.0f));
    int3 rgb=clamp((((s.rgb-d.rgb)*s.a)>>7)+d.rgb,0,255);
    return {float4(int4(rgb,s.a))/255.0f};
}

fragment Output image_gs_fragment(Fragment v [[stage_in]],constant uint4& rgba [[buffer(0)]],
    device atomic_uint* invalid [[buffer(1)]],device const uchar* bytes [[buffer(2)]],
    device const uchar* known [[buffer(3)]],constant GsTextureRegistersMetal& regs [[buffer(4)]],
    float4 destination [[color(0),raster_order_group(0)]]) {
    const float4 sampled=gs_sample_texture(bytes,known,regs,v.st,invalid);
    if(any(!isfinite(sampled)))discard_fragment();
    int4 s=int4(clamp(sampled*float4(rgba)/128.0f,0.0f,255.0f));
    int4 d=int4(round(destination*255.0f));
    int3 rgb=clamp((((s.rgb-d.rgb)*s.a)>>7)+d.rgb,0,255);
    return {float4(int4(rgb,s.a))/255.0f};
}
)metal";
struct Vertex {std::array<float,4> position,st;};
id<MTLTexture> upload_raw(id<MTLDevice> device,const Texture& image){
    check(device&&image.width&&image.height&&image.width<=1024&&image.height<=1024&&
        image.rgba.size()==std::size_t(image.width)*image.height*4,"Image raw texture upload extent");
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image.width height:image.height mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    auto texture=[device newTextureWithDescriptor:d];check(texture,"Image texture allocation");
    [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:image.rgba.data() bytesPerRow:image.width*4];return texture;
}
}
id<MTLTexture> upload_image_gs_source(id<MTLDevice> device,Bytes bytes){return upload_raw(device,decode_xim_gs32(bytes));}
id<MTLTexture> upload_image_gs_texture(id<MTLDevice> device,const Texture& image,ImageMetalPreviewAlpha alpha){
    check(alpha==ImageMetalPreviewAlpha::SourceAtMost128,"Image preview upload requires proven source alpha range");
    check(device&&image.width&&image.height&&image.width<=1024&&image.height<=1024&&
        image.rgba.size()==std::size_t(image.width)*image.height*4,"Image texture upload extent");
    check(image.pixel_format==0||((image.pixel_format==19||image.pixel_format==20)&&image.palette_format==0),
        "Image texture format requires unrecovered TEXA conversion");
    auto bytes=image.rgba;
    for(std::size_t i=3;i<bytes.size();i+=4){
        check(bytes[i]==255||!(bytes[i]&1),"Image texture alpha is not reversibly expanded GS alpha");
        bytes[i]=bytes[i]==255?128:bytes[i]/2;
    }
    auto raw=image;raw.rgba=std::move(bytes);return upload_raw(device,raw);
}
ImageMetalValidation::ImageMetalValidation(id<MTLCommandBuffer> command,id<MTLBuffer> errors,ImageMetalTexture texture,
        std::shared_ptr<const ImageMetalGsSnapshot> gs)
    :command_(command),errors_(errors),texture_(std::move(texture)),gs_(std::move(gs)){}
void ImageMetalValidation::validate_completed(id<MTLCommandBuffer> command) const {
    check(command&&command==command_,"Image validation token command identity mismatch");
    check(command.status==MTLCommandBufferStatusCompleted,"Image validation requires successful command completion");
    const auto count=*static_cast<const std::uint32_t*>(errors_.contents);
    if(gs_) {
        if(count==UINT32_MAX)return;
        if(count==0xfffffffeu||count==0xfffffffdu)throw std::runtime_error("Image unsupported shared GS register/coordinate state code="+std::to_string(count));
        const auto address=(count&0x7fffffu)>>1,nibble=count&1u;
        const auto observation=gs_->snapshot.observe_nibble(address,std::uint8_t(nibble));
        std::ostringstream message;message<<"Image unknown shared GS "<<((count&0x800000u)?"CLUT":"texture")
            <<" nibble address=0x"<<std::hex<<address<<std::dec<<" nibble="<<nibble<<" owner=";
        if(observation.allocation_owner)message<<*observation.allocation_owner;else message<<"none";
        throw std::runtime_error(message.str());
    }
    if(count)throw std::runtime_error("Image unsupported sampled texture footprint: fragment count="+std::to_string(count));
}
ImageMetal::ImageMetal(id<MTLDevice> device):device_(device){
    check(device&&[device supportsFamily:MTLGPUFamilyApple1],"Image programmable blending requires Apple GPU");
    NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
    const auto combined=std::string(gs_texture_sampling_metal_source)+source;
    auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:combined.c_str()] options:options error:&error];
    if(!library)throw std::runtime_error(error.localizedDescription.UTF8String);
    auto d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[library newFunctionWithName:@"image_vertex"];
    d.fragmentFunction=[library newFunctionWithName:@"image_fragment"];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;d.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
    pipeline_=[device newRenderPipelineStateWithDescriptor:d error:&error];if(!pipeline_)throw std::runtime_error(error.localizedDescription.UTF8String);
    d.fragmentFunction=[library newFunctionWithName:@"image_gs_fragment"];
    gs_pipeline_=[device newRenderPipelineStateWithDescriptor:d error:&error];if(!gs_pipeline_)throw std::runtime_error(error.localizedDescription.UTF8String);
    auto z=[MTLDepthStencilDescriptor new];z.depthCompareFunction=MTLCompareFunctionAlways;z.depthWriteEnabled=NO;
    no_depth_=[device newDepthStencilStateWithDescriptor:z];check(no_depth_,"Image depth state allocation");
}
ImageMetalValidation ImageMetal::encode(id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth,
        const ImageMetalQuad& quad,const ImageMetalBaseline& baseline,ImageMetalTexture texture){
    check(command&&command.device==device_&&command.retainedReferences&&color&&depth&&color!=depth&&color.device==device_&&depth.device==device_&&
        color.sampleCount==1&&depth.sampleCount==1&&color.textureType==MTLTextureType2D&&depth.textureType==MTLTextureType2D&&
        color.pixelFormat==MTLPixelFormatRGBA8Unorm&&depth.pixelFormat==MTLPixelFormatDepth32Float&&
        color.width&&color.height&&color.width<=2048&&color.height<=2048&&color.width==depth.width&&color.height==depth.height,
        "Image render target format/extent");
    check(baseline.offset_x16<=65535&&baseline.offset_y16<=65535&&baseline.framebuffer_mask==0&&baseline.colclamp==1&&
        baseline.dither==0&&baseline.prmodecont==1,"Image unsupported inherited framebuffer state");
    check(baseline.scissor_x0<=baseline.scissor_x1&&baseline.scissor_y0<=baseline.scissor_y1&&baseline.scissor_x1<=2047&&baseline.scissor_y1<=2047,
        "Image invalid SCISSOR");
    check(texture.texture&&texture.texture.device==device_&&texture.texture!=color&&texture.texture.sampleCount==1&&
        texture.texture.textureType==MTLTextureType2D&&texture.texture.pixelFormat==MTLPixelFormatRGBA8Unorm&&texture.texture.width&&
        texture.texture.height&&texture.texture.width<=1024&&texture.texture.height<=1024,"Image texture must contain raw GS bytes in linear RGBA8 backing");
    const auto& xy=quad.xy16;
    for(const auto& p:xy)check(p[0]<=65535&&p[1]<=65535,"Image XY outside unsigned GS16 range");
    check(xy[0][0]==xy[3][0]&&xy[1][0]==xy[2][0]&&xy[0][1]==xy[1][1]&&xy[2][1]==xy[3][1]&&
        xy[0][0]<xy[1][0]&&xy[0][1]<xy[3][1],"Image unsupported transformed/empty quad");
    check(quad.z==0xffff,"Image unsupported Z payload");
    for(float st:quad.st)check(std::isfinite(st),"Image invalid ST endpoint");
    check(quad.st[0]<=quad.st[2]&&quad.st[1]<=quad.st[3],"Image unsupported reversed ST rectangle");
    std::array<Vertex,4> corners;
    for(unsigned i=0;i<4;++i){
        const float x=(float(xy[i][0])-float(baseline.offset_x16))/16.f+.5f;
        const float y=(float(xy[i][1])-float(baseline.offset_y16))/16.f+.5f;
        corners[i]={{2*x/float(color.width)-1,1-2*y/float(color.height),0,1},
            {quad.st[(i==1||i==2)?2:0],quad.st[i>=2?3:1],1,0}};
    }
    std::array<Vertex,6> vertices{corners[0],corners[1],corners[2],corners[0],corners[2],corners[3]};
    std::array<std::uint32_t,4> rgba;for(unsigned i=0;i<4;++i)rgba[i]=(quad.rgba>>(8*i))&255;
    const std::uint32_t zero=0;
    auto errors=[device_ newBufferWithBytes:&zero length:sizeof(zero) options:MTLResourceStorageModeShared];check(errors,"Image validation buffer allocation");
    ImageMetalValidation validation(command,errors,std::move(texture));
    MTLScissorRect rect{std::min<NSUInteger>(baseline.scissor_x0,color.width),std::min<NSUInteger>(baseline.scissor_y0,color.height),0,0};
    rect.width=std::min<NSUInteger>(baseline.scissor_x1+1,color.width)-rect.x;
    rect.height=std::min<NSUInteger>(baseline.scissor_y1+1,color.height)-rect.y;
    if(!rect.width||!rect.height)return validation;
    auto buffer=[device_ newBufferWithBytes:vertices.data() length:sizeof(vertices) options:MTLResourceStorageModeShared];check(buffer,"Image vertex allocation");
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
    pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;
    auto e=[command renderCommandEncoderWithDescriptor:pass];check(e,"Image render encoder");
    [e setRenderPipelineState:pipeline_];[e setDepthStencilState:no_depth_];[e setCullMode:MTLCullModeNone];[e setScissorRect:rect];
    [e setVertexBuffer:buffer offset:0 atIndex:0];[e setFragmentTexture:validation.texture_.texture atIndex:0];
    [e setFragmentBytes:rgba.data() length:sizeof(rgba) atIndex:0];[e setFragmentBuffer:errors offset:0 atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];[e endEncoding];return validation;
}

ImageMetalValidation ImageMetal::encode(id<MTLCommandBuffer> command,id<MTLTexture> color,id<MTLTexture> depth,
        const ImageMetalQuad& quad,const ImageMetalBaseline& baseline,ImageMetalGsTexture source){
    check(command&&command.device==device_&&command.retainedReferences&&color&&depth&&color!=depth&&color.device==device_&&depth.device==device_&&
        color.sampleCount==1&&depth.sampleCount==1&&color.textureType==MTLTextureType2D&&depth.textureType==MTLTextureType2D&&
        color.pixelFormat==MTLPixelFormatRGBA8Unorm&&depth.pixelFormat==MTLPixelFormatDepth32Float&&
        color.width&&color.height&&color.width<=2048&&color.height<=2048&&color.width==depth.width&&color.height==depth.height,
        "Image render target format/extent");
    check(baseline.offset_x16<=65535&&baseline.offset_y16<=65535&&baseline.framebuffer_mask==0&&baseline.colclamp==1&&
        baseline.dither==0&&baseline.prmodecont==1,"Image unsupported inherited framebuffer state");
    check(baseline.scissor_x0<=baseline.scissor_x1&&baseline.scissor_y0<=baseline.scissor_y1&&baseline.scissor_x1<=2047&&baseline.scissor_y1<=2047,
        "Image invalid SCISSOR");

    check(source.snapshot.valid(),"Image shared GS snapshot is empty");
    const auto& xy=quad.xy16;
    for(const auto& p:xy)check(p[0]<=65535&&p[1]<=65535,"Image XY outside unsigned GS16 range");
    std::int64_t winding=0;
    for(unsigned i=0;i<4;++i) {
        const auto& a=xy[i];const auto& b=xy[(i+1)%4];const auto& c=xy[(i+2)%4];
        const auto cross=(std::int64_t(b[0])-a[0])*(std::int64_t(c[1])-b[1])-
                         (std::int64_t(b[1])-a[1])*(std::int64_t(c[0])-b[0]);
        check(cross!=0&&(!winding||(cross>0)==(winding>0)),"Image shared GS requires an ordered nonempty convex quad");
        winding=cross;
    }
    check(quad.z==0xffff,"Image unsupported Z payload");
    for(float st:quad.st)check(std::isfinite(st),"Image invalid ST endpoint");
    check(quad.st[0]<=quad.st[2]&&quad.st[1]<=quad.st[3],"Image unsupported reversed ST rectangle");
    std::array<Vertex,4> corners;
    for(unsigned i=0;i<4;++i){
        const float x=(float(xy[i][0])-float(baseline.offset_x16))/16.f+.5f;
        const float y=(float(xy[i][1])-float(baseline.offset_y16))/16.f+.5f;
        corners[i]={{2*x/float(color.width)-1,1-2*y/float(color.height),0,1},
            {quad.st[(i==1||i==2)?2:0],quad.st[i>=2?3:1],1,0}};
    }
    std::array<Vertex,6> vertices{corners[0],corners[1],corners[2],corners[0],corners[2],corners[3]};
    std::array<std::uint32_t,4> rgba;for(unsigned i=0;i<4;++i)rgba[i]=(quad.rgba>>(8*i))&255;
    if(!cached_gs_||cached_gs_->snapshot.identity()!=source.snapshot.identity()||
        cached_gs_->snapshot.storage_identity()!=source.snapshot.storage_identity()) {
        auto gs=std::make_shared<ImageMetalGsSnapshot>();gs->snapshot=source.snapshot;
        const auto bytes=gs->snapshot.physical_bytes(),known=gs->snapshot.known_nibbles();
        gs->bytes=[device_ newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
        gs->known=[device_ newBufferWithBytes:known.data() length:known.size() options:MTLResourceStorageModeShared];
        check(gs->bytes&&gs->known,"Image shared GS buffer allocation");cached_gs_=std::move(gs);
    }
    const std::uint32_t untouched=UINT32_MAX;
    auto errors=[device_ newBufferWithBytes:&untouched length:sizeof(untouched) options:MTLResourceStorageModeShared];check(errors,"Image validation buffer allocation");
    ImageMetalValidation validation(command,errors,ImageMetalTexture{nil,std::move(source.owner)},cached_gs_);
    MTLScissorRect rect{std::min<NSUInteger>(baseline.scissor_x0,color.width),std::min<NSUInteger>(baseline.scissor_y0,color.height),0,0};
    rect.width=std::min<NSUInteger>(baseline.scissor_x1+1,color.width)-rect.x;
    rect.height=std::min<NSUInteger>(baseline.scissor_y1+1,color.height)-rect.y;
    if(!rect.width||!rect.height)return validation;
    auto buffer=[device_ newBufferWithBytes:vertices.data() length:sizeof(vertices) options:MTLResourceStorageModeShared];check(buffer,"Image vertex allocation");
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
    pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;
    auto e=[command renderCommandEncoderWithDescriptor:pass];check(e,"Image render encoder");
    [e setRenderPipelineState:gs_pipeline_];[e setDepthStencilState:no_depth_];[e setCullMode:MTLCullModeNone];[e setScissorRect:rect];
    [e setVertexBuffer:buffer offset:0 atIndex:0];
    [e setFragmentBytes:rgba.data() length:sizeof(rgba) atIndex:0];[e setFragmentBuffer:errors offset:0 atIndex:1];
    [e setFragmentBuffer:validation.gs_->bytes offset:0 atIndex:2];[e setFragmentBuffer:validation.gs_->known offset:0 atIndex:3];
    [e setFragmentBytes:&source.registers length:sizeof(source.registers) atIndex:4];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];[e endEncoding];return validation;
}
}
