// Offscreen owned-asset component exercise. Not a gameplay or Session entry.
#import <AppKit/AppKit.h>
#include "../src/ui_metal.hpp"
#include "okage/battle_interface.hpp"
#include "okage/ui_layout.hpp"
#include "okage/font.hpp"
#include "okage/enemy.hpp"
#include "okage/ui_draw_diagnostic.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace okage;
namespace fs=std::filesystem;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::vector<std::uint8_t> read(const fs::path& path){
    std::ifstream in(path,std::ios::binary);require(bool(in),"Owned UI input missing");
    return {std::istreambuf_iterator<char>(in),{}};
}
void fresh(const fs::path& p){require(!fs::exists(p)&&!fs::is_symlink(p),"Owned UI outputs must be fresh");}
void bytes(const fs::path& p,const void* data,std::size_t n){
    fresh(p);std::ofstream out(p,std::ios::binary);
    require(bool(out.write(static_cast<const char*>(data),n)),"Owned UI output write failed");
}
}
int main(int argc,const char** argv){@autoreleasepool {try {
    require(argc==5||argc==6,"Usage: okage-ui-owned-draw BATTLE_DIRECTORY FONT_DIRECTORY WINDOW_XIM NEW_PREFIX [--opaque-fixture]");
    const bool opaque=argc==6;require(!opaque||std::string(argv[5])=="--opaque-fixture","Unknown owned UI fixture option");
    const fs::path battle=argv[1],font_directory=argv[2];const std::string prefix=argv[4];
    for(const auto* suffix:{".rgba",".depth.bin",".png",".json",".draw.json"})fresh(prefix+suffix);
    auto resource=std::make_shared<RuwResource>(read(battle/"BTL.RUW"));
    const BattleUiData definitions(read(battle/"battle-ui.bin"));
    auto fonts=std::make_shared<std::vector<Font>>();
    for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts->push_back(read_fnt(read(font_directory/name)));
    auto atlas_bytes=read(argv[3]);require(atlas_bytes.size()>=4,"Window XIM header missing");
    //18f500's actual owned window PSMT8H descriptor override.
    unsigned header=0;for(unsigned i=0;i<4;++i)header|=unsigned(atlas_bytes[i])<<(8*i);
    header=(header&0xfc0fffff)|(0x1bu<<20);for(unsigned i=0;i<4;++i)atlas_bytes[i]=header>>(8*i);
    auto atlas=std::make_shared<Texture>(decode_xim(atlas_bytes));
    BattleUiShared shared;BattleInterface ui(shared,definitions,atlas,0,[](unsigned){});
    BattleUiPad pad;ui.first_update(resource,pad,{},60);
    const auto sine=make_sine_table();UiLayoutShared layout;UiPacketBuildContext packets{fonts};GameRandom random{1};
    std::array<unsigned,2> glyphs{},draws{};
    for(unsigned i=0;i<2;++i){auto& window=*shared.registry[ui.handles()[i]];
        window.advance_clock();
        if(opaque)window.set_opacity_tween(0,128,0,128);
        window.prepare_mode2({layout,*fonts,sine,[&]{++draws[i];return random.next();},0,packets});
        glyphs[i]=window.word(0xcdc);
    }
    UiDrawState draw_state;const auto list=build_ui_draw_list(shared.registry,draw_state,initial_ui_projection(),sine);
    {
        std::ofstream report(prefix+".draw.json");
        write_ui_draw_diagnostic(report,list,0,0,0,opaque?"opaque-owned-constructor-fixture":"owned-constructor-fixture");
        require(bool(report),"Owned UI draw diagnostic failed");
    }
    const auto geometry=resolve_ui_geometry(list);
    auto device=MTLCreateSystemDefaultDevice();auto queue=[device newCommandQueue];require(device&&queue,"Owned UI Metal unavailable");
    UiMetal renderer(device);
    std::array<id<MTLTexture>,3> font_textures;
    for(unsigned i=0;i<3;++i)font_textures[i]=upload_ui_gs_texture(device,fonts->at(i).atlas);
    auto window_texture=upload_ui_gs_texture(device,*atlas);
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
    auto target=[device newTextureWithDescriptor:descriptor];
    descriptor.pixelFormat=MTLPixelFormatDepth32Float;descriptor.storageMode=MTLStorageModePrivate;
    auto depth=[device newTextureWithDescriptor:descriptor];require(target&&depth,"Owned UI target allocation failed");
    auto command=[queue commandBuffer];auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
    pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;
    pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1;
    [[command renderCommandEncoderWithDescriptor:pass] endEncoding];
    const UiMetalBaseline baseline{0x6c00,0x7900,true,0,1,0,1,0xdf0000027f0000ull};
    const auto validation=renderer.encode(command,target,depth,geometry,baseline,[&](const UiGeometryTexture& texture){
        if(texture.kind==UiGeometryTexture::Kind::Font){
            require(texture.fonts==fonts&&texture.font_index<3,"Owned UI font binding mismatch");
            return UiMetalTexture{font_textures[texture.font_index],fonts};
        }
        require(texture.window==atlas,"Owned UI window binding mismatch");
        return UiMetalTexture{window_texture,atlas};
    });
    auto depth_readback=[device newBufferWithLength:640*224*4 options:MTLResourceStorageModeShared];
    require(bool(depth_readback),"Owned UI depth readback allocation failed");
    auto blit=[command blitCommandEncoder];
    [blit copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(640,224,1) toBuffer:depth_readback destinationOffset:0
        destinationBytesPerRow:640*4 destinationBytesPerImage:640*224*4];[blit endEncoding];
    [command commit];[command waitUntilCompleted];
    require(command.status==MTLCommandBufferStatusCompleted,"Owned UI Metal command failed");
    validation.validate_completed(command);
    std::vector<std::uint8_t> rgba(640*224*4);
    [target getBytes:rgba.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];
    bytes(prefix+".rgba",rgba.data(),rgba.size());bytes(prefix+".depth.bin",depth_readback.contents,640*224*4);
    unsigned colored=0,depth_written=0,primitives=0;
    for(unsigned i=0;i<640*224;++i){colored+=rgba[i*4]||rgba[i*4+1]||rgba[i*4+2];
        depth_written+=static_cast<const float*>(depth_readback.contents)[i]!=1.f;rgba[i*4+3]=255;}
    for(const auto& c:geometry.commands)primitives+=std::holds_alternative<UiResolvedPrimitive>(c);
    // PNG is presentation only; raw RGBA above preserves actual GS-byte alpha.
    auto bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:640 pixelsHigh:224
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:640*4 bitsPerPixel:32];
    require(bool(bitmap),"Owned UI bitmap allocation failed");std::copy(rgba.begin(),rgba.end(),bitmap.bitmapData);
    auto png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];require(bool(png),"Owned UI PNG encoding failed");
    bytes(prefix+".png",png.bytes,png.length);
    std::ofstream report(prefix+".json");report<<"{\n  \"schema\":\"okage-owned-ui-component-v1\",\n  \"scope\":\"owned constructor text through native layout, draw, projection and Metal; explicit synthetic startup fixture, no active battle HUD or Session scheduling\",\n"
        <<"  \"opaque_fixture\":"<<(opaque?"true":"false")<<",\n  \"rng_seed\":1,\n  \"glyphs\":["<<glyphs[0]<<','<<glyphs[1]<<"],\n  \"rng_draws\":["<<draws[0]<<','<<draws[1]<<"],\n"
        <<"  \"draw_commands\":"<<list.commands().size()<<",\n  \"projected_quads\":"<<primitives<<",\n  \"colored_pixels\":"<<colored<<",\n  \"written_depth_pixels\":"<<depth_written<<"\n}\n";
    require(bool(report),"Owned UI report failed");std::cout<<primitives<<" owned UI quads; "<<colored<<" colored pixels; "<<depth_written<<" written depth pixels\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}return 0;}}
