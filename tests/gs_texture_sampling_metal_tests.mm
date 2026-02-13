#include "okage/gs_texture_sampling.hpp"
#include "gs_texture_sampling_metal.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include "okage/gs_texture_address.hpp"
#include "okage/texture_transfer.hpp"
#include "okage/texture_handles.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

using namespace okage;
namespace {
using Bytes = std::vector<std::uint8_t>;

class Differential {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> pipeline_;
    id<MTLBuffer> bytes_, known_;
    Ps2TextureSnapshot retained_;
public:
    unsigned samples=0, rejected=0;
    float max_difference=0;
    Differential() {
        device_=MTLCreateSystemDefaultDevice();
        if(!device_)throw std::runtime_error("Metal device unavailable");
        queue_=[device_ newCommandQueue];
        const std::string source=std::string(gs_texture_sampling_metal_source)+R"MSL(
        kernel void sample_test(device const uchar* bytes [[buffer(0)]],
                                device const uchar* known [[buffer(1)]],
                                constant GsTextureRegistersMetal& regs [[buffer(2)]],
                                constant float2* uv [[buffer(3)]],
                                device float4* rgba [[buffer(4)]],
                                device atomic_uint* invalid [[buffer(5)]],
                                uint gid [[thread_position_in_grid]]) {
            rgba[gid]=gs_sample_texture(bytes,known,regs,uv[gid],invalid);
        }
        )MSL";
        MTLCompileOptions* options=[MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled=NO;
#pragma clang diagnostic pop
        NSError* error=nil;
        id<MTLLibrary> library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if(!library)throw std::runtime_error(std::string("Metal library: ")+error.localizedDescription.UTF8String);
        pipeline_=[device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"sample_test"] error:&error];
        if(!pipeline_)throw std::runtime_error(std::string("Metal pipeline: ")+error.localizedDescription.UTF8String);
        bytes_=[device_ newBufferWithLength:Ps2TextureMemory::vram_bytes options:MTLResourceStorageModeShared];
        known_=[device_ newBufferWithLength:Ps2TextureMemory::vram_bytes options:MTLResourceStorageModeShared];
        if(!bytes_||!known_)throw std::runtime_error("Metal snapshot buffers unavailable");
        std::cout<<"GPU device: "<<device_.name.UTF8String<<"; fastMathEnabled=NO\n";
        // Optional provenance export is the exact string submitted to Metal.
        if(const char* path=std::getenv("OKAGE_GS_SAMPLER_SHADER_DUMP")) {
            FILE* f=std::fopen(path,"wb");
            if(!f)throw std::runtime_error("shader dump open");
            const auto written=std::fwrite(source.data(),1,source.size(),f);const int close=std::fclose(f);
            if(written!=source.size()||close)throw std::runtime_error("shader dump write");
        }
    }
    std::pair<std::array<float,4>,std::uint32_t> run_batch(const Ps2TextureSnapshot& snapshot,
             const GsTextureRegisters& regs,const std::vector<std::array<float,2>>& uv) {
        if(!retained_.valid()||retained_.storage_identity()!=snapshot.storage_identity()) {
            std::memset(bytes_.contents,0,Ps2TextureMemory::vram_bytes);
            std::memset(known_.contents,0,Ps2TextureMemory::vram_bytes);
            for(const auto address:snapshot.known_byte_addresses()) {
                const auto observation=snapshot.observe_byte_masked(address);
                static_cast<std::uint8_t*>(bytes_.contents)[address]=observation.value;
                static_cast<std::uint8_t*>(known_.contents)[address]=observation.known_mask;
            }
            retained_=snapshot;
        }
        id<MTLBuffer> rgba=[device_ newBufferWithLength:sizeof(float)*4*uv.size() options:MTLResourceStorageModeShared];
        id<MTLBuffer> invalid=[device_ newBufferWithLength:sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
        *static_cast<std::uint32_t*>(invalid.contents)=0xffffffffu;
        id<MTLCommandBuffer> command=[queue_ commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline_];
        [encoder setBuffer:bytes_ offset:0 atIndex:0];[encoder setBuffer:known_ offset:0 atIndex:1];
        [encoder setBytes:&regs length:sizeof(regs) atIndex:2];[encoder setBytes:uv.data() length:sizeof(uv[0])*uv.size() atIndex:3];
        [encoder setBuffer:rgba offset:0 atIndex:4];[encoder setBuffer:invalid offset:0 atIndex:5];
        [encoder dispatchThreads:MTLSizeMake(uv.size(),1,1) threadsPerThreadgroup:MTLSizeMake(std::min(uv.size(),pipeline_.maxTotalThreadsPerThreadgroup),1,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) {
            const char* detail=command.error.localizedDescription.UTF8String;
            throw std::runtime_error(std::string("Metal command failed: ")+(detail?detail:"unknown"));
        }
        std::array<float,4> result;std::memcpy(result.data(),rgba.contents,sizeof(result));
        return {result,*static_cast<std::uint32_t*>(invalid.contents)};
    }
    std::pair<std::array<float,4>,std::uint32_t> run(const Ps2TextureSnapshot& snapshot,
             const GsTextureRegisters& regs,float u,float v) {return run_batch(snapshot,regs,{{u,v}});}
};
Differential& differential(){static Differential value;return value;}
GsTextureSample differential_sample(const Ps2TextureSnapshot& snapshot,const GsTextureRegisters& regs,float u,float v) {
    auto& tester=differential();
    GsTextureSample cpu{};
    std::exception_ptr rejection;
    std::uint32_t expected=0xffffffffu;
    try {cpu=sample_gs_texture(snapshot,regs,u,v);}
    catch(const std::invalid_argument& e) {
        expected=std::string(e.what()).find("ST coordinate")!=std::string::npos?0xfffffffdu:0xfffffffeu;
        rejection=std::current_exception();
    }
    catch(const std::runtime_error& e) {
        const std::string message=e.what();
        const auto at=message.find("address=0x");unsigned address=0,nibble=0;
        if(at==std::string::npos||std::sscanf(message.c_str()+at,"address=0x%x nibble=%u",&address,&nibble)!=2)throw;
        expected=(address<<1)|nibble|(message.find("CLUT")!=std::string::npos?0x800000u:0u);
        rejection=std::current_exception();
    }
    const auto [gpu,invalid]=tester.run(snapshot,regs,u,v);++tester.samples;
    if(invalid!=expected)throw std::runtime_error("CPU/Metal invalid code mismatch: "+std::to_string(invalid)+" != "+std::to_string(expected));
    if(rejection) {
        ++tester.rejected;
        for(float component:gpu)if(!std::isnan(component))throw std::runtime_error("invalid sample must return NaNs");
        std::rethrow_exception(rejection);
    }
    for(unsigned c=0;c<4;++c) {
        if(!std::isfinite(gpu[c]))throw std::runtime_error("valid Metal sample is not finite");
        const float difference=std::abs(gpu[c]-cpu.rgba[c]);
        tester.max_difference=std::max(tester.max_difference,difference);
        if(difference>.0001f)throw std::runtime_error("CPU/Metal component mismatch: "+std::to_string(gpu[c])+" != "+std::to_string(cpu.rgba[c]));
    }
    return cpu;
}
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void equal(const GsTextureSample& s, std::array<float,4> expected) {
    for(unsigned i=0;i<4;++i) if(std::abs(s.rgba[i]-expected[i])>0.0001f)
        throw std::runtime_error("sample component "+std::to_string(i)+": "+std::to_string(s.rgba[i])+" != "+std::to_string(expected[i]));
}
template<class F> void rejects(F&& run, const char* text) {
    try {run();}catch(const std::exception& e){require(std::string(e.what()).find(text)!=std::string::npos,"wrong sample guard");return;}
    throw std::runtime_error(std::string("missing sample guard: ")+text);
}
GsTextureRegisters registers(unsigned psm, unsigned tbp=128, unsigned cbp=0, bool linear=false,
                             unsigned tw=10,unsigned th=10) {
    return {std::uint64_t(tbp)|(std::uint64_t(2)<<14)|(std::uint64_t(psm)<<20)|
        (std::uint64_t(tw)<<26)|(std::uint64_t(th)<<30)|(std::uint64_t(1)<<34)|
        (std::uint64_t(cbp)<<37)|(std::uint64_t(1)<<61),linear?0x60u:0u,
        2u|(2u<<2)|(std::uint64_t(1023)<<14)|(std::uint64_t(1023)<<34),0x8000008080ull};
}
std::array<float,4> color(unsigned i){return {float(i),float(i^85),float(255-i),128};}
Bytes palette(unsigned entries, bool other=false) {
    Bytes data(entries*4);
    for(unsigned i=0;i<entries;++i){const auto c=color(i);for(unsigned b=0;b<4;++b)data[i*4+b]=other?std::uint8_t(200+b):std::uint8_t(c[b]);}
    return data;
}
void put(Bytes& b,unsigned at,unsigned n){for(unsigned i=0;i<4;++i)b.at(at+i)=n>>(8*i);}
Bytes xim(unsigned width,unsigned height,unsigned index,bool other=false) {
    Bytes b(0x430+width*height);put(b,0,19<<20);put(b,16,0x410);put(b,24,1);put(b,28,256);
    const auto pal=palette(256,other);std::copy(pal.begin(),pal.end(),b.begin()+32);
    put(b,0x428,height);put(b,0x42c,width);std::fill(b.begin()+0x430,b.end(),index);return b;
}
void formats() {
    for(unsigned psm:{0u,1u,19u,20u,27u,36u,44u}) {
        auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
        const unsigned bits=psm==0?32:psm==1?24:(psm==19||psm==27)?8:4;
        const auto pal=memory->allocate(0,GsPixelStorage::Psmct32,16,16);require(bool(pal),"palette allocation");
        // Real retail palette upload, not test-specific swizzled memory writes.
        upload_clut(*memory,pal->id,256,palette(256));
        Bytes data(32*bits/8);
        for(unsigned i=0;i<32;++i) {
            if(bits==4)data[i/2]|=std::uint8_t((i%16)<<((i%2)*4));
            else if(bits==8)data[i]=i+8;
            else for(unsigned b=0;b<bits/8;++b)data[i*(bits/8)+b]=11+i+b*20;
        }
        transfer_host_to_local(*memory,{psm,128,2,0,0,32,1},data);
        const auto r=registers(psm,128,pal->base_block);const auto snapshot=memory->snapshot();
        for(unsigned x=0;x<32;++x) {
            const auto expected=bits<=8?color(bits==4?x%16:x+8):
                std::array<float,4>{float(11+x),float(31+x),float(51+x),psm==1?128.f:float(71+x)};
            equal(differential_sample(snapshot,r,(x+.25f)/1024,.25f/1024),expected);
        }
        if(psm==1||psm==27||psm==36||psm==44)
            require(!snapshot.observe_byte(gs_texel_address(0,128,2,0,0)->byte_address+(psm==1?3:0)).value,"unused byte should stay unknown");
    }
}
void all_clut_entries() {
    for(unsigned psm:{19u,20u,27u,36u,44u}) {
        auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
        const bool eight=psm==19||psm==27;
        const unsigned count=eight?256:16;
        const auto allocation=memory->allocate(0,GsPixelStorage::Psmct32,16,clut_rect_rows(count));
        require(bool(allocation),"CLUT extent");upload_clut(*memory,allocation->id,count,palette(count));
        Bytes indices(eight?256:16);
        for(unsigned i=0;i<indices.size();++i)indices[i]=eight?i:((i*2%16)|(((i*2+1)%16)<<4));
        transfer_host_to_local(*memory,{psm,128,4,0,0,256,1},indices);
        const auto r=registers(psm,128,allocation->base_block);const auto snapshot=memory->snapshot();
        for(unsigned i=0;i<count;++i)equal(differential_sample(snapshot,r,(i+.25f)/1024,.25f/1024),color(i));
    }
}
void filtering_and_bounds() {
    auto memory=std::make_unique<Ps2TextureMemory>();auto r=registers(0,0,0,true);
    memory->write_bytes(0,Bytes{20,40,60,128});
    equal(differential_sample(memory->snapshot(),r,.5f/1024,.5f/1024),{20,40,60,128});
    rejects([&]{differential_sample(memory->snapshot(),r,.75f/1024,.5f/1024);},"unknown texture nibble address=0x4 nibble=0");
    transfer_host_to_local(*memory,{0,0,2,0,0,2,2},Bytes{20,40,60,128,40,60,80,128,60,80,100,128,80,100,120,128});
    equal(differential_sample(memory->snapshot(),r,1.f/1024,1.f/1024),{50,70,90,128});
    r.tex1=0;equal(differential_sample(memory->snapshot(),r,1.f/1024,1.f/1024),{80,100,120,128});
    r.clamp=2u|(2u<<2)|(std::uint64_t(1)<<14)|(std::uint64_t(1)<<34);
    equal(differential_sample(memory->snapshot(),r,-1,-1),{20,40,60,128});
    equal(differential_sample(memory->snapshot(),r,2,2),{80,100,120,128});
    r.clamp=3u|(3u<<2)|(std::uint64_t(1)<<4)|(std::uint64_t(1)<<24);
    equal(differential_sample(memory->snapshot(),r,-1.f/1024,-1.f/1024),{80,100,120,128});
    transfer_host_to_local(*memory,{0,0,2,11,4,4,1},Bytes(16,7));
    r.clamp=3u|(3u<<2)|(std::uint64_t(3)<<4)|(std::uint64_t(8)<<14)|
        (std::uint64_t(1)<<24)|(std::uint64_t(4)<<34);
    equal(differential_sample(memory->snapshot(),r,-1.f/1024,0),{7,7,7,7});
    r=registers(0,0,0,false,0,0);equal(differential_sample(memory->snapshot(),r,1.25f,.25f),{40,60,80,128});
    r=registers(0,0,0,true,1,1);
    r.clamp=5u|(std::uint64_t(1023)<<4)|(std::uint64_t(1023)<<24);
    equal(differential_sample(memory->snapshot(),r,-.25f,2.f),{60,80,100,128});
    equal(differential_sample(memory->snapshot(),r,2.f,-.25f),{40,60,80,128});
    equal(differential_sample(memory->snapshot(),r,1.f,1.f),{80,100,120,128});
    r=registers(0,0,0,true,2,1);r.clamp=5;
    rejects([&]{differential_sample(memory->snapshot(),r,2.f,.25f);},"unknown texture nibble");
}
void alpha_and_unknown() {
    auto memory=std::make_unique<Ps2TextureMemory>();auto r=registers(1,0);r.texa=0x80000080abull;
    transfer_host_to_local(*memory,{1,0,2,0,0,16,1},Bytes(48,0));
    equal(differential_sample(memory->snapshot(),r,0,0),{0,0,0,0});
    r.texa&=~(std::uint64_t(1)<<15);equal(differential_sample(memory->snapshot(),r,0,0),{0,0,0,171});
    memory->write_bytes(0,Bytes{1,2,3});r.texa|=std::uint64_t(1)<<15;
    equal(differential_sample(memory->snapshot(),r,0,0),{1,2,3,171});
    memory->write_bytes(3,Bytes{255});
    equal(differential_sample(memory->snapshot(),registers(0,0),0,0),{1,2,3,255});
    auto partial=std::make_unique<Ps2TextureMemory>();partial->write_nibble(3,0,0);
    auto indexed=registers(36,0,1);
    rejects([&]{differential_sample(partial->snapshot(),indexed,0,0);},"unknown CLUT nibble address=0x100 nibble=0");
    partial->write_nibble(0x100,0,1);
    rejects([&]{differential_sample(partial->snapshot(),indexed,0,0);},"unknown CLUT nibble address=0x100 nibble=1");
    indexed=registers(44,0,1);
    rejects([&]{differential_sample(partial->snapshot(),indexed,0,0);},"unknown texture nibble address=0x3 nibble=1");
    partial->write_bytes(0x100,Bytes{9,8,7,255});indexed=registers(36,0,1);
    equal(differential_sample(partial->snapshot(),indexed,0,0),{9,8,7,255});
}
void retired_snapshot() {
    auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
    const auto allocation=memory->allocate(1,GsPixelStorage::Psmct32,8,8);require(bool(allocation),"texture allocation");
    transfer_host_to_local(*memory,{0,allocation->base_block,1,0,0,4,1},Bytes(16,37));
    const auto before=memory->snapshot();const auto r=registers(0,allocation->base_block);
    require(memory->free_allocation(allocation->id),"retire texture");
    equal(differential_sample(memory->snapshot(),r,0,0),{37,37,37,37});
    const auto physical=gs_texel_address(0,allocation->base_block,2,0,0)->byte_address;
    memory->write_bytes(physical,Bytes{90,91,92,93});
    equal(differential_sample(before,r,0,0),{37,37,37,37});
    equal(differential_sample(memory->snapshot(),r,0,0),{90,91,92,93});
}
void historical_neighbor_shape() {
    auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();TextureHandleOwner handles(*memory);
    const auto env=handles.load_xim(OwnedRawXim(xim(64,64,17)));
    const auto grass=handles.load_xim(OwnedRawXim(xim(128,128,91,true)));
    require(env.handle>=0&&grass.handle>=0,"actual image owner loads");
    const auto image=memory->allocation(handles.image(env.handle)->id);const auto clut=memory->allocation(handles.palette(env.handle,0)->id);
    const auto neighbor=memory->allocation(handles.image(grass.handle)->id);
    require(image&&clut&&neighbor,"actual descriptors");
    auto r=registers(19,image->base_block,clut->base_block,true);
    const auto address=gs_texel_address(19,image->base_block,image->buffer_width_64,36,64)->byte_address;
    require(memory->observe_byte(address).allocation_owner==neighbor->id,"36/64 must depend on another actual allocation");
    const auto snapshot=memory->snapshot();
    equal(differential_sample(snapshot,r,36.5f/1024,64.5f/1024),color(91));
    const auto mixed=[](unsigned neighbor){auto a=color(17),b=color(neighbor);
        for(unsigned i=0;i<4;++i)a[i]=a[i]*.25f+b[i]*.75f;return a;};
    equal(differential_sample(snapshot,r,36.5f/1024,64.25f/1024),mixed(91));
    memory->write_bytes(address,Bytes{92});
    equal(differential_sample(snapshot,r,36.5f/1024,64.5f/1024),color(91));
    equal(differential_sample(memory->snapshot(),r,36.5f/1024,64.5f/1024),color(92));
    equal(differential_sample(memory->snapshot(),r,36.5f/1024,64.25f/1024),mixed(92));
    equal(differential_sample(snapshot,r,36.5f/1024,64.25f/1024),mixed(91));
    memory->invalidate_owned_nibbles(neighbor->id);
    rejects([&]{differential_sample(memory->snapshot(),r,36.5f/1024,64.5f/1024);},"unknown texture nibble");
    handles.retire_all();
}

void address_pages_wrap_and_extrema() {
    for(unsigned psm:{0u,1u,19u,20u,27u,36u,44u}) {
        auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
        const auto pal=memory->allocate(0,GsPixelStorage::Psmct32,16,16);
        require(bool(pal),"edge palette");upload_clut(*memory,pal->id,256,palette(256));
        for(unsigned tbw:{0u,1u,3u,63u})for(const auto xy:{std::array<unsigned,2>{0,0},
            {7,15},{31,31},{63,63},{127,127},{128,128},{255,511},{1023,1023}}) {
            auto r=registers(psm,0x3fff,pal->base_block);
            r.tex0=(r.tex0&~(std::uint64_t(63)<<14))|(std::uint64_t(tbw)<<14);
            const auto address=*gs_texel_address(psm,0x3fff,tbw,xy[0],xy[1]);
            const unsigned value=psm>=16?(address.bits==4?13:213):0x9b734921;
            for(unsigned bit=0;bit<address.bits;bit+=4) {
                const auto offset=address.shift+bit;
                memory->write_nibble((address.byte_address+offset/8)&0x3fffff,(offset/4)&1,(value>>bit)&15);
            }
            const auto expected=psm>=16?color(value):std::array<float,4>{33,73,115,psm==1?128.f:155.f};
            equal(differential_sample(memory->snapshot(),r,(xy[0]+.25f)/1024,(xy[1]+.25f)/1024),expected);
        }
    }
    auto memory=std::make_unique<Ps2TextureMemory>();memory->write_bytes(0,Bytes{1,2,3,128});
    auto r=registers(0,0,0,false,0,0);
    r.clamp=2u|(2u<<2);
    const auto snapshot=memory->snapshot();
    equal(differential_sample(snapshot,r,std::nextafter(2147483648.f,0.f),0),{1,2,3,128});
    equal(differential_sample(snapshot,r,std::nextafter(-2147483648.f,0.f),0),{1,2,3,128});
    for(float bad:{2147483648.f,-2147483648.f})
        rejects([&]{differential_sample(snapshot,r,bad,0);},"unsupported ST");
    r=registers(0,0,0,true);
    equal(differential_sample(snapshot,r,std::nextafter(.5f,1.f)/1024,.5f/1024),{1,2,3,128});
    equal(differential_sample(snapshot,r,(.5f+15.f/256)/1024,.5f/1024),{1,2,3,128});
    rejects([&]{differential_sample(snapshot,r,(.5f+1.f/16)/1024,.5f/1024);},"unknown texture");
}

void concurrent_invalid_minimum() {
    auto memory=std::make_unique<Ps2TextureMemory>();
    auto r=registers(0,0);
    std::vector<std::array<float,2>> uv;
    for(unsigned i=0;i<64;++i)uv.push_back({float(63-i)/1024,0});
    // The result is the smallest encoded unknown, independent of thread order.
    require(differential().run_batch(memory->snapshot(),r,uv).second==0,"atomic minimum of texture faults");
    memory->write_nibble(3,0,0);r=registers(36,0,1);
    require(differential().run_batch(memory->snapshot(),r,{{0,0},{1.f/1024,0}}).second==14,
            "atomic minimum distinguishes CLUT role from texture role");
}
void guards() {
    auto memory=std::make_unique<Ps2TextureMemory>();const auto snapshot=memory->snapshot();const auto good=registers(19);
    for(unsigned shift:{51u,55u,56u,61u}) {
        auto r=good;r.tex0^=std::uint64_t(1)<<shift;
        rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported CLUT");
    }
    auto r=good;r.tex1=1;rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported TEX1");
    r=good;r.tex0^=std::uint64_t(1)<<34;rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported TCC/TFX");
    r=good;r.tex0|=std::uint64_t(1)<<35;rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported TCC/TFX");
    r=good;r.tex0=(r.tex0&~(std::uint64_t(15)<<26))|(std::uint64_t(11)<<26);rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported TEX0 dimensions");
    r=good;r.clamp=2u|(2u<<2)|(std::uint64_t(1)<<4);
    rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported CLAMP bounds");
    r=good;r.clamp=0;rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported CLAMP mode");
    r=good;r.clamp|=std::uint64_t(1)<<44;rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported CLAMP reserved");
    r=good;r.tex0=(r.tex0&~(std::uint64_t(63)<<20))|(std::uint64_t(2)<<20);rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported PSM");
    rejects([&]{differential_sample(snapshot,good,std::numeric_limits<float>::quiet_NaN(),0);},"unsupported ST");
    rejects([&]{differential_sample(snapshot,good,std::numeric_limits<float>::infinity(),0);},"unsupported ST");
    rejects([&]{differential_sample(snapshot,good,std::numeric_limits<float>::max(),0);},"unsupported ST");
    r=registers(1);r.texa|=std::uint64_t(1)<<16;
    rejects([&]{differential_sample(snapshot,r,0,0);},"unsupported TEXA");
}
}
int main(){@autoreleasepool{try{formats();all_clut_entries();filtering_and_bounds();alpha_and_unknown();retired_snapshot();historical_neighbor_shape();address_pages_wrap_and_extrema();concurrent_invalid_minimum();guards();
    std::cout<<"CPU/Metal samples="<<differential().samples<<", rejected="<<differential().rejected<<", max component difference="<<differential().max_difference<<"\n";
    std::cout<<"GS texture sampling: formats, source CLUT lookup, filtering, required nibbles, retirement and 36/64 neighbor dependency passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
