#include "../src/battle_filter_metal.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
constexpr unsigned width=640,height=224,pixels=width*height,bytes=pixels*4;
struct Fixture {
    std::array<std::uint32_t,8> header{0x36464b4f,1,width,height,1,0x80000000,0,1};
    std::vector<std::uint8_t> rgba=std::vector<std::uint8_t>(bytes);
    std::vector<std::uint32_t> z=std::vector<std::uint32_t>(pixels);
};
Fixture run(const Fixture& input) {
    if(input.header[0]!=0x36464b4f||input.header[1]!=1||input.header[2]!=width||
       input.header[3]!=height||input.header[7]>1)throw std::runtime_error("Filter6 fixture header");
    auto device=MTLCreateSystemDefaultDevice();auto queue=[device newCommandQueue];
    okage::BattleFilterMetal renderer(device);
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:width height:height mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
    auto target=[device newTextureWithDescriptor:descriptor];
    descriptor.pixelFormat=MTLPixelFormatDepth32Float;descriptor.storageMode=MTLStorageModePrivate;
    descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    auto depth=[device newTextureWithDescriptor:descriptor];
    auto upload=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    auto readback=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if(!target||!depth||!upload||!readback||!queue)throw std::runtime_error("Filter6 fixture allocation");
    for(unsigned i=0;i<pixels;++i) {
        if(input.z[i]>0xffffff)throw std::runtime_error("Filter6 fixture Z24 overflow");
        static_cast<float*>(upload.contents)[i]=1.f-float(input.z[i])/16777216.f;
    }
    [target replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:input.rgba.data() bytesPerRow:width*4];
    auto command=[queue commandBuffer];auto blit=[command blitCommandEncoder];
    [blit copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:width*4 sourceBytesPerImage:bytes
        sourceSize:MTLSizeMake(width,height,1) toTexture:depth destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
    renderer.encode(command,target,depth,{input.header[4],input.header[5],
        __builtin_bit_cast(std::int32_t, (input.header[6])),bool(input.header[7])});
    blit=[command blitCommandEncoder];
    [blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toBuffer:readback destinationOffset:0
        destinationBytesPerRow:width*4 destinationBytesPerImage:bytes];[blit endEncoding];
    [command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)
        throw std::runtime_error(command.error.localizedDescription.UTF8String);
    Fixture output=input;
    [target getBytes:output.rgba.data() bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
    for(unsigned i=0;i<pixels;++i) {
        const float value=static_cast<const float*>(readback.contents)[i];
        if(__builtin_bit_cast(std::uint32_t, (value))!=__builtin_bit_cast(std::uint32_t, (static_cast<const float*>(upload.contents)[i])))
            throw std::runtime_error("Filter6 modified receiving depth");
        output.z[i]=std::uint32_t((1.f-value)*16777216.f);
    }
    return output;
}
void self_test() {
    Fixture input;input.header[6]=__builtin_bit_cast(std::uint32_t, (std::int32_t(-64)));
    const std::array<std::uint32_t,8> depths={0,0xff,0x100,0xbeff,0xbf00,0xffff,0x10000,0xffffff};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const auto i=y*width+x;input.z[i]=depths[(x/17+y/13)%depths.size()];
        input.rgba[4*i]=std::uint8_t(x*17+y*13);input.rgba[4*i+1]=std::uint8_t(x^y);
        input.rgba[4*i+2]=std::uint8_t(y*37);input.rgba[4*i+3]=std::uint8_t(i);
    }
    auto expected=input.rgba;
    for(unsigned i=0;i<pixels;++i) {
        const auto index=(input.z[i]>>8)&255;
        expected[4*i+3]=input.z[i]>0xffff||index>=191?0:std::uint8_t((191-index)/4);
    }
    for(const auto delta:std::array<std::array<int,2>,4>{{{-3,0},{2,0},{0,-1},{0,1}}}) {
        const auto previous=expected;
        for(int y=0;y<int(height);++y)for(int x=0;x<int(width);++x) {
            const int sx=x-delta[0],sy=y-delta[1];
            if(sx<0||sy<0||sx>=int(width)||sy>=int(height))continue;
            const auto dst=4*(y*width+x),src=4*(sy*width+sx);
            for(unsigned c=0;c<3;++c) {
                const int product=(int(previous[src+c])-previous[dst+c])*previous[dst+3];
                const int quotient=product>=0?product/128:-((-product+127)/128);
                expected[dst+c]=std::uint8_t(int(previous[dst+c])+quotient);
            }
        }
    }
    for(unsigned field=0;field<2;++field) {
        input.header[7]=field;const auto output=run(input);
        if(output.rgba!=expected||output.z!=input.z)throw std::runtime_error("Filter6 snapshot raster/depth mismatch");
    }
    std::cout<<"Filter6: two field branches, 286720 pixels and unchanged depth; native snapshot semantics only\n";
}
}
int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc==2&&std::string(argv[1])=="--self-test"){self_test();return 0;}
        if(argc!=3)throw std::runtime_error("Usage: okage-battle-filter-raster FIXTURE NEW_RGBA_Z24_OUTPUT | --self-test");
        if(std::filesystem::file_size(argv[1])!=32+2*bytes)throw std::runtime_error("Filter6 fixture extent");
        if(std::filesystem::exists(argv[2])||std::filesystem::is_symlink(argv[2]))throw std::runtime_error("Filter6 output must be fresh");
        Fixture input;std::ifstream in(argv[1],std::ios::binary);
        if(!in.read(reinterpret_cast<char*>(input.header.data()),32)||
           !in.read(reinterpret_cast<char*>(input.rgba.data()),bytes)||
           !in.read(reinterpret_cast<char*>(input.z.data()),bytes))throw std::runtime_error("Short filter6 fixture");
        const auto output=run(input);std::ofstream out(argv[2],std::ios::binary);
        if(!out.write(reinterpret_cast<const char*>(output.rgba.data()),bytes)||
           !out.write(reinterpret_cast<const char*>(output.z.data()),bytes))throw std::runtime_error("Filter6 output write");
        out.close();if(!out)throw std::runtime_error("Filter6 output close");
        std::cout<<"One native filter6 case, RGBA8 followed by Z24 words\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
}
