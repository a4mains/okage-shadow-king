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
        if(argc!=5)throw std::runtime_error("Usage: okage-shadow-preview STATIC_XMP EFD TEXTURE_DIRECTORY NEW_OUTPUT_DIRECTORY");
        const fs::path output=argv[4];
        if(fs::exists(output)||!fs::create_directories(output))throw std::runtime_error("Preview needs a fresh output directory");
        okage::SceneRenderer renderer(argv[1]);renderer.load_effect_textures(argv[3]);
        const auto bank=okage::read_efd(read(argv[2]));
        const auto found=std::find_if(bank.begin(),bank.end(),[](const auto& d){return d.id==24;});
        if(found==bank.end())throw std::runtime_error("Owned effect24 definition missing");
        okage::GameRandom rng{1};const auto random=[&]{return rng.next();};
        const auto geometry=std::make_shared<const std::vector<okage::MapEffectVertex>>(
            okage::make_shadow_effect_shape(55,okage::make_sine_table(),random));
        okage::MapEffects effects;
        effects[0]=std::make_unique<okage::MapEffect>(*found,okage::Float4{-7.57f,0,-2.5f,1},
            okage::Float4{0,.75f,0,1},okage::Float4{1,1,1,1},renderer.effect_texture_sizes(),
            random,okage::EffectStart::FirstFrame);
        effects[0]->shape55_geometry=geometry;
        renderer.bind_map_effects(&effects);
        const auto view=okage::camera_matrix({-7,4,2.5f,1},{0,-.75f,0,0},{1,1,1,0},okage::make_sine_table());
        const okage::Float4 eye{7,-4,-2.5f,1},camera{0,.75f,0,1},projection{1.2f,1.2f*640/224,1,-2};
        for(unsigned tick=1;tick<=81;++tick) {
            for(auto& effect:effects)if(effect)effect->tick(random,true,&eye);
            if(tick!=1&&tick!=9&&tick!=17&&tick!=25&&tick!=41&&tick!=65&&tick!=81)continue;
            const auto image=renderer.render(view,projection,nullptr,camera,450,eye);
            auto* bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:image.width pixelsHigh:image.height
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:image.width*4 bitsPerPixel:32];
            if(!bitmap)throw std::runtime_error("Preview bitmap allocation failed");
            std::memcpy(bitmap.bitmapData,image.rgba.data(),image.rgba.size());
            const auto path=output/("shadow-"+std::to_string(tick)+".png");
            NSData* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if(!png||![png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES])
                throw std::runtime_error("Preview PNG write failed");
        }
        std::cout<<"Rendered seven isolated effect24 samples through the native SceneRenderer; not an original raster comparison\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
