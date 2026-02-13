#include "image_metal.hpp"
#include "okage/gs_texture_address.hpp"
#include "okage/pose.hpp"
#include "okage/texture_handles.hpp"
#include "okage/texture_transfer.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
using namespace okage;
namespace {
using Data=std::vector<std::uint8_t>;
using Pixel=std::array<unsigned,4>;
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
template<class F>void rejects(F run,const std::string& text){try{run();}catch(const std::exception& e){
    if(std::string(e.what()).find(text)==std::string::npos)throw std::runtime_error(std::string("wrong guard: ")+e.what());return;}
    throw std::runtime_error("missing guard: "+text);}
ImageMetalBaseline baseline(){return {0x6c00,0x7900,0,31,0,15,0,1,0,1};}
ImageMetalQuad quad(float x=.5f,float y=.5f){return {{{{0x6c20,0x7920},{0x6ca0,0x7920},{0x6ca0,0x79a0},{0x6c20,0x79a0}}},
    {x/1024,y/1024,x/1024,y/1024},0x80808080,0xffff};}
GsTextureRegisters regs(unsigned psm,unsigned tbp,unsigned tbw=1,unsigned cbp=0){return {
    0x20000006a8000000ull|tbp|(std::uint64_t(tbw)<<14)|(std::uint64_t(psm)<<20)|(std::uint64_t(cbp)<<37),
    0x60,5,0x810000807full};}
Pixel blend(const Ps2TextureSnapshot& s,const GsTextureRegisters& r,const ImageMetalQuad& q,Pixel d){
    const auto sampled=sample_gs_texture(s,r,q.st[0],q.st[1]).rgba;Pixel out;
    for(unsigned b=0;b<4;++b)out[b]=std::min(255u,unsigned(sampled[b]*float((q.rgba>>(8*b))&255)/128));
    const unsigned alpha=out[3];for(unsigned b=0;b<3;++b){const int n=(int(out[b])-int(d[b]))*int(alpha);
        // Portable signed floor division, matching MSL arithmetic shift.
        out[b]=std::clamp(int(d[b])+(n>=0?n/128:-((-n+127)/128)),0,255);}return out;
}
struct Pending {
    id<MTLCommandBuffer> command;id<MTLTexture> color;id<MTLBuffer> depth;
    std::vector<ImageMetalValidation> validations;
    void complete(){[command commit];[command waitUntilCompleted];for(const auto& v:validations)v.validate_completed(command);}
    Pixel pixel(unsigned x=4,unsigned y=4)const{std::array<std::uint8_t,4> p{};
        [color getBytes:p.data() bytesPerRow:4 fromRegion:MTLRegionMake2D(x,y,1,1) mipmapLevel:0];return {p[0],p[1],p[2],p[3]};}
    void depth_unchanged()const{for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x){float v;
        std::memcpy(&v,static_cast<const char*>(depth.contents)+y*256+x*4,4);check(v==.25f,"Image TESTf changed inherited depth");}}
};
struct Harness {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> queue=[device newCommandQueue];
    std::unique_ptr<ImageMetal> renderer=std::make_unique<ImageMetal>(device);
    Pending encode(ImageMetalGsTexture source,ImageMetalQuad q=quad(),ImageMetalBaseline b=baseline(),Pixel clear={200,100,0,64},bool twice=false){
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:16 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
        auto color=[device newTextureWithDescriptor:descriptor];descriptor.pixelFormat=MTLPixelFormatDepth32Float;descriptor.storageMode=MTLStorageModePrivate;
        auto depth=[device newTextureWithDescriptor:descriptor];auto cb=[queue commandBuffer];
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(clear[0]/255.,clear[1]/255.,clear[2]/255.,clear[3]/255.);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;
        pass.depthAttachment.clearDepth=.25;auto encoder=[cb renderCommandEncoderWithDescriptor:pass];[encoder endEncoding];
        Pending out{cb,color,[device newBufferWithLength:256*16 options:MTLResourceStorageModeShared],{}};
        out.validations.push_back(renderer->encode(cb,color,depth,q,b,source));
        rejects([&]{out.validations[0].validate_completed(cb);},"successful command completion");
        rejects([&]{out.validations[0].validate_completed([queue commandBuffer]);},"identity");
        if(twice)out.validations.push_back(renderer->encode(cb,color,depth,q,b,source));
        auto blit=[cb blitCommandEncoder];[blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(32,16,1) toBuffer:out.depth destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*16];
        [blit endEncoding];return out;
    }
    Pixel run(ImageMetalGsTexture source,ImageMetalQuad q=quad(),ImageMetalBaseline b=baseline(),Pixel clear={200,100,0,64},bool twice=false){
        auto p=encode(std::move(source),q,b,clear,twice);p.complete();p.depth_unchanged();return p.pixel();}
};
void raw_blend_filter_and_clamp(Harness& h){
    auto m=std::make_unique<Ps2TextureMemory>();m->initialize_empty_allocator();
    transfer_host_to_local(*m,{0,128,1,0,0,4,1},Data{100,150,200,255,100,150,200,129,32,64,96,128,8,16,24,0});
    auto r=regs(0,128);auto s=m->snapshot();
    for(float x:{.5f,1.f,1.5f,2.5f,3.5f}){const auto q=quad(x);
        check(h.run({s,r,{ }},q)==blend(s,r,q,{200,100,0,64}),"Raw shared GS alpha/filter/blend mismatch");}
    auto q=quad(1);q.rgba=0x40808080;check(h.run({s,r,{}},q)==blend(s,r,q,{200,100,0,64}),"Vertex alpha modulation order");
    q=quad(2.5f);q.rgba=0x82808080;
    check(h.run({s,r,{}},q)==Pixel{29,63,97,130},"NAVI upper-pulse vertex alpha130 preserves source signed blend beyond alpha128");
    q=quad(.5f);const auto once=blend(s,r,q,{200,100,0,64});
    check(h.run({s,r,{}},q,baseline(),{200,100,0,64},true)==blend(s,r,q,once),"Ordered Image GS blend did not read previous destination");
    // Standard clamp uses TW/TH, ignoring region fields, not uploaded width.
    r.tex0=(r.tex0&~((15ull<<26)|(15ull<<30)))|(2ull<<26);r.clamp=5|(1023ull<<4)|(3ull<<14)|(1023ull<<24)|(1ull<<34);
    check(h.run({s,r,{}},quad(-10,-10))==blend(s,r,quad(.5f),{200,100,0,64}),"Exact CLAMP5 negative edge");
    check(h.run({s,r,{}},quad(2000,2000))==blend(s,r,quad(896,512),{200,100,0,64}),"Exact CLAMP5 TW/TH upper edge");
    r.tex0=(r.tex0&~(15ull<<26))|(3ull<<26);
    rejects([&]{h.run({s,r,{}},quad(960,512));},"unknown shared GS texture nibble");
    r.tex0=(r.tex0&~(15ull<<26))|(10ull<<26);r.clamp=0;
    rejects([&]{h.run({s,r,{}},quad());},"unsupported shared GS register/coordinate state");
}
void put(Data& data,unsigned offset,unsigned value){for(unsigned b=0;b<4;++b)data.at(offset+b)=value>>(8*b);}
Data xim44(){Data b(112+16,0x55);std::fill(b.begin(),b.begin()+112,0);put(b,0,44u<<20);put(b,16,80);put(b,24,1);put(b,28,16);
    for(unsigned i=0;i<16;++i){b[32+i*4]=i*7;b[33+i*4]=i*9;b[34+i*4]=i*11;b[35+i*4]=128;}
    put(b,104,1);put(b,108,32);return b;}
