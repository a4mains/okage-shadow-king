// Explicit synthetic raster fixtures, not game state or reference output.
#include "../src/shadow_metal.hpp"
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc!=3)throw std::runtime_error("Usage: okage-shadow-raster FIXTURE NEW_RGBA_OUTPUT");
        const auto size=std::filesystem::file_size(argv[1]);constexpr unsigned stride=58*4;
        if(!size||size%stride||size>stride*48)throw std::runtime_error("Shadow raster fixture extent");
        if(std::filesystem::exists(argv[2]))throw std::runtime_error("Shadow raster output must be new");
        std::ifstream in(argv[1],std::ios::binary);std::ofstream out(argv[2],std::ios::binary);
        auto device=MTLCreateSystemDefaultDevice();auto queue=[device newCommandQueue];okage::ShadowMetal renderer(device);
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
        d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
        auto target=[device newTextureWithDescriptor:d];
        d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;
        auto depth=[device newTextureWithDescriptor:d];
        for(unsigned at=0;at<size;at+=stride) {
            std::array<std::uint32_t,58> f;if(!in.read(reinterpret_cast<char*>(f.data()),stride))throw std::runtime_error("Short shadow raster fixture");
            if(f[55]>0xffffff||!f[57]||f[57]>8)throw std::runtime_error("Shadow raster depth/repeat range");
            okage::ShadowPrismInput input;input.clip_status=__builtin_bit_cast(std::int32_t, (f[1]));
            std::memcpy(input.direction.data(),f.data()+2,16);std::memcpy(input.camera.data(),f.data()+6,96);
            std::memcpy(input.screen.data(),f.data()+30,96);
            for(unsigned i=0;i<6;++i){input.clip[i]=f[9+i*4];input.camera[i][3]=0;}
            const auto prism=okage::shadow_prism(input);
            std::vector<okage::ShadowRasterDraw> draws(f[57],{prism,{f[56]&255,(f[56]>>8)&255,(f[56]>>16)&255}});
            const okage::ShadowRasterMapping mapping;
            std::vector<std::uint8_t> raw;
            for(unsigned opaque=0;opaque<2;++opaque) {
            auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
            pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            pass.colorAttachments[0].clearColor=MTLClearColorMake(float(f[54]&255)/255,float((f[54]>>8)&255)/255,float((f[54]>>16)&255)/255,1);
            pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;
            pass.depthAttachment.storeAction=MTLStoreActionStore;
            pass.depthAttachment.clearDepth=float(f[55])*mapping.depth_scale+mapping.depth_offset;
            auto command=[queue commandBuffer];auto encoder=[command renderCommandEncoderWithDescriptor:pass];[encoder endEncoding];
            renderer.encode(command,target,depth,draws,mapping,opaque);
            auto depthReadback=[device newBufferWithLength:640*224*4 options:MTLResourceStorageModeShared];
            auto blit=[command blitCommandEncoder];
            [blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(640,224,1) toBuffer:depthReadback destinationOffset:0
                destinationBytesPerRow:640*4 destinationBytesPerImage:640*224*4];
            [blit endEncoding];
            [command commit];[command waitUntilCompleted];
            if(command.status!=MTLCommandBufferStatusCompleted)throw std::runtime_error(command.error.localizedDescription.UTF8String);
            std::vector<std::uint8_t> pixels(640*224*4);
            [target getBytes:pixels.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];
            const auto* z=static_cast<const float*>(depthReadback.contents);
            for(unsigned i=0;i<640*224;++i)if(z[i]!=float(pass.depthAttachment.clearDepth))
                throw std::runtime_error("Shadow raster changed receiving depth");
            if(!opaque)raw=std::move(pixels);
            else for(unsigned i=0;i<pixels.size();++i)if(pixels[i]!=(i%4==3?255:raw[i]))
                throw std::runtime_error("Opaque alpha restoration changed shadow RGB");
            }
            if(!out.write(reinterpret_cast<const char*>(raw.data()),raw.size()))throw std::runtime_error("Shadow raster output failed");
        }
        std::cout<<size/stride<<" raster cases verified unchanged depth and opaque alpha restoration\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
