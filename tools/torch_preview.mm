#import <AppKit/AppKit.h>
#include "okage/scene_renderer.hpp"
#include "okage/enemy.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs=std::filesystem;
static std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size=fs::file_size(path);
    if(size>128*1024*1024)throw std::runtime_error("Preview input limit");
    std::vector<std::uint8_t> result(size);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(result.data()),size))throw std::runtime_error("Preview read failed");
    return result;
}
int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc!=5)throw std::runtime_error("Usage: okage-torch-preview STATIC_XMP EFD TEXTURE_DIRECTORY NEW_OUTPUT_DIRECTORY");
        const fs::path output=argv[4];
        if(fs::exists(output)||!fs::create_directories(output))throw std::runtime_error("Preview needs a fresh output directory");
        okage::SceneRenderer renderer(argv[1]);renderer.load_effect_textures(argv[3]);
        const auto bank=okage::read_efd(read(argv[2]));
        const auto found=std::find_if(bank.begin(),bank.end(),[](const auto& d){return d.id==91;});
        if(found==bank.end())throw std::runtime_error("Owned torch91 definition missing");
        okage::GameRandom rng{1};const auto random=[&]{return rng.next();};
        okage::MapEffects effects;
        for(unsigned i=0;i<3;++i)effects[i]=std::make_unique<okage::MapEffect>(*found,
            okage::Float4{float(i)*5-5,2,120,1},okage::Float4{},okage::Float4{1,1,1,1},
            renderer.effect_texture_sizes(),random);
        renderer.bind_map_effects(&effects);
        const okage::Matrix4 view{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,-100,1}}};
        const okage::Float4 eye{0,0,100,1},projection{1.2f,1.2f*640/224,1,-2};
        for(unsigned tick=1;tick<=61;++tick) {
            for(auto& effect:effects)if(effect)effect->tick(random,true,&eye);
            if(tick!=1&&tick!=7&&tick!=15&&tick!=31&&tick!=61)continue;
            const auto image=renderer.render(view,projection,nullptr,{},450,eye);
            auto* bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:image.width pixelsHigh:image.height
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:image.width*4 bitsPerPixel:32];
            if(!bitmap)throw std::runtime_error("Preview bitmap allocation failed");
            std::memcpy(bitmap.bitmapData,image.rgba.data(),image.rgba.size());
            const auto path=output/("torch-"+std::to_string(tick)+".png");
            NSData* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if(!png||![png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES])
                throw std::runtime_error("Preview PNG write failed");
        }
        std::cout<<"Rendered five isolated torch samples through the native SceneRenderer; not an original raster comparison\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
