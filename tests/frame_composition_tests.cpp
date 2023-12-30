#include "okage/frame_composition.hpp"
#include <array>
#include <climits>
#include <iostream>
#include <limits>
#include <memory>
#include <source_location>
#include <stdexcept>
using namespace okage;
namespace {
void require(bool value,std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("Frame composition test line "+std::to_string(at.line()));
}
template<class F> void rejects(F f) {try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected rejection");}
Texture image(unsigned height,std::array<unsigned char,4> color) {
    Texture t{640,height,0,0,std::vector<std::uint8_t>(640*height*4)};
    for(std::size_t i=0;i<t.rgba.size();++i)t.rgba[i]=color[i%4];return t;
}
std::array<unsigned char,4> pixel(const Texture& t,unsigned x,unsigned y) {
    const auto i=(y*640+x)*4;return {t.rgba[i],t.rgba[i+1],t.rgba[i+2],t.rgba[i+3]};
}
FrameComposition base() {
    FrameComposition c;c.frame=17;c.retain_game=true;
    c.dialogue=image(224,{0,0,0,0});c.narration=image(224,{0,0,0,0});return c;
}
void order_and_pixels() {
    auto c=base();c.live_enabled=true;c.fade.immediate(.5f);c.choice_cursor=0;
    c.narration=image(224,{200,100,0,128});
    unsigned calls=0;
    c.capture_pass=[&calls](Texture& t) {
        ++calls;
        // Fade: (100,80,60)->(50,40,30), alpha64. Narration then rounds /255.
        require(pixel(t,0,0)==std::array<unsigned char,4>{125,70,15,64});
        // Highlight before narration: fade red50 ->126; overlay red ->163.
        require(pixel(t,162,168)==std::array<unsigned char,4>{163,110,57,64});
        t.rgba[0]=77;t.rgba[3]=9;
    };
    auto out=c.compose(image(224,{100,80,60,31}));
    require(calls==1&&out.frame==17&&out.retain_game);
    require(pixel(out.diagnostic,0,0)==std::array<unsigned char,4>{77,70,15,9});
    require(pixel(out.diagnostic,0,224)==std::array<unsigned char,4>{16,16,16,255});
    require(out.live&&pixel(*out.live,0,0)==std::array<unsigned char,4>{77,70,15,255});
    require(pixel(*out.live,0,1)==pixel(*out.live,0,0));
    require(c.compose(image(224,{100,80,60,31})).diagnostic.rgba==out.diagnostic.rgba&&calls==2);
}
void owning_snapshot() {
    std::shared_ptr<const FrameComposition> snapshot;
    {
        auto c=base();c.live_enabled=true;c.dialogue_alpha=.5f;
        c.dialogue=image(224,{200,80,40,255});c.live_dialogue=image(448,{200,80,40,255});
        c.transition_alpha=64;c.transition_diagnostic=image(448,{20,40,60,0});
        c.transition_live=image(448,{20,40,60,0});
        c.fade.color=0x102030;c.fade.immediate(.5f); // Published color survives later writes.
        const auto owned=image(224,{100,100,100,13});
        c.capture_pass=[owned](Texture& t){t=owned;};
        snapshot=std::make_shared<const FrameComposition>(c);
        c.dialogue.rgba.assign(c.dialogue.rgba.size(),0);c.fade.immediate(0);
        c.transition_diagnostic->rgba.clear();c.capture_pass={};
    }
    const auto a=snapshot->compose(image(224,{0,0,0,0}));
    const auto b=snapshot->compose(image(224,{255,255,255,255}));
    require(a.diagnostic.rgba==b.diagnostic.rgba&&a.live->rgba==b.live->rgba);
    require(pixel(a.diagnostic,0,0)==std::array<unsigned char,4>{60,70,80,13});
    require(pixel(a.diagnostic,0,224)==std::array<unsigned char,4>{64,44,44,255});
    // Live body: background68.5; half-ink (200,80,40) -> (134,74,54), then transition.
    require(pixel(*a.live,25,332)==std::array<unsigned char,4>{77,57,57,255});
    require(pixel(*a.live,24,332)==std::array<unsigned char,4>{60,70,80,255});
    require(pixel(*a.live,25,431)==std::array<unsigned char,4>{60,70,80,255});
    auto c=base();c.fade.color=0x204060;c.fade.immediate(.5f);
    const auto frozen=c;c.fade.color=0xffffff;c.fade.immediate(0);
    require(pixel(frozen.compose(image(224,{100,100,100,5})).diagnostic,0,0)==std::array<unsigned char,4>{98,82,66,64});
}
void boundaries() {
    auto c=base();auto scene=image(224,{0,0,0,7});
    c.choice_cursor=3;auto out=c.compose(scene);
    require(pixel(out.diagnostic,162,208)[0]==95&&pixel(out.diagnostic,162,222)[0]==0);
    for(int cursor:{-1,4,INT_MAX,INT_MIN}) {c.choice_cursor=cursor;rejects([&]{c.compose(scene);});}
    c=base();c.dialogue_alpha=std::numeric_limits<float>::quiet_NaN();rejects([&]{c.compose(scene);});
    for(float alpha:{-.1f,1.1f}) {c.dialogue_alpha=alpha;rejects([&]{c.compose(scene);});}
    c=base();c.transition_alpha=129;rejects([&]{c.compose(scene);});
    c.transition_alpha=1;rejects([&]{c.compose(scene);});
    c=base();c.live_enabled=true;c.dialogue_alpha=.1f;rejects([&]{c.compose(scene);});
    c.live_dialogue=image(224,{0,0,0,0});rejects([&]{c.compose(scene);});
    c=base();auto bad=scene;bad.width=639;rejects([&]{c.compose(bad);});
    c.narration.rgba.pop_back();rejects([&]{c.compose(scene);});
    c=base();c.capture_pass=[](Texture& t){t.rgba.pop_back();};rejects([&]{c.compose(scene);});
    c=base();c.live_enabled=true;c.transition_alpha=64;c.transition_diagnostic=image(448,{0,0,0,0});rejects([&]{c.compose(scene);});
    c.transition_live=image(224,{0,0,0,0});rejects([&]{c.compose(scene);});
    c=base();c.apply_fade=false;c.fade.immediate(0);
    require(pixel(c.compose(scene).diagnostic,0,0)[3]==7);
    // Legacy text-only ignores every unused scene field and never calls capture.
    c.game_view=false;c.live_enabled=true;c.choice_cursor=INT_MAX;c.transition_alpha=999;
    c.dialogue=image(224,{1,2,3,4});c.narration={};c.capture_pass=[](Texture&){throw std::runtime_error("must not call");};
    out=c.compose({});require(!out.live&&out.diagnostic.rgba==c.dialogue.rgba&&out.diagnostic.height==224);
    c.dialogue.rgba.pop_back();rejects([&]{c.compose({});});
}
}
int main(){try{order_and_pixels();owning_snapshot();boundaries();std::cout<<"Frame composition tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
