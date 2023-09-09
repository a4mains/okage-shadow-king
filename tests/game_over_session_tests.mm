#include "game_over_session.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
using namespace okage;
namespace {
void check(bool b,unsigned at=__builtin_LINE()) {
    if(!b)throw std::runtime_error("Game-over session assertion line "+std::to_string(at));
}
template<class F>void rejects(F f){bool bad=false;try{f();}catch(const std::runtime_error&){bad=true;}check(bad);}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned value){for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(8*i);}
std::vector<std::uint8_t> image_bytes(){
    constexpr unsigned block=0x420,payload=0x430;
    std::vector<std::uint8_t> b(payload+640*448,7);
    std::fill_n(b.begin(),payload,0);put(b,0,19<<20);put(b,16,0x410);put(b,24,1);put(b,28,256);
    put(b,block+8,448);put(b,block+12,640);put(b,32+7*4,0x80785028);return b;
}
}
int main(){@autoreleasepool{try{
    SceneClearColor clear;SceneFade fade;BattleUiShared ui;GameOverShared shared;
    shared.mode_words.fill(42);shared.menu_word=31;shared.input.repeat_ticks=7;
    shared.image_frame_counter=42;
    ui.resource.draw_state().background_alpha_top=99;
    unsigned stopped=0,loaded=0;const auto bytes=image_bytes();
    GameOverSession game(clear,fade,ui,shared,{[&]{check(!loaded);++stopped;},[&]{check(stopped==1);++loaded;return bytes;}});
    check(stopped==1&&loaded==1&&game.resource_live()&&!game.rendered_image());
    check(shared.image_frame_counter==42);
    check(shared.mode_words==std::array<std::uint32_t,6>{0,9999,0,0,0,0}&&shared.menu_word==0);
    check(shared.input.repeat_ticks==7&&ui.resource.draw_state().background_alpha_top==99);
    check(fade.current==0&&fade.target==0&&fade.alpha()==128);
    Animation camera(3);reset_scene_camera(camera);FieldProjection projection;GameRandom random{1234};
    const auto sine=make_sine_table();GameModeParent parent({8,4,0});unsigned ticks=0,destroys=0,frees=0,constructs=0;
    auto tick=[&](unsigned pressed){
        parent.tick([&]{game.retire(camera,projection,nullptr,ui);++destroys;},[&]{++frees;},
            [&](auto selection){check(selection==GameModeSelection{8,1,0});++constructs;});
        if(parent.has_child()&&parent.current()[1]==4){
            fade.tick();game.tick({0,std::uint16_t(pressed),{128,128,128,128}},shared,fade,camera,projection,
                nullptr,ui,{},sine,random,[&](auto selection){parent.request(selection);});++ticks;
        }
    };
    tick(0);check(game.state().tick_counter==1&&game.state().mode_state==0&&fade.alpha()==128);
    check(shared.image_frame_counter==43);
    const auto* image=game.rendered_image();check(image&&image->width==640&&image->height==224);
    for(unsigned i=0;i<image->rgba.size();i+=4)
        check(image->rgba[i]==40&&image->rgba[i+1]==80&&image->rgba[i+2]==120&&image->rgba[i+3]==128);
    auto faded=*image;fade.apply(faded);
    for(unsigned i=0;i<faded.rgba.size();i+=4)
        check(faded.rgba[i]==0&&faded.rgba[i+1]==0&&faded.rgba[i+2]==0&&faded.rgba[i+3]==128);
    for(unsigned i=1;i<61;++i)tick(0);
    check(game.state().mode_state==10&&game.state().tick_counter==0&&fade.current==0);
    for(unsigned i=0;i<120;++i)tick(0x40);
    check(game.state().mode_state==10&&game.state().tick_counter==120&&!parent.requested());
    tick(0x40);check(game.state().mode_state==20&&fade.current>0&&fade.current<1);
    for(unsigned i=0;i<100&&!parent.requested();++i)tick(0);
    check(parent.requested()==GameModeSelection{8,1,0}&&game.resource_live()&&game.rendered_image());
    check(fade.current<=0&&random.state==1234);const auto active_ticks=ticks;
    tick(0);check(destroys==1&&!game.resource_live()&&!game.rendered_image()&&ticks==active_ticks);
    tick(0);check(frees==1&&constructs==0&&ticks==active_ticks);
    tick(0);check(constructs==1&&ticks==active_ticks);
    check(shared.image_frame_counter==42+active_ticks);
    rejects([&]{game.tick({0,0,{128,128,128,128}},shared,fade,camera,projection,nullptr,ui,{},sine,random,[](auto){});});
    auto bad=bytes;bad[32+7*4+3]=129;
    rejects([&]{GameOverSession invalid(clear,fade,ui,shared,{[]{},[&]{return bad;}});});
    check(ui.resource.draw_state().background_alpha_top==99);
    std::cout<<"Game-over task/resource/image integration tests passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}}
