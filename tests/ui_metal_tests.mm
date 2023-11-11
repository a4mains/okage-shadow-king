#include "ui_metal.hpp"
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace okage;
namespace {
void check(bool yes,const char* text){if(!yes)throw std::runtime_error(text);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught,"Expected UI Metal guard");}
const UiMetalBaseline baseline{0x6c00,0x7900,true,0,1,0,1};
UiGeometryList list(){UiGeometryList l;
    l.commands={UiDrawControl::Begin,UiDrawRegister{0x49,0},UiDrawRegister{0x47,0x30003},UiDrawRegister{0x3b,0x100008080ull},
        UiDrawRegister{0x4a,0},UiDrawRegister{0x42,0x48},UiDrawRegister{0x40,(15ull<<48)|(31ull<<16)},UiDrawRegister{0x14,0x60},UiDrawRegister{8,5},UiDrawCurrentFramebuffer{}};return l;}
UiResolvedPrimitive quad(std::array<unsigned,4> rgba,unsigned z=100){UiResolvedPrimitive p{};p.prim=0x4d;p.quad.emitted=true;
    const std::array<std::array<unsigned,2>,4> xy={{{2,2},{10,2},{10,10},{2,10}}};
    for(unsigned i=0;i<4;++i){auto& v=p.quad.vertices[i];v.xyzf={0x6c00+xy[i][0]*16,0x7900+xy[i][1]*16,z<<4,0};
        for(unsigned j=0;j<4;++j)v.rgba[j]=0x4b000000|rgba[j];}return p;}
