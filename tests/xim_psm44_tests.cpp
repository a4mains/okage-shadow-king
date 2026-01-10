#include "okage/assets.hpp"
#include "okage/gs_texture_address.hpp"
#include "okage/gs_texture_sampling.hpp"
#include "okage/sha256.hpp"
#include "okage/texture_handles.hpp"
#include "okage/texture_transfer.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace okage;
using Data=std::vector<std::uint8_t>;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static void put(Data& bytes,std::size_t at,std::uint32_t value){for(unsigned b=0;b<4;++b)bytes.at(at+b)=value>>(b*8);}
static std::uint32_t get(const Data& bytes,std::size_t at){std::uint32_t value=0;for(unsigned b=0;b<4;++b)value|=std::uint32_t(bytes.at(at+b))<<(b*8);return value;}
template<class F>static void rejects(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,"missing format guard");}
static Data authored(unsigned width,unsigned height,unsigned cpsm=0,unsigned levels=1) {
    const unsigned stride=16*(cpsm==2?2:4),image_header=32+stride*levels;
    Data bytes(image_header+16+(width*height+1)/2);
    put(bytes,0,(0x81234567u&0xfc0fffffu) | (20u<<20));put(bytes,4,cpsm<<19);
    put(bytes,8,0xabcde123);put(bytes,12,0x89123456);put(bytes,16,16+stride*levels);put(bytes,24,levels);put(bytes,28,16);
    for(unsigned level=0;level<levels;++level)for(unsigned index=0;index<16;++index) {
        const unsigned at=32+level*stride+index*(cpsm==2?2:4);
        if(cpsm==2){bytes[at]=index;bytes[at+1]=index+1;}
        else {bytes[at]=index*7+level;bytes[at+1]=index*9+level;bytes[at+2]=index*11+level;bytes[at+3]=std::array<unsigned,4>{0,64,128,255}[index%4];}
    }
    put(bytes,image_header,0xdeadbeef);put(bytes,image_header+4,0x76543210);put(bytes,image_header+8,height);put(bytes,image_header+12,width);
    for(unsigned index=0;index<width*height;++index)bytes[image_header+16+index/2]|=(index%16)<<((index%2)*4);
    return bytes;
}
static Data navigation_override(Data bytes) {
    const auto original=bytes;put(bytes,0,(get(bytes,0)&0xfc0fffffu)|(44u<<20));
    require((get(bytes,0)&0xfc0fffffu)==(get(original,0)&0xfc0fffffu),"override preserves non-PSM header bits");
    require(std::equal(bytes.begin()+4,bytes.end(),original.begin()+4),"override changes no palette/header/payload bytes");return bytes;
}
static GsTextureRegisters registers(const GsTextureAllocation& image,const GsTextureAllocation& clut) {
    return {0x20000006a8000000ull|image.base_block|(std::uint64_t(image.buffer_width_64)<<14)|(44ull<<20)|(std::uint64_t(clut.base_block)<<37),0,
        2u|(2u<<2)|(std::uint64_t(image.width-1)<<14)|(std::uint64_t(image.height-1)<<34),0x8000008080ull};
}
static void load_and_check(const Data& overridden,bool coexist) {
    OwnedRawXim xim(overridden);const auto& layout=xim.layout();
    require(layout.pixel_format==44&&layout.bits_per_pixel==4&&layout.indexed&&layout.palette_format==0,"44 indexed layout");
    require(xim_upload_support(layout).status==XimUploadStatus::Supported,"44 upload classified supported");
    auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();TextureHandleOwner handles(*memory);
    OptDescriptor low24,low4;
    if(coexist) {
        const int direct=handles.create_image(GsPixelStorage::Psmct24,layout.width,layout.height,Data(layout.width*layout.height*3,0x6d));require(direct>=0,"low24 owner");low24=handles.image(direct);
        const int nibble=handles.create_image(GsPixelStorage::Psmt4Hl,layout.width,layout.height,Data((layout.width*layout.height+1)/2,0xaa));require(nibble>=0,"low4 owner");low4=handles.image(nibble);
    }
    const auto before=memory->snapshot();const auto result=handles.load_xim(xim);require(result.handle>=0,"44 source handle load");
    const auto image=*memory->allocation(handles.image(result.handle)->id);const auto palette=*memory->allocation(handles.palette(result.handle,0)->id);
    require(image.psm==GsPixelStorage::Psmt4Hh&&image.width==layout.width&&image.height==layout.height,"44 source descriptor");
    require(result.palettes.size()==layout.palette_level_count&&result.palettes.front().uploaded,"palette levels attempted/uploaded");
    if(coexist)require(memory->allocation(low24->id)->base_block==image.base_block&&memory->allocation(low4->id)->base_block==image.base_block,"real descriptors share high-lane words");
    const auto snapshot=memory->snapshot();const auto regs=registers(image,palette);const auto payload=xim.image();
    const auto sent=retail_transfer_qwc(44,layout.width,layout.height)*32;
    for(unsigned index=0;index<layout.width*layout.height;++index) {
        const unsigned x=index%layout.width,y=index/layout.width;const auto address=*gs_texel_address(44,image.base_block,image.buffer_width_64,x,y);
        require(address.shift==4&&address.bits==4,"high-half texel lane");const auto observed=snapshot.observe_nibble(address.byte_address,1);
        if(index>=sent){require(!observed.known,"floor-QWC remainder stays unknown");continue;}
        const unsigned value=(payload[index/2]>>((index%2)*4))&15;
        require(observed.known&&observed.value==value&&observed.allocation_owner==image.id,"packed nibble source upload");
        require(!before.observe_nibble(address.byte_address,1).known,"pre-upload snapshot high-half remains unknown");
        if(coexist) {
            const auto low=snapshot.observe_nibble(address.byte_address,0);require(low.known&&low.value==10&&low.allocation_owner==low4->id,"low-half contents/owner preserved");
            for(unsigned b=0;b<3;++b)require(snapshot.observe_byte(address.byte_address-3+b).value==0x6d&&snapshot.observe_nibble(address.byte_address-3+b,0).allocation_owner==low24->id,"low24 contents/owner preserved");
        } else {
            require(!snapshot.observe_nibble(address.byte_address,0).known,"other high-byte half stays unknown");
            for(unsigned b=0;b<3;++b)require(!snapshot.observe_byte(address.byte_address-3+b).value,"low24 stays unknown");
        }
        const auto sample=sample_gs_texture(snapshot,regs,(x+.25f)/1024,(y+.25f)/1024);
        const auto colors=xim.palette_level(0);
        for(unsigned c=0;c<4;++c)require(sample.rgba[c]==colors[value*4+c],"44 source CLUT sample selects original palette word");
    }
    handles.retire_all();
}
static void generated() {
    const auto input=authored(56,112),overridden=navigation_override(input);
    const auto ordinary=decode_xim(input),hh=decode_xim(overridden);require(ordinary.rgba==hh.rgba&&hh.pixel_format==44,"44 display decoding keeps packed-index semantics");
    const auto raw=decode_xim_gs32(overridden);const auto layout=parse_xim_layout(overridden);
    for(unsigned index=0;index<56*112;++index)for(unsigned c=0;c<4;++c) {
        const auto expected=overridden[32+(index%16)*4+c];require(raw.rgba[index*4+c]==expected,"44 raw RGBA preserves GS alpha");
        require(hh.rgba[index*4+c]==(c==3?std::min(255u,unsigned(expected)*2):expected),"44 display alpha policy unchanged");
    }
    require(layout.image_payload_size==56*112/2&&layout.image_header_words[0]==0xdeadbeef,"44 raw payload/stale header preserved");
    load_and_check(overridden,false);load_and_check(overridden,true);
    load_and_check(navigation_override(authored(33,1)),false);
    load_and_check(navigation_override(authored(32,1,0,2)),false);
    // Also traverse the raw upload API, not only the handle wrapper.
    auto memory=std::make_unique<Ps2TextureMemory>();memory->initialize_empty_allocator();
    const auto uploaded=upload_xim(*memory,OwnedRawXim(overridden));require(uploaded&&uploaded->palette_attempts.size()==1&&uploaded->palette_attempts[0].uploaded,"44 raw upload image/palette");
    require(memory->allocation(uploaded->image)->psm==GsPixelStorage::Psmt4Hh,"raw upload allocates44");
    for(unsigned psm:{2u,10u,36u,48u}){auto bad=overridden;put(bad,0,(get(bad,0)&0xfc0fffffu)|(psm<<20));rejects([&]{(void)parse_xim_layout(bad);});}
    const auto bad=navigation_override(authored(32,1,2));const OwnedRawXim unsupported(bad);
    require(xim_upload_support(unsupported.layout()).status==XimUploadStatus::UnsupportedPaletteFormat,"CPSM2 upload remains guarded");
    const auto identity=memory->identity();rejects([&]{(void)upload_xim(*memory,unsupported);});require(memory->identity()==identity,"unsupported upload prevalidation changes nothing");
    rejects([&]{(void)decode_xim(bad);});rejects([&]{(void)decode_xim_gs32(bad);});
}
static void owned_optional() {
    const char* path=std::getenv("OKAGE_OWNED_NAVI_FIELD_1D");if(!path)return;
    std::ifstream file(path,std::ios::binary);require(bool(file),"owned NAVI input open");const Data input(std::istreambuf_iterator<char>(file),{});
    require(sha256_hex(input)=="bca882d65bdfb86614bbb8cbf6dc65e30af2fdb38ff6d64ce3cdd7d032657fe8","owned FIELD_1D hash");
    const auto layout=parse_xim_layout(input);require(layout.pixel_format==20&&layout.width==56&&layout.height==112&&layout.palette_color_count==16,"owned FIELD_1D authored layout");
    const auto overridden=navigation_override(input);require(decode_xim(input).rgba==decode_xim(overridden).rgba,"owned override preserves decoded RGBA");
    load_and_check(overridden,false);std::cout<<"Owned FIELD_1D PSM44 load/6272 texels passed; override SHA256="<<sha256_hex(overridden)<<'\n';
}
int main(){try{generated();owned_optional();std::cout<<"XIM44 source override, packed decoding, high-lane ownership, palette and floor-QWC tests passed\n";}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
