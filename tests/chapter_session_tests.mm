#include "chapter_session.hpp"
#include <bit>
#include <iostream>
#include <source_location>
#include <stdexcept>
using namespace okage;
namespace {
void check(bool b,const std::source_location at=std::source_location::current()) {
    if(!b)throw std::runtime_error("Chapter session assertion line "+std::to_string(at.line()));
}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned value){for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(8*i);}
std::vector<std::uint8_t> image_bytes(){
    constexpr unsigned block=0x420,payload=0x430;
    std::vector<std::uint8_t> b(payload+512*192,7);
    std::fill_n(b.begin(),payload,0);put(b,0,19<<20);put(b,16,0x410);put(b,24,1);put(b,28,256);
    put(b,block+8,192);put(b,block+12,512);put(b,32+7*4,0xff785028);return b;
}
}
int main(){@autoreleasepool{try{
    for(unsigned chapter_id:{1u,2u,7u}) {
    const GameModeSelection destination{2,chapter_id==1?72u:1u,0};
    SceneClearColor clear;SceneFade fade;BattleUiShared ui;
    std::vector<std::uint8_t> variables(0x1000,0xa5);std::array<std::uint32_t,3> command{14,chapter_id,0};
    const auto bytes=image_bytes();unsigned loaded=0;
    ChapterSession chapter(variables,command,destination,clear,fade,ui,[&](unsigned id){
        check(id==chapter_id&&fade.current==0&&fade.target==0);++loaded;return bytes;
    });
    check(loaded==1&&chapter.resource_live()&&!chapter.rendered_image());
    for(unsigned i=0;i<variables.size();++i)check(variables[i]==(i>=0x10&&i<0x110?0:0xa5));
    check(command==std::array<std::uint32_t,3>{14,chapter_id,0}&&fade.current==0&&fade.target==1);
    Animation camera(3);reset_scene_camera(camera);FieldProjection projection;GameRandom random{1234};
    const auto sine=make_sine_table();GameModeParent parent(destination);unsigned ticks=0,destroys=0,frees=0,constructs=0;
    std::uint32_t image_frames=42;
    auto tick=[&]{
        fade.tick();
        parent.tick([&]{chapter.retire(camera,projection,nullptr,ui,fade);++destroys;},[&]{++frees;},
            [&](auto selection){check(selection==destination);++constructs;});
        if(chapter.resource_live()){
            chapter.tick(command,fade,camera,projection,nullptr,ui,{},sine,random,image_frames,
                [&](auto selection){check(command==std::array<std::uint32_t,3>{});parent.request(selection);});++ticks;
        }
    };
    tick();check(chapter.state().counter==1&&image_frames==43);
    const auto* image=chapter.rendered_image();check(image&&image->width==640&&image->height==224);
    for(unsigned y=0;y<224;++y)for(unsigned x=0;x<640;++x){
        const auto at=(y*640+x)*4;const bool inside=x>=64&&x<576&&y>=64&&y<160;
        check(image->rgba[at]==(inside?79:0)&&image->rgba[at+1]==(inside?159:0)&&
            image->rgba[at+2]==(inside?239:0)&&image->rgba[at+3]==(inside?255:64));
    }
    for(unsigned i=1;i<239;++i)tick();
    check(chapter.state().counter==239&&fade.current==1&&fade.target==1&&!parent.requested());
    tick();check(chapter.state().counter==240&&fade.target==0&&fade.current==1&&!parent.requested());
    while(!parent.requested()&&ticks<400)tick();
    check(parent.requested()==destination&&fade.current==0&&command[0]==0);
    check(random.state==1234&&image_frames==42+ticks);const auto active_ticks=ticks;
    tick();check(destroys==1&&!chapter.resource_live()&&!chapter.rendered_image()&&ticks==active_ticks);
    check(std::bit_cast<std::uint32_t>(fade.step)==0x3ca3d70a);
    tick();check(frees==1&&constructs==0);tick();check(constructs==1);
    }
    std::cout<<"Chapter resource, raw image, fade and parent handoff tests passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
