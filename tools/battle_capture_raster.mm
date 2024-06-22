#include "../src/battle_capture_metal.hpp"
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void require_frozen(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
void frozen_tests() {
    okage::BattleCaptureSurface surface;
    auto image=[](std::array<std::uint8_t,4> rgba) {
        okage::Texture out{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
        for(std::size_t at=0;at<out.rgba.size();at+=4)std::copy(rgba.begin(),rgba.end(),out.rgba.begin()+at);
        return out;
    };
    auto capture=image({37,91,203,7});const auto unchanged=capture;
    for(bool field:{true,false}) {
        auto destination=image({255,1,79,239});surface.apply_frozen(destination,capture,field);
        for(std::size_t at=0;at<destination.rgba.size();at+=4)
            require_frozen(destination.rgba[at]==37&&destination.rgba[at+1]==91&&
                destination.rgba[at+2]==203&&destination.rgba[at+3]==128,
                "Frozen constant surface/FIX/TCC0 mismatch");
    }
    require_frozen(capture.rgba==unchanged.rgba,"Frozen capture mutated its source");
    for(unsigned y=0;y<224;++y)for(unsigned x=0;x<640;++x) {
        const auto at=(y*640+x)*4;
        capture.rgba[at]=y>=64?128:0;
        capture.rgba[at+1]=x>=320?128:0;
        capture.rgba[at+2]=y==0?255:0;
        capture.rgba[at+3]=std::uint8_t(x+y);
    }
    for(bool field:{true,false}) {
        auto destination=image({8,9,10,11});surface.apply_frozen(destination,capture,field);
        auto pixel=[&](unsigned x,unsigned y,unsigned c){return destination.rgba[(y*640+x)*4+c];};
        require_frozen(pixel(200,63,0)==0&&pixel(200,64,0)==(field?29:13)&&
            pixel(200,65,0)==(field?120:72)&&pixel(200,66,0)==128,
            "Frozen ordered vertical edge sampling mismatch");
        require_frozen(pixel(319,100,1)==0&&pixel(320,100,1)==63&&pixel(321,100,1)==128,
            "Frozen horizontal endpoint interpolation mismatch");
        require_frozen(pixel(200,0,2)==(field?223:255),"Frozen half-field top-row coverage mismatch");
        for(std::size_t at=3;at<destination.rgba.size();at+=4)
            require_frozen(destination.rgba[at]==128,"Frozen source texture alpha leaked through TCC0");
        auto again=image({211,221,231,241});surface.apply_frozen(again,capture,field);
        require_frozen(again.rgba==destination.rgba,"Frozen result depends on old destination");
    }
    auto invalid=capture;invalid.rgba.pop_back();auto destination=image({1,2,3,4});const auto old=destination;
    bool rejected=false;try{surface.apply_frozen(destination,invalid,true);}catch(const std::exception&){rejected=true;}
    require_frozen(rejected&&destination.rgba==old.rgba,"Frozen malformed input mutation/acceptance");
    invalid=capture;invalid.width=639;rejected=false;
    try{surface.apply_frozen(destination,invalid,true);}catch(const std::exception&){rejected=true;}
    require_frozen(rejected&&destination.rgba==old.rgba,"Frozen invalid extent mutation/acceptance");
    std::cout<<"Frozen capture constant/edges/alpha/coverage/ownership/bounds passed\n";
}
}

int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc==2&&std::strcmp(argv[1],"--self-test-frozen")==0){frozen_tests();return 0;}
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
