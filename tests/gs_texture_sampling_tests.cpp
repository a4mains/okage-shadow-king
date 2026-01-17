#include "okage/gs_texture_sampling.hpp"
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
            equal(sample_gs_texture(snapshot,r,(x+.25f)/1024,.25f/1024),expected);
        }
        if(psm==1||psm==27||psm==36||psm==44)
            require(!snapshot.observe_byte(gs_texel_address(0,128,2,0,0)->byte_address+(psm==1?3:0)).value,"unused byte should stay unknown");
    }
}
void all_clut_entries() {
    for(unsigned psm:{19u,20u}) {
        auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
        const unsigned count=psm==19?256:16;
        const auto allocation=memory->allocate(0,GsPixelStorage::Psmct32,16,clut_rect_rows(count));
        require(bool(allocation),"CLUT extent");upload_clut(*memory,allocation->id,count,palette(count));
        Bytes indices(psm==19?256:16);
        for(unsigned i=0;i<indices.size();++i)indices[i]=psm==19?i:((i*2%16)|(((i*2+1)%16)<<4));
        transfer_host_to_local(*memory,{psm,128,4,0,0,256,1},indices);
        const auto r=registers(psm,128,allocation->base_block);const auto snapshot=memory->snapshot();
        for(unsigned i=0;i<count;++i)equal(sample_gs_texture(snapshot,r,(i+.25f)/1024,.25f/1024),color(i));
    }
}
void filtering_and_bounds() {
    auto memory=std::make_unique<Ps2TextureMemory>();auto r=registers(0,0,0,true);
    memory->write_bytes(0,Bytes{20,40,60,128});
    equal(sample_gs_texture(memory->snapshot(),r,.5f/1024,.5f/1024),{20,40,60,128});
    rejects([&]{sample_gs_texture(memory->snapshot(),r,.75f/1024,.5f/1024);},"unknown texture nibble address=0x4 nibble=0");
    transfer_host_to_local(*memory,{0,0,2,0,0,2,2},Bytes{20,40,60,128,40,60,80,128,60,80,100,128,80,100,120,128});
    equal(sample_gs_texture(memory->snapshot(),r,1.f/1024,1.f/1024),{50,70,90,128});
    r.tex1=0;equal(sample_gs_texture(memory->snapshot(),r,1.f/1024,1.f/1024),{80,100,120,128});
    r.clamp=2u|(2u<<2)|(std::uint64_t(1)<<14)|(std::uint64_t(1)<<34);
    equal(sample_gs_texture(memory->snapshot(),r,-1,-1),{20,40,60,128});
    equal(sample_gs_texture(memory->snapshot(),r,2,2),{80,100,120,128});
    r.clamp=3u|(3u<<2)|(std::uint64_t(1)<<4)|(std::uint64_t(1)<<24);
    equal(sample_gs_texture(memory->snapshot(),r,-1.f/1024,-1.f/1024),{80,100,120,128});
    transfer_host_to_local(*memory,{0,0,2,11,4,4,1},Bytes(16,7));
    r.clamp=3u|(3u<<2)|(std::uint64_t(3)<<4)|(std::uint64_t(8)<<14)|
        (std::uint64_t(1)<<24)|(std::uint64_t(4)<<34);
    equal(sample_gs_texture(memory->snapshot(),r,-1.f/1024,0),{7,7,7,7});
    r=registers(0,0,0,false,0,0);equal(sample_gs_texture(memory->snapshot(),r,1.25f,.25f),{40,60,80,128});
    r=registers(0,0,0,true,1,1);
    r.clamp=5u|(std::uint64_t(1023)<<4)|(std::uint64_t(1023)<<24);
    equal(sample_gs_texture(memory->snapshot(),r,-.25f,2.f),{60,80,100,128});
    equal(sample_gs_texture(memory->snapshot(),r,2.f,-.25f),{40,60,80,128});
    equal(sample_gs_texture(memory->snapshot(),r,1.f,1.f),{80,100,120,128});
    r=registers(0,0,0,true,2,1);r.clamp=5;
    rejects([&]{sample_gs_texture(memory->snapshot(),r,2.f,.25f);},"unknown texture nibble");
}
void alpha_and_unknown() {
    auto memory=std::make_unique<Ps2TextureMemory>();auto r=registers(1,0);r.texa=0x80000080abull;
    transfer_host_to_local(*memory,{1,0,2,0,0,16,1},Bytes(48,0));
    equal(sample_gs_texture(memory->snapshot(),r,0,0),{0,0,0,0});
    r.texa&=~(std::uint64_t(1)<<15);equal(sample_gs_texture(memory->snapshot(),r,0,0),{0,0,0,171});
    memory->write_bytes(0,Bytes{1,2,3});r.texa|=std::uint64_t(1)<<15;
    equal(sample_gs_texture(memory->snapshot(),r,0,0),{1,2,3,171});
    memory->write_bytes(3,Bytes{255});
    equal(sample_gs_texture(memory->snapshot(),registers(0,0),0,0),{1,2,3,255});
    auto partial=std::make_unique<Ps2TextureMemory>();partial->write_nibble(3,0,0);
    auto indexed=registers(36,0,1);
    rejects([&]{sample_gs_texture(partial->snapshot(),indexed,0,0);},"unknown CLUT nibble address=0x100 nibble=0");
    partial->write_nibble(0x100,0,1);
    rejects([&]{sample_gs_texture(partial->snapshot(),indexed,0,0);},"unknown CLUT nibble address=0x100 nibble=1");
    indexed=registers(44,0,1);
    rejects([&]{sample_gs_texture(partial->snapshot(),indexed,0,0);},"unknown texture nibble address=0x3 nibble=1");
    partial->write_bytes(0x100,Bytes{9,8,7,255});indexed=registers(36,0,1);
    equal(sample_gs_texture(partial->snapshot(),indexed,0,0),{9,8,7,255});
}
void retired_snapshot() {
    auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
    const auto allocation=memory->allocate(1,GsPixelStorage::Psmct32,8,8);require(bool(allocation),"texture allocation");
    transfer_host_to_local(*memory,{0,allocation->base_block,1,0,0,4,1},Bytes(16,37));
    const auto before=memory->snapshot();const auto r=registers(0,allocation->base_block);
    require(memory->free_allocation(allocation->id),"retire texture");
    equal(sample_gs_texture(memory->snapshot(),r,0,0),{37,37,37,37});
    const auto physical=gs_texel_address(0,allocation->base_block,2,0,0)->byte_address;
    memory->write_bytes(physical,Bytes{90,91,92,93});
    equal(sample_gs_texture(before,r,0,0),{37,37,37,37});
    equal(sample_gs_texture(memory->snapshot(),r,0,0),{90,91,92,93});
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
    equal(sample_gs_texture(snapshot,r,36.5f/1024,64.5f/1024),color(91));
    const auto mixed=[](unsigned neighbor){auto a=color(17),b=color(neighbor);
        for(unsigned i=0;i<4;++i)a[i]=a[i]*.25f+b[i]*.75f;return a;};
    equal(sample_gs_texture(snapshot,r,36.5f/1024,64.25f/1024),mixed(91));
    memory->write_bytes(address,Bytes{92});
    equal(sample_gs_texture(snapshot,r,36.5f/1024,64.5f/1024),color(91));
    equal(sample_gs_texture(memory->snapshot(),r,36.5f/1024,64.5f/1024),color(92));
    equal(sample_gs_texture(memory->snapshot(),r,36.5f/1024,64.25f/1024),mixed(92));
    equal(sample_gs_texture(snapshot,r,36.5f/1024,64.25f/1024),mixed(91));
    memory->invalidate_owned_nibbles(neighbor->id);
    rejects([&]{sample_gs_texture(memory->snapshot(),r,36.5f/1024,64.5f/1024);},"unknown texture nibble");
    handles.retire_all();
}
void guards() {
    auto memory=std::make_unique<Ps2TextureMemory>();const auto snapshot=memory->snapshot();const auto good=registers(19);
    for(unsigned shift:{51u,55u,56u,61u}) {
        auto r=good;r.tex0^=std::uint64_t(1)<<shift;
        rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported CLUT");
    }
    auto r=good;r.tex1=1;rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported TEX1");
    r=good;r.tex0^=std::uint64_t(1)<<34;rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported TCC/TFX");
    r=good;r.tex0|=std::uint64_t(1)<<35;rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported TCC/TFX");
    r=good;r.tex0=(r.tex0&~(std::uint64_t(15)<<26))|(std::uint64_t(11)<<26);rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported TEX0 dimensions");
    r=good;r.clamp=2u|(2u<<2)|(std::uint64_t(1)<<4);
    rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported CLAMP bounds");
    r=good;r.clamp=0;rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported CLAMP mode");
    r=good;r.clamp|=std::uint64_t(1)<<44;rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported CLAMP reserved");
    r=good;r.tex0=(r.tex0&~(std::uint64_t(63)<<20))|(std::uint64_t(2)<<20);rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported PSM");
    rejects([&]{sample_gs_texture(snapshot,good,std::numeric_limits<float>::quiet_NaN(),0);},"unsupported ST");
    rejects([&]{sample_gs_texture(snapshot,good,std::numeric_limits<float>::infinity(),0);},"unsupported ST");
    rejects([&]{sample_gs_texture(snapshot,good,std::numeric_limits<float>::max(),0);},"unsupported ST");
    r=registers(1);r.texa|=std::uint64_t(1)<<16;
    rejects([&]{sample_gs_texture(snapshot,r,0,0);},"unsupported TEXA");
    rejects([&]{sample_gs_texture(Ps2TextureSnapshot{},good,0,0);},"unsupported empty snapshot");
}
}
int main(){try{formats();all_clut_entries();filtering_and_bounds();alpha_and_unknown();retired_snapshot();historical_neighbor_shape();guards();
    std::cout<<"GS texture sampling: formats, source CLUT lookup, filtering, required nibbles, retirement and 36/64 neighbor dependency passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