void high_half_retention_and_unknown(Harness& h){
    auto m=std::make_unique<Ps2TextureMemory>();m->initialize_empty_allocator();auto handles=std::make_unique<TextureHandleOwner>(*m);
    const int low=handles->create_image(GsPixelStorage::Psmt4Hl,32,1,Data(16,0xaa));check(low>=0,"low lane owner creation");
    const auto high=handles->load_xim(OwnedRawXim(xim44()));check(high.handle>=0,"PSM44 XIM owner load");
    const auto image=*m->allocation(handles->image(high.handle)->id),pal=*m->allocation(handles->palette(high.handle,0)->id);
    const auto low_image=*m->allocation(handles->image(low)->id);check(image.base_block==low_image.base_block,"Actual high/low owners must alias");
    auto r=regs(44,image.base_block,image.buffer_width_64,pal.base_block);auto old=m->snapshot();
    auto owner=std::make_shared<int>(42);std::weak_ptr<int> weak=owner;
    auto pending=h.encode({old,r,owner});owner.reset();check(!weak.expired(),"Validation token failed to retain source owner");
    const auto a=*gs_texel_address(44,image.base_block,image.buffer_width_64,0,0);
    check(a.shift==4&&a.bits==4&&old.observe_nibble(a.byte_address,1).allocation_owner==image.id&&
        old.observe_nibble(a.byte_address,0).allocation_owner==low_image.id,"Lane owner fixture incorrect");
    m->write_nibble(a.byte_address,0,1);check(h.run({m->snapshot(),r,{}})==Pixel{35,45,55,128},"Other half changed44 sample");
    check(m->invalidate_owned_nibbles(low_image.id)>0,"Low owner invalidation fixture");
    check(h.run({m->snapshot(),r,{}})==Pixel{35,45,55,128},"Other half unknown must not reject44");
    m->write_nibble(a.byte_address,1,6);check(h.run({m->snapshot(),r,{}})==Pixel{42,54,66,128},"New frame ignored physical high nibble overwrite");
    check(m->invalidate_owned_nibbles(image.id)>0,"High owner invalidation fixture");
    std::ostringstream expected;expected<<"texture nibble address=0x"<<std::hex<<a.byte_address<<std::dec<<" nibble=1 owner="<<image.id;
    rejects([&]{h.run({m->snapshot(),r,{}});},expected.str());
    handles->retire_all();
    std::ostringstream freed;freed<<"texture nibble address=0x"<<std::hex<<a.byte_address<<" nibble=1 owner=none";
    rejects([&]{h.run({m->snapshot(),r,{}});},freed.str());
    const auto replacement=handles->create_image(GsPixelStorage::Psmt4Hh,32,1,Data(16,0x77));check(replacement>=0,"Replacement owner load");
    // Encode retained original after renderer cache has seen newer snapshots.
    auto replay=h.encode({old,r,{}});h.renderer.reset();handles.reset();m.reset();
    pending.complete();replay.complete();check(pending.pixel()==Pixel{35,45,55,128}&&replay.pixel()==pending.pixel(),"Retained GPU snapshot changed across write/free/reuse/destruction");
    pending.depth_unchanged();replay.depth_unchanged();pending.validations.clear();check(weak.expired(),"Completed token owner lifetime leaked");
    h.renderer=std::make_unique<ImageMetal>(h.device);
}
void unknown_weights_and_clut(Harness& h){
    auto m=std::make_unique<Ps2TextureMemory>();m->initialize_empty_allocator();m->write_bytes(0,Data{20,40,60,128});auto r=regs(0,0);
    auto independent=std::make_unique<Ps2TextureMemory>();independent->initialize_empty_allocator();independent->write_bytes(0,Data{60,80,100,128});
    check(independent->identity()==m->identity(),"Independent cache identity fixture");
    check(h.run({independent->snapshot(),r,{}})==Pixel{60,80,100,128},"Independent snapshot fixture sample");
    check(h.run({m->snapshot(),r,{}})==Pixel{20,40,60,128},"Zero-weight unknown neighbors incorrectly read");
    rejects([&]{h.run({m->snapshot(),r,{}},quad(.75f));},"texture nibble address=0x4 nibble=0 owner=none");
    m->write_nibble(3,1,0);m->write_nibble(256,0,1);r=regs(44,0,1,1);
    rejects([&]{h.run({m->snapshot(),r,{}});},"CLUT nibble address=0x100 nibble=1 owner=none");
}
void convex_and_scissor(Harness& h){
    auto m=std::make_unique<Ps2TextureMemory>();m->write_bytes(0,Data{16,32,64,128});const auto s=m->snapshot();const auto r=regs(0,0);
    auto q=quad();q.xy16={{{0x6c80,0x7920},{0x6ce0,0x7980},{0x6c80,0x79e0},{0x6c20,0x7980}}};
    auto p=h.encode({s,r,{}},q);p.complete();p.depth_unchanged();check(p.pixel(8,8)==Pixel{16,32,64,128}&&p.pixel(2,2)==Pixel{200,100,0,64},"Rotated convex fan coverage");
    Data gradient;for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)gradient.insert(gradient.end(),{std::uint8_t(x*40),std::uint8_t(y*40),64,128});
    transfer_host_to_local(*m,{0,0,1,0,0,4,4},gradient);const auto varied=m->snapshot();q.st={.5f/1024,.5f/1024,3.5f/1024,3.5f/1024};
    p=h.encode({varied,r,{}},q);p.complete();check(p.pixel(8,8)==Pixel{60,60,64,128},"Rotated fan lost authored corner ST interpolation");
    q.st={.5f/1024,.5f/1024,.5f/1024,.5f/1024};
    std::reverse(q.xy16.begin(),q.xy16.end());p=h.encode({s,r,{}},q);p.complete();check(p.pixel(8,8)==Pixel{16,32,64,128},"Reverse winding rejected/dropped");
    auto b=baseline();b.scissor_x0=3;b.scissor_x1=4;b.scissor_y0=3;b.scissor_y1=4;
    p=h.encode({s,r,{}},quad(),b);p.complete();check(p.pixel(3,3)==Pixel{16,32,64,128}&&p.pixel(4,4)==p.pixel(3,3)&&p.pixel(5,4)==Pixel{200,100,0,64},"Inclusive inherited scissor changed");
    b=baseline();b.scissor_x1=32;b.scissor_y1=16;p=h.encode({s,r,{}},quad(),b);p.complete();
    check(p.pixel()==Pixel{16,32,64,128},"Inclusive scissor equal to target extent must intersect");
    b.scissor_x0=100;b.scissor_x1=110;p=h.encode({Ps2TextureMemory{}.snapshot(),r,{}},quad(),b);p.complete();check(p.pixel()==Pixel{200,100,0,64},"Empty scissor must not read unknown GS");
    q=quad();q.xy16[2]=q.xy16[0];rejects([&]{h.encode({s,r,{}},q);},"convex quad");
    q=quad();std::swap(q.xy16[1],q.xy16[2]);rejects([&]{h.encode({s,r,{}},q);},"convex quad");
    q=quad();q.xy16[2]=q.xy16[1];rejects([&]{h.encode({s,r,{}},q);},"convex quad");
}
}
void rotated_navigation_marker_fraction(Harness& h){
    auto m=std::make_unique<Ps2TextureMemory>();m->initialize_empty_allocator();
    Data indices(512);for(unsigned i=0;i<512;++i)indices[i]=std::uint8_t(i*37+11);
    Data clut(1024);for(unsigned i=0;i<256;++i){clut[i*4]=i;clut[i*4+1]=255-i;clut[i*4+2]=(i*3)&255;clut[i*4+3]=128;}
    transfer_host_to_local(*m,{27,64,1,0,0,32,16},indices);transfer_host_to_local(*m,{0,512,1,0,0,16,16},clut);
    const auto r=regs(27,64,1,512);const auto snapshot=m->snapshot();
    const auto below=*gs_texel_address(27,64,1,3,16);
    check(!snapshot.observe_nibble(below.byte_address,0).known,"Fixture row16 must be undefined memory");
    // Live field target and inherited environment: 640x224, OFFSET 6c00/7900.
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
    auto color=[h.device newTextureWithDescriptor:descriptor];descriptor.pixelFormat=MTLPixelFormatDepth32Float;
    descriptor.storageMode=MTLStorageModePrivate;auto depth=[h.device newTextureWithDescriptor:descriptor];
    const ImageMetalBaseline field{0x6c00,0x7900,0,639,0,223,0,1,0,1};
    const auto sine=make_sine_table();
    const auto words=[](float x,float y,float angle,float alpha){return std::array<std::uint32_t,6>{std::bit_cast<std::uint32_t>(x),
        std::bit_cast<std::uint32_t>(y),0x3f800000u,0x3f800000u,std::bit_cast<std::uint32_t>(angle),std::bit_cast<std::uint32_t>(alpha)};};
    unsigned draws=0;
    for(unsigned position=0;position<16;++position){
        auto command=[h.queue commandBuffer];std::vector<ImageMetalValidation> validations;
        for(unsigned a=0;a<256;++a){
            const float x=-3840.f+float(position*101%977)+float(a%7)*.37f,y=448.f+float(position*53%409)-200.f+float(a%5)*.61f;
            validations.push_back(h.renderer->encode(command,color,depth,
                source_image_quad(16,16,words(x,y,float(a)/256.f+float(position)/4096.f,.8f),sine),field,{snapshot,r,{}}));
            ++draws;
        }
        [command commit];[command waitUntilCompleted];for(const auto& v:validations)v.validate_completed(command);
    }
    check(draws==4096,"Rotated marker sweep count");
}
int main(){@autoreleasepool{try{Harness h;rotated_navigation_marker_fraction(h);raw_blend_filter_and_clamp(h);unknown_weights_and_clut(h);convex_and_scissor(h);high_half_retention_and_unknown(h);
    std::cout<<"Image Metal shared GS raw blend, exact CLAMP5, positive tap/nibble guards, convex fan and retained owner tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