struct Result{std::vector<std::uint8_t> rgba;std::vector<float> depth;};
struct Harness{
    id<MTLDevice> device;id<MTLCommandQueue> queue;UiMetal renderer;
    explicit Harness(id<MTLDevice> d):device(d),queue([d newCommandQueue]),renderer(d){}
    Result run(const UiGeometryList& l,const UiMetalTextureResolver& resolver={},UiMetalBaseline state=baseline,
        std::array<unsigned,4> clear={3,5,7,255},UiMetal* backend=nullptr){
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:16 mipmapped:NO];
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;auto color=[device newTextureWithDescriptor:d];
        d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;auto depth=[device newTextureWithDescriptor:d];
        auto cb=[queue commandBuffer];auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(clear[0]/255.,clear[1]/255.,clear[2]/255.,clear[3]/255.);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1;
        auto e=[cb renderCommandEncoderWithDescriptor:pass];[e endEncoding];
        auto validation=(backend?backend:&renderer)->encode(cb,color,depth,l,state,resolver);
        rejects([&]{validation.validate_completed(cb);});
        rejects([&]{validation.validate_completed([queue commandBuffer]);});
        auto buffer=[device newBufferWithLength:256*16 options:MTLResourceStorageModeShared];auto blit=[cb blitCommandEncoder];
        [blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,16,1)
            toBuffer:buffer destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*16];[blit endEncoding];
        [cb commit];[cb waitUntilCompleted];check(cb.status==MTLCommandBufferStatusCompleted,"Metal command failed");validation.validate_completed(cb);
        Result result;result.rgba.resize(32*16*4);result.depth.resize(32*16);
        [color getBytes:result.rgba.data() bytesPerRow:32*4 fromRegion:MTLRegionMake2D(0,0,32,16) mipmapLevel:0];
        for(unsigned y=0;y<16;++y)std::memcpy(result.depth.data()+y*32,static_cast<const char*>(buffer.contents)+y*256,32*4);return result;
    }
};
std::array<unsigned,4> pixel(const Result& r,unsigned x=4,unsigned y=4){std::array<unsigned,4> a;for(unsigned i=0;i<4;++i)a[i]=r.rgba[(y*32+x)*4+i];return a;}
void arithmetic(Harness& h){auto l=list();l.commands.push_back(quad({129,129,129,127}));auto r=h.run(l);
    check(pixel(r)==std::array<unsigned,4>{130,132,134,127},"Integer ALPHA48 truncation/raw alpha");
    check(pixel(r,1,4)==std::array<unsigned,4>{3,5,7,255}&&pixel(r,10,4)==std::array<unsigned,4>{3,5,7,255},"GS pixel-center/quad boundary");
    check(pixel(r,2,4)==pixel(r),"GS left-edge inclusion");
    check(r.depth[4*32+4]==1.f-100.f/16777216.f&&r.depth[4*32+1]==1,"Z24 write/no clear");
    l.commands.push_back(quad({255,255,255,128},200));r=h.run(l);check(pixel(r)==std::array<unsigned,4>{255,255,255,128},"Ordered saturation and raw final alpha");
    auto zero=list();zero.commands.push_back(quad({240,240,240,0},100));zero.commands.push_back(UiDrawRegister{0x47,0x5000d});zero.commands.push_back(quad({255,255,255,128},50));
    r=h.run(zero);check(pixel(r)==std::array<unsigned,4>{3,5,7,0}&&r.depth[4*32+4]==1.f-100.f/16777216.f,"Alpha0 writes depth then GEQUAL rejects behind");
    auto keep=baseline;keep.depth_write=false;r=h.run(zero,{},keep);check(pixel(r)==std::array<unsigned,4>{255,255,255,128}&&r.depth[4*32+4]==1,"ZMSK baseline preserves depth");
    auto alpha_test=list();alpha_test.commands.push_back(UiDrawRegister{0x47,0x5000d});alpha_test.commands.push_back(quad({240,240,240,0},100));
    r=h.run(alpha_test);check(pixel(r)==std::array<unsigned,4>{3,5,7,255}&&r.depth[4*32+4]==1,"Source alpha-positive TEST discards all writes");
}
void textures(Harness& h){Texture t{2,1,0,0,{128,128,128,255,128,128,128,0}};auto gpu=upload_ui_gs_texture(h.device,t);
    std::array<unsigned char,8> bytes{};[gpu getBytes:bytes.data() bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0];check(bytes[3]==128,"GS alpha restored before filtering");
    UiGeometryTexture binding{UiGeometryTexture::Kind::Window,0,0x20000006a8000000ull,{},std::make_shared<int>(1)};
    auto make=[&](float texel){auto l=list();l.commands.push_back(binding);auto p=quad({128,128,128,128});p.prim=0x5d;p.texture=binding;
        for(auto& v:p.quad.vertices)v.stq={texel/1024.f,.5f/1024.f,1};l.commands.push_back(p);return l;};
    const UiMetalTextureResolver resolver=[&](const auto&){return UiMetalTexture{gpu,binding.window};};
    auto r=h.run(make(1),resolver);check(pixel(r)==std::array<unsigned,4>{67,69,71,64},"Half blend uses restored128 alpha, not expanded255 alpha");
    auto perspective=make(1);auto& projected=std::get<UiResolvedPrimitive>(perspective.commands.back());
    for(unsigned i=0;i<4;++i){const float q=float(i+1);projected.quad.vertices[i].stq={q/1024.f,.5f*q/1024.f,q};}
    r=h.run(perspective,resolver);check(pixel(r)==std::array<unsigned,4>{67,69,71,64},"Perspective STQ division precedes logical1024 addressing");
    r=h.run(make(-.5f),resolver);check(pixel(r)==std::array<unsigned,4>{131,133,135,128},"Logical clamp handles negative half texel");
    r=h.run(make(1.5f),resolver);check(pixel(r)==std::array<unsigned,4>{3,5,7,0}&&r.depth[4*32+4]==1.f-100.f/16777216.f,"Last texel center zero-weight outside tap and transparent depth write");
    auto beyond=make(.5f);auto& narrow=std::get<UiResolvedPrimitive>(beyond.commands.back());
    for(unsigned i=0;i<4;++i){const bool right=i==1||i==2;
        narrow.quad.vertices[i].xyzf[0]=0x6c00+(right?3:2)*16;
        narrow.quad.vertices[i].stq[0]=(right?2.5f:.5f)/1024.f;}
    r=h.run(beyond,resolver);check(pixel(r,2,4)==std::array<unsigned,4>{131,133,135,128},"Unsampled outside endpoint is not rejected");
    auto invalid=make(1.5001f);auto bad_draw=std::get<UiResolvedPrimitive>(invalid.commands.back());
    bad_draw.source={UiDrawPacketFamily::SpriteElement,1,4,0,7,0,0,{}};
    invalid.commands.pop_back();invalid.commands.push_back(quad({128,128,128,0}));
    invalid.commands.push_back(bad_draw);invalid.commands.push_back(bad_draw);
    bool sampled_error=false;
    try{h.run(invalid,resolver);}catch(const std::runtime_error& e){const std::string error=e.what();
        sampled_error=error.find("UI unsupported sampled texture footprint: fragment count=")!=std::string::npos&&
            error.find("first primitive 1 window 1 family 2 group 4 element 7")!=std::string::npos;}
    check(sampled_error,"Actual invalid taps report stable first primitive with source context");
    auto bad=t;bad.rgba[3]=127;rejects([&]{upload_ui_gs_texture(h.device,bad);});
    auto q=make(.5f);std::get<UiResolvedPrimitive>(q.commands.back()).quad.vertices[0].stq[2]=0;rejects([&]{h.run(q,resolver);});
}
void guards(Harness& h){const auto empty=h.run(list());check(pixel(empty)==std::array<unsigned,4>{3,5,7,255},"Empty submission validation token");auto l=list();l.commands[4]=UiDrawRegister{0x42,0x44};l.commands.push_back(quad({128,128,128,128}));rejects([&]{h.run(l);});
    l=list();l.commands[7]=UiDrawRegister{8,15};rejects([&]{h.run(l);});
    l=list();l.commands.push_back(quad({128,128,128,128}));auto b=baseline;b.colclamp=0;rejects([&]{h.run(l,{},b);});
    l=list();l.commands.push_back(UiDrawRegister{0x40,(3ull<<48)|(3ull<<16)});l.commands.push_back(quad({128,128,128,128}));const auto r=h.run(l);
    check(pixel(r,3,3)==std::array<unsigned,4>{131,133,135,128}&&pixel(r,4,3)==std::array<unsigned,4>{3,5,7,255},"Inclusive GS SCISSOR");
}
void prepared_backend_reuse(Harness& h,UiMetal& second){
    UiMetal::prepare_backend(h.device);
    auto geometry=list();geometry.commands.push_back(quad({129,129,129,127}));
    const auto first=h.run(geometry);
    const auto repeated=h.run(geometry,{},baseline,{3,5,7,255},&second);
    check(first.rgba==repeated.rgba&&first.depth==repeated.depth,
        "Prewarmed shared UI backend preserves independent renderer output");
}
}
int main(){@autoreleasepool{try{auto device=MTLCreateSystemDefaultDevice();UiMetal::prepare_backend(device);
Harness h(device);UiMetal second{device};prepared_backend_reuse(h,second);arithmetic(h);textures(h);guards(h);std::cout<<"UI Metal offscreen tests passed\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
