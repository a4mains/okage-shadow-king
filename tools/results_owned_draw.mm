// Offscreen owned-asset component exercise. Not a gameplay or Session entry.
#import <AppKit/AppKit.h>
#include "../src/ui_metal.hpp"
#include "okage/results_interface.hpp"
#include "okage/ui_projection.hpp"
#include "okage/event.hpp"
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
    require(argc==8||argc==9,"Usage: okage-results-owned-draw BATTLE_DIR XDB_DIR FLD_RUW FONT_DIR WINDOW_XIM RESULTS_DIR NEW_PREFIX [--level-up-fixture]");
    const bool level_up=argc==9;
    require(!level_up||std::string(argv[8])=="--level-up-fixture","Unknown results fixture option");
    const fs::path battle=argv[1],xdb=argv[2],font_directory=argv[4],results=argv[6];
    const std::string prefix=argv[7];
    for(const auto* suffix:{".rgba",".depth.bin",".png",".json",".draw.json"})fresh(prefix+suffix);
    auto resource=std::make_shared<RuwResource>(read(argv[3]));
    const ResultsUiData definitions(read(results/"results-ui.bin"));
    const ResultsTextData text(read(results/"results-text.bin"));
    const BattleHudData numbers(read(battle/"battle-hud.bin"));
    const GameDataTables tables(read(xdb/"BKDATA.XDB"),read(xdb/"ITEM.XDB"));
    auto fonts=std::make_shared<std::vector<Font>>();
    for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts->push_back(read_fnt(read(font_directory/name)));
    auto atlas_bytes=read(argv[5]);require(atlas_bytes.size()>=4,"Window XIM header missing");
    unsigned header=0;for(unsigned i=0;i<4;++i)header|=unsigned(atlas_bytes[i])<<(8*i);
    header=(header&0xfc0fffff)|(0x1bu<<20);for(unsigned i=0;i<4;++i)atlas_bytes[i]=header>>(8*i);
    auto atlas=std::make_shared<Texture>(decode_xim(atlas_bytes));
    std::vector<std::uint8_t> variables(0xda0);initialize_new_game_variables(variables,tables);
    const std::string name="Ari";std::copy(name.begin(),name.end(),variables.begin()+0x844);
    std::array<std::uint8_t,56> reward{};
    auto put_half=[&](unsigned at,unsigned v){reward[at]=v;reward[at+1]=v>>8;};
    put_half(2,30);put_half(4,1);put_half(6,1);put_half(0x26,level_up?1000:7);
    UiPartySelection party;ResultsUiShared shared;UiResourceCoordinator coordinator;
    UiResourceCoordinator::Registry registry{};std::array<std::uint32_t,16> accumulators{};
    unsigned jingles=0;
    ResultsUiContext context{variables,reward,tables,party,numbers,text,[&](unsigned id){require(id==5,"Unexpected results jingle");++jingles;},2};
    ResultsInterface ui(registry,coordinator,accumulators,shared,definitions,resource,atlas,0,context);
    const auto sine=make_sine_table();GameRandom random{1};std::optional<UiDrawList> rendered;
    unsigned ticks=0;
    while(ticks<4000) {
        const unsigned pressed=shared.progress.notice==1&&shared.progress.notice_wait>360?0x40:0;
        ui.first_update(pressed,context);ui.second_update(context);++ticks;
        const std::array<UiResourceSlotBinding,1> binding{{{ui.handle(),0}}};
        rendered=coordinator.advance_resource(registry,binding,accumulators,fonts,sine,random,initial_ui_projection());
        if(ui.words()[0xe4c/4]==10)break;
    }
    require(ticks<4000&&rendered.has_value(),"Results fixture did not reach confirmation state");
    const auto& list=*rendered;
    {
        std::ofstream report(prefix+".draw.json");
        write_ui_draw_diagnostic(report,list,ticks,0,0,"owned-results-synthetic-reward-component");
        require(bool(report),"Results draw diagnostic failed");
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
    const UiMetalBaseline baseline{0x6c00,0x7900,true,0,1,0,1};
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
    std::ofstream report(prefix+".json");
    report<<"{\n  \"schema\":\"okage-owned-results-component-v1\",\n"
        <<"  \"scope\":\"synthetic reward after native new-game variable initialization; actual results controller, layout and Metal; not Session scheduling or gameplay proof\",\n"
        <<"  \"level_up_fixture\":"<<(level_up?"true":"false")<<",\n  \"ticks\":"<<ticks<<",\n  \"jingles\":"<<jingles
        <<",\n  \"state\":"<<ui.words()[0xe4c/4]<<",\n  \"rng_state\":"<<random.state
        <<",\n  \"draw_commands\":"<<list.commands().size()<<",\n  \"projected_quads\":"<<primitives
        <<",\n  \"colored_pixels\":"<<colored<<",\n  \"written_depth_pixels\":"<<depth_written<<"\n}\n";
    require(bool(report),"Results report failed");
    std::cout<<ticks<<" ticks; "<<primitives<<" results quads; "<<colored<<" colored pixels; "<<jingles<<" jingle requests\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}return 0;}}
