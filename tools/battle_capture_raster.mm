#include "../src/battle_capture_metal.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc!=3)throw std::runtime_error("Usage: okage-battle-capture-raster FIXTURE NEW_RGBA_OUTPUT");
        constexpr std::size_t imageSize=640*256*4,stride=32+3*imageSize;
        const auto size=std::filesystem::file_size(argv[1]);
        if(!size||size%stride||size>16*stride)throw std::runtime_error("Capture raster fixture extent");
        if(std::filesystem::exists(argv[2])||std::filesystem::is_symlink(argv[2]))throw std::runtime_error("Capture raster output must be fresh");
        std::ifstream in(argv[1],std::ios::binary);std::ofstream out(argv[2],std::ios::binary);
        auto device=MTLCreateSystemDefaultDevice();auto queue=[device newCommandQueue];okage::BattleCaptureMetal renderer(device);
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:256 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        auto capture=[device newTextureWithDescriptor:descriptor],history=[device newTextureWithDescriptor:descriptor];
        descriptor.height=224;descriptor.usage=MTLTextureUsageRenderTarget;
        auto target=[device newTextureWithDescriptor:descriptor];
        const auto sine=okage::make_sine_table();std::vector<std::uint8_t> pixels(imageSize);
        for(std::size_t at=0;at<size;at+=stride) {
            std::array<std::uint32_t,8> row;
            if(!in.read(reinterpret_cast<char*>(row.data()),32))throw std::runtime_error("Short capture raster header");
            if(row[3]>1||row[4]||row[5]||row[6]||row[7])throw std::runtime_error("Capture raster fixture header");
            for(auto texture:{target,history,capture}) {
                if(!in.read(reinterpret_cast<char*>(pixels.data()),pixels.size()))throw std::runtime_error("Short capture raster image");
                [texture replaceRegion:MTLRegionMake2D(0,0,640,texture.height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:640*4];
            }
            okage::BattleCaptureLayer layer{row[0],row[1],row[2]};const auto draw=layer.draw(row[3],sine);
            auto command=[queue commandBuffer];renderer.encode(command,target,capture,history,draw);
            [command commit];[command waitUntilCompleted];
            if(command.status!=MTLCommandBufferStatusCompleted)throw std::runtime_error(command.error.localizedDescription.UTF8String);
            [target getBytes:pixels.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];
            if(!out.write(reinterpret_cast<const char*>(pixels.data()),640*224*4))throw std::runtime_error("Capture raster output failed");
        }
        std::cout<<size/stride<<" native capture raster cases\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
}
