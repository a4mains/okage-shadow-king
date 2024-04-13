#include "image_metal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
using namespace okage;
namespace {
void check(bool yes,const char* text){if(!yes)throw std::runtime_error(text);}
template<class F>void rejects(F f,const char* why){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught,why);}
ImageMetalBaseline baseline(unsigned w=32,unsigned h=16){return {0x6c00,0x7900,0,w-1,0,h-1,0,1,0,1};}
ImageMetalQuad quad(unsigned x0=2,unsigned y0=2,unsigned x1=10,unsigned y1=10){
    return {{{{0x6c00+x0*16,0x7900+y0*16},{0x6c00+x1*16,0x7900+y0*16},
              {0x6c00+x1*16,0x7900+y1*16},{0x6c00+x0*16,0x7900+y1*16}}},
            {.5f/1024,.5f/1024,.5f/1024,.5f/1024},0x80808080,0xffff};
}
struct Result {unsigned width,height;std::vector<std::uint8_t> rgba;std::vector<float> depth;};
std::array<unsigned,4> pixel(const Result& r,unsigned x=4,unsigned y=4){
    std::array<unsigned,4> a;for(unsigned i=0;i<4;++i)a[i]=r.rgba.at((y*r.width+x)*4+i);return a;
}
struct Harness {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> queue=[device newCommandQueue];ImageMetal renderer{device};
    ImageMetalTexture texture(const Texture& t){return {upload_image_gs_texture(device,t,ImageMetalPreviewAlpha::SourceAtMost128),{}};}
    Result run(const ImageMetalQuad& q,ImageMetalTexture texture,ImageMetalBaseline state=baseline(),
            std::array<unsigned,4> clear={3,5,7,255},float clear_depth=0,unsigned width=32,unsigned height=16,bool twice=false){
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;auto color=[device newTextureWithDescriptor:d];
        d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;auto depth=[device newTextureWithDescriptor:d];
        auto cb=[queue commandBuffer];auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(clear[0]/255.,clear[1]/255.,clear[2]/255.,clear[3]/255.);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;
        pass.depthAttachment.clearDepth=clear_depth;auto e=[cb renderCommandEncoderWithDescriptor:pass];[e endEncoding];
        auto token=renderer.encode(cb,color,depth,q,state,texture);
        rejects([&]{token.validate_completed(cb);},"Premature completion must reject");
        rejects([&]{token.validate_completed([queue commandBuffer]);},"Wrong command identity must reject");
        std::optional<ImageMetalValidation> token2;if(twice)token2.emplace(renderer.encode(cb,color,depth,q,state,texture));
        const unsigned row=(width*4+255)&~255u;
        auto buffer=[device newBufferWithLength:row*height options:MTLResourceStorageModeShared];auto blit=[cb blitCommandEncoder];
        [blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
            toBuffer:buffer destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*height];[blit endEncoding];
        [cb commit];[cb waitUntilCompleted];token.validate_completed(cb);if(token2)token2->validate_completed(cb);
        Result result{width,height,std::vector<std::uint8_t>(width*height*4),std::vector<float>(width*height)};
        [color getBytes:result.rgba.data() bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        for(unsigned y=0;y<height;++y)std::memcpy(result.depth.data()+y*width,static_cast<const char*>(buffer.contents)+y*row,width*4);
        check(std::all_of(result.depth.begin(),result.depth.end(),[&](float v){return v==clear_depth;}),"TEST0x0f must preserve every depth sample");return result;
    }
};
void blend_and_edges(Harness& h){auto t=h.texture({1,1,0,0,{129,129,129,254}});auto q=quad();
    auto r=h.run(q,t,baseline(),{200,100,0,231});
    check(pixel(r)==std::array<unsigned,4>{129,128,127,127},"ALPHA44 signed difference/floor and raw source alpha");
    check(pixel(r,1,4)==std::array<unsigned,4>{200,100,0,231}&&pixel(r,10,4)==pixel(r,1,4),"GS left/right edge coverage");
    check(pixel(r,2,4)==pixel(r)&&pixel(r,4,2)==pixel(r)&&pixel(r,4,10)==pixel(r,1,4),"GS top/bottom and +0.5 lattice");
    auto opaque=h.texture({1,1,0,0,{16,32,64,255}});r=h.run(q,opaque,baseline(),{230,210,190,251},1);
    check(pixel(r)==std::array<unsigned,4>{16,32,64,128},"Opaque GS alpha128 replaces destination without additive blending");
    q.rgba=0x40808080;r=h.run(q,opaque,baseline(),{0,0,0,211},0,32,16,true);
    check(pixel(r)==std::array<unsigned,4>{12,24,48,64},"Ordered ALPHA44 submissions must read previous destination");
    q.rgba=0x00808080;r=h.run(q,opaque,baseline(),{25,51,77,211});
    check(pixel(r)==std::array<unsigned,4>{25,51,77,0},"Alpha0 passes source TEST and writes raw alpha");
    q.rgba=0xff808080;r=h.run(q,h.texture({1,1,0,0,{255,0,128,255}}),baseline(),{100,200,128,211});
    check(pixel(r)==std::array<unsigned,4>{255,0,128,255},"Over128 alpha extrapolation saturates after signed ALPHA44");
    auto b=baseline();b.scissor_x0=3;b.scissor_x1=4;b.scissor_y0=3;b.scissor_y1=4;
    r=h.run(quad(),opaque,b);check(pixel(r,3,3)==std::array<unsigned,4>{16,32,64,128}&&pixel(r,4,4)==pixel(r,3,3)&&
        pixel(r,5,4)==std::array<unsigned,4>{3,5,7,255}&&pixel(r,2,3)==pixel(r,5,4),"Inclusive inherited SCISSOR, no pass clear");
    b.scissor_x0=100;b.scissor_x1=110;r=h.run(quad(),opaque,b);check(pixel(r)==std::array<unsigned,4>{3,5,7,255},"Off-target scissor yields valid empty draw");
}
void sampling(Harness& h){Texture t{4,4,19,0,{}};
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)t.rgba.insert(t.rgba.end(),{std::uint8_t(x*64),std::uint8_t(y*64),0,255});
    auto q=quad();q.st={.5f/1024,.5f/1024,3.5f/1024,3.5f/1024};
    auto r=h.run(q,h.texture(t));
    check(pixel(r,2,2)==std::array<unsigned,4>{0,0,0,128},"Inset UV maps first pixel to first source texel center");
    check(pixel(r,4,4)==std::array<unsigned,4>{48,48,0,128}&&pixel(r,6,6)==std::array<unsigned,4>{96,96,0,128},
        "Two-dimensional linear gradient follows authored logical1024 endpoints");
    Texture alpha{2,1,19,0,{128,128,128,255,128,128,128,0}};auto gpu=h.texture(alpha);
    std::array<std::uint8_t,8> bytes{};[gpu.texture getBytes:bytes.data() bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0];
    check(bytes[3]==128&&bytes[7]==0,"Preview alpha reversal precedes filtering");
    q=quad();q.st={1.f/1024,.5f/1024,1.f/1024,.5f/1024};r=h.run(q,gpu);
    check(pixel(r)==std::array<unsigned,4>{65,66,67,64},"Fractional alpha filtering and ALPHA44 source-over");
    q.st={-.5f/1024,.5f/1024,-.5f/1024,.5f/1024};r=h.run(q,gpu);
    check(pixel(r)==std::array<unsigned,4>{128,128,128,128},"Logical clamp handles negative ST");
    q.st={1.5f/1024,.5f/1024,1.5f/1024,.5f/1024};r=h.run(q,gpu);
    check(pixel(r)==std::array<unsigned,4>{3,5,7,0},"Last texel zero-weight outside taps are valid");
    q.st[0]=q.st[2]=1.5001f/1024;
    bool footprint=false;try{h.run(q,gpu);}catch(const std::runtime_error& e){footprint=std::string(e.what()).find("Image unsupported sampled texture footprint:")!=std::string::npos;}
    check(footprint,"Actual nonzero-weight missing texel must fail command validation");
}
void raw_source_alpha(Harness& h){
    std::vector<std::uint8_t> bytes(40,0);bytes[24]=1;bytes[28]=2;
    const std::array<std::uint8_t,8> pixels{100,150,200,255,100,150,200,129};
    std::copy(pixels.begin(),pixels.end(),bytes.begin()+32);
    ImageMetalTexture gpu{upload_image_gs_source(h.device,bytes),{}};
    std::array<std::uint8_t,8> uploaded{};
    [gpu.texture getBytes:uploaded.data() bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0];
    check(uploaded==pixels,"Raw texture upload preserves all alpha bits");
    auto q=quad();auto r=h.run(q,gpu,baseline(),{200,100,0,64});
    check(pixel(r)==std::array<unsigned,4>{0,199,255,255},"Raw alpha255 extrapolates positive and negative differences");
    q.st={1.f/1024,.5f/1024,1.f/1024,.5f/1024};r=h.run(q,gpu,baseline(),{200,100,0,64});
    check(pixel(r)==std::array<unsigned,4>{50,175,255,192},"Raw alpha filtering occurs before modulation and blend");
    q.rgba=0x40808080;r=h.run(q,gpu,baseline(),{200,100,0,64});
    check(pixel(r)==std::array<unsigned,4>{125,137,150,96},"Raw alpha times vertex alpha retains source MODULATE order");
}
void authored_extent(Harness& h){Texture t{640,448,19,0,{}};t.rgba.resize(640*448*4);
    for(unsigned y=0;y<448;++y)for(unsigned x=0;x<640;++x){auto* p=t.rgba.data()+(y*640+x)*4;p[0]=std::uint8_t(x/4);p[1]=std::uint8_t(y/2);p[2]=64;p[3]=255;}
    ImageMetalQuad q{{{{27648,30976},{37888,30976},{37888,34560},{27648,34560}}},
        {.5f/1024,.5f/1024,639.5f/1024,447.5f/1024},0x80808080,0xffff};
    auto r=h.run(q,h.texture(t),baseline(640,224),{255,255,255,255},.25f,640,224);
    check(pixel(r,0,0)==std::array<unsigned,4>{0,0,64,128},"Authored source coordinates start at GS pixel0");
    const auto middle=pixel(r,320,112),last=pixel(r,639,223);
    check(middle[0]>=79&&middle[0]<=80&&middle[1]>=111&&middle[1]<=112&&middle[2]==64&&middle[3]==128,
        "Authored half-field geometry samples center of448-high texture");
    check(last[0]==159&&last[1]==222&&last[2]==64&&last[3]==128,"Authored last pixel keeps inset UV and source half-field sampling");
    check(std::all_of(r.rgba.begin()+3,r.rgba.end(),[i=0u](std::uint8_t v)mutable{return (i++%4)!=0||v==128;}),"Authored quad covers all640x224 pixels once");
}
void guards(Harness& h){auto t=h.texture({1,1,0,0,{16,32,64,255}});auto q=quad();
    auto bad=q;bad.xy16[1][1]+=1;rejects([&]{h.run(bad,t);},"Rotation/skew must remain guarded");
    bad=q;bad.z=1;rejects([&]{h.run(bad,t);},"Unsupported source Z must reject");
    bad=q;bad.st[0]=std::numeric_limits<float>::quiet_NaN();rejects([&]{h.run(bad,t);},"NaN ST must reject");
    bad=q;bad.st[0]=1;rejects([&]{h.run(bad,t);},"Reversed ST must reject");
    auto b=baseline();b.framebuffer_mask=1;rejects([&]{h.run(q,t,b);},"Unsupported framebuffer mask must reject");
    rejects([&]{h.texture({1,1,0,0,{16,32,64,127}});},"Irreversible preview alpha must reject");
    rejects([&]{h.texture({1,1,2,0,{16,32,64,255}});},"16-bit TEXA interpretation must remain guarded");
    rejects([&]{h.texture({1,1,19,2,{16,32,64,255}});},"16-bit palette TEXA interpretation must remain guarded");
    rejects([&]{h.run(q,{});},"Missing texture must reject");
}
}
int main(){@autoreleasepool{try{Harness h;blend_and_edges(h);sampling(h);raw_source_alpha(h);authored_extent(h);guards(h);
    std::cout<<"Image Metal offscreen tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
