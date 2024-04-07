#import <AppKit/AppKit.h>
#include "../src/name_entry_session.hpp"
#include "okage/event.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
namespace {
namespace fs=std::filesystem;using namespace okage;
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
std::vector<std::uint8_t> read(const fs::path& p){std::ifstream in(p,std::ios::binary);check(bool(in),"Component asset missing");return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const void* data,std::size_t size){
    check(!fs::exists(p)&&!fs::is_symlink(p),"Component output must be fresh");
    std::ofstream out(p,std::ios::binary);check(bool(out.write(static_cast<const char*>(data),size)),"Component output write failed");
}
}
int main(int argc,const char** argv){@autoreleasepool {try {
    check(argc==7,"usage: okage-name-entry-component UI_ROOT COMMON_ROOT FONT_ROOT XDB_ROOT PAD_TAPE FRESH_PREFIX");
    const fs::path fonts_root=argv[3],xdb=argv[4];const std::string prefix=argv[6];
    for(const auto* suffix:{".state.bin",".rgba",".png",".json",".window.txt"})check(!fs::exists(prefix+suffix)&&!fs::is_symlink(prefix+suffix),"Component output exists");
    auto fonts=std::make_shared<std::vector<Font>>();
    for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts->push_back(read_fnt(read(fonts_root/name)));
    GameDataTables tables(read(xdb/"BKDATA.XDB"),read(xdb/"ITEM.XDB"));
    std::vector<std::uint8_t> variables(4096);initialize_game_variables(variables);
    SceneClearColor clear;SceneFade fade;BattleUiShared ui;NameEntryShared name;BattleUiPad pad;
    Animation camera(3);reset_scene_camera(camera);FieldProjection projection;
    GameRandom random{1};const auto sine=make_sine_table();unsigned image_counter=0,requests=0;
    NameEntrySession session(clear,fade,ui,name,{argv[2],argv[1],fonts,[]{}});
    std::ifstream input(argv[5]);check(bool(input),"Component pad tape missing");
    std::vector<std::uint32_t> states;std::vector<unsigned> sounds;std::string line;unsigned ticks=0;
    while(std::getline(input,line)) {
        if(line.empty()||line[0]=='#')continue;
        unsigned tick,held,pressed,released,repeat,x,y;std::string tail;std::istringstream row(line);
        check(bool(row>>tick>>held>>pressed>>released>>repeat>>x>>y)&&!(row>>tail),"Component pad row needs seven decimal values");
        check(tick==ticks+1&&tick<=4096&&held<=65535&&pressed<=65535&&released<=65535&&repeat<=65535&&x<=255&&y<=255,"Component pad bounds/order");
        check(!requests,"Component tape extends beyond the field request");
        fade.tick();session.tick(pad,{std::uint16_t(held),std::uint16_t(pressed),std::uint16_t(released),std::uint16_t(repeat),std::uint8_t(x),std::uint8_t(y)},
            variables,tables,fade,camera,projection,nullptr,ui,image_counter,sine,random,
            [&](unsigned id){sounds.push_back(id);},[&]{++requests;});
        ++ticks;states.push_back(ticks);const auto record=session.observation();states.insert(states.end(),record.begin(),record.end());
    }
    check(ticks>0&&session.rendered_image(),"Component tape produced no frames");
    std::ostringstream window_dump;
    for(unsigned slot=0;slot<ui.registry.size();++slot)if(ui.registry[slot]) {
        const auto& w=*ui.registry[slot];window_dump<<"window "<<slot<<'\n';
        for(unsigned at=0;at<UiWindow::scalar_bytes;at+=4)if(const auto v=w.known_word(at))
            window_dump<<"scalar "<<at<<' '<<*v<<'\n';
        for(unsigned side=0;side<2;++side)for(unsigned family=0;family<2;++family) {
            const auto& b=family?w.sprite_buffer(side):w.glyph_buffer(side);
            for(unsigned at=0;at<b.size();++at)if(const auto v=b.known_byte(at))
                window_dump<<(family?"sprite":"glyph")<<side<<' '<<at<<' '<<unsigned(*v)<<'\n';
        }
    }
    const auto dump=window_dump.str();write(prefix+".window.txt",dump.data(),dump.size());
    const auto& image=*session.rendered_image();
    write(prefix+".state.bin",states.data(),states.size()*4);write(prefix+".rgba",image.rgba.data(),image.rgba.size());
    auto bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image.width pixelsHigh:image.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:image.width*4 bitsPerPixel:32];
    check(bool(bitmap),"Component bitmap allocation");std::copy(image.rgba.begin(),image.rgba.end(),bitmap.bitmapData);
    for(std::size_t i=3;i<image.rgba.size();i+=4)bitmap.bitmapData[i]=255;
    auto png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];check(bool(png),"Component PNG encode");write(prefix+".png",png.bytes,png.length);
    std::ostringstream report;report<<"{\"schema\":\"okage-name-entry-component-v1\",\"scope\":\"Owned constructor and pad-driven callbacks; component tick labels, seed1, no full-session timing or audio claim\",\"ticks\":"<<ticks<<",\"record_words\":967,\"field_requests\":"<<requests<<",\"sound_calls\":"<<sounds.size()<<"}\n";
    const auto text=report.str();write(prefix+".json",text.data(),text.size());std::cout<<text;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
