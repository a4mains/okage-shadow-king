#include "okage/chapter.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace okage;
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
template<class F> void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,"Expected explicit chapter error");}
void lifetime() {
    ChapterState state(7,72,3);std::vector<std::string> calls;bool pending=true;unsigned images=0;
    require(state.flag==1&&state.chapter_argument==7&&state.counter==0&&state.phase==19,"Constructor scalar writes");
    ChapterUpdateServices services{
        [&](const GameOverImageCall& image){
            require(state.counter==images,"Image observes pre-increment counter");++images;
            require(image.a0==0&&image.fp_words==std::array<std::uint32_t,6>{0,0,0x3f800000,0x3f800000,0,0x3f800000},"Actual chapter transform words");
            calls.push_back("image");
        },
        [&](float target){require(std::bit_cast<std::uint32_t>(target)==0&&state.counter==240,"Fade0 at exact240");calls.push_back("fade");},
        [&]{calls.push_back("poll");return pending;},
        [&]{calls.push_back("clear");},
        [&](GameModeSelection destination){require(destination==GameModeSelection{2,72,3},"Saved destination preserved");calls.push_back("request");}
    };
    for(unsigned i=0;i<239;++i)chapter_update(state,services);
    require(calls.size()==239&&state.counter==239,"No fade/readiness/input shortcut before240");calls.clear();
    chapter_update(state,services);require(calls==std::vector<std::string>{"image","fade"},"Exact240 never polls");calls.clear();
    chapter_update(state,services);require(calls==std::vector<std::string>{"image","poll"},"Unfinished fade waits after image");calls.clear();
    pending=false;chapter_update(state,services);
    require(calls==std::vector<std::string>{"image","poll","clear","request"},"Completion clears command before request");
    require(state.counter==242&&state.phase==19&&state.flag==1&&state.chapter_argument==7,"Update does not invent completion/phase mutation");
    calls.clear();chapter_update(state,services);
    require(calls==std::vector<std::string>{"image","poll","clear","request"},"Parent owns retirement; helper has no invented once-only latch");
}
void wrapping_and_failure_order() {
    ChapterState state(1,0x80000048u,0xffffffffu);unsigned images=0;
    ChapterUpdateServices services;services.submit_image=[&](const auto&){++images;};
    state.counter=0x7fffffffu;chapter_update(state,services);
    require(state.counter==0x80000000u&&images==1,"Wrapped negative count obeys signed threshold");
    state.counter=0xffffffffu;chapter_update(state,services);require(state.counter==0,"ADDIU wraps to zero");
    state.phase=20;rejects([&]{chapter_update(state,services);});require(images==2&&state.counter==0,"Unknown phase has no side effects");state.phase=19;
    services.submit_image={};rejects([&]{chapter_update(state,services);});require(state.counter==0,"Missing image fails before increment");
    services.submit_image=[](const auto&){throw std::runtime_error("GPU submission failure");};
    rejects([&]{chapter_update(state,services);});require(state.counter==0,"Image failure preserves source prefix");
    services.submit_image=[](const auto&){};state.counter=239;
    rejects([&]{chapter_update(state,services);});require(state.counter==240,"Missing fade leaves actual increment");
    rejects([&]{chapter_update(state,services);});require(state.counter==241,"Missing readiness fails only after next image/increment");
    services.fade_pending=[]{return false;};
    services.clear_command=[&]{state.destination_scene=9;};GameModeSelection actual{};
    services.request_mode=[&](auto destination){actual=destination;};
    chapter_update(state,services);require(actual==GameModeSelection{2,9,0xffffffffu},"Destination words read after command clear callback");
    bool cleared=false;services.clear_command=[&]{cleared=true;};services.request_mode={};
    rejects([&]{chapter_update(state,services);});require(cleared,"Missing parent request does not erase earlier command clear");
}
}
int main(){try{lifetime();wrapping_and_failure_order();std::cout<<"Chapter tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
