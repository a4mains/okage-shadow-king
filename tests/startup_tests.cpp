#include "okage/startup.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using namespace okage;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,"Expected explicit startup error");}
GameOverInputState input(){return {0,{0x1000,0x2000,0x4000,0x8000},{},{}};}
constexpr GameOverPadSample pad{0,0x40,{128,128,128,128}};
void timeline() {
    StartupTaskState state{{0,0,0,0,0,0,0xdeadbeef},99};auto in=input();
    unsigned frame=0,images=0,initializations=0,requests=0;std::vector<std::string> order;
    StartupUpdateServices services{{
        [&](float alpha){
            require(state.common.tick_counter==frame-1,"Prefix fade before logo counter");
            if(frame==1)require(alpha==0,"First frame uses old rate before logo sets+.025");
            if(frame==2)require(std::bit_cast<std::uint32_t>(alpha)==0x3ccccccd,"Second prefix uses authored positive rate");
            if(frame==180||frame==480)require(alpha==1,"Fadeout boundary consumes old positive rate first");
            if(frame==270||frame==600)require(alpha==0,"Completed negative interval clamps alpha0");
            order.push_back("fade");
        },
        [&](GameModeSelection selected){++requests;require(frame==600&&selected==GameModeSelection{8,1,0},"Actual title request");order.push_back("request");},
        [&](const GameOverImageCall& image){++images;
            require(image.a0==(frame<270?0u:1u),"Correct authored logo selected after service");
            require(image.fp_words==std::array<std::uint32_t,6>{0,0,0x3f800000,0x3f800000,0,0x3f800000},"Original image transform words");order.push_back("image");}
        },[&]{++initializations;require(frame==270&&state.common.tick_counter==270,"Shared service at exact270");
            require(state.image_index==0&&state.common.generic_alpha_rate_bits==0xbccccccd,"Shared service before image/rate mutation");order.push_back("initialize");}};
    const auto rounding=std::fegetround();std::fesetround(FE_UPWARD);
    for(frame=1;frame<=600;++frame) {
        order.clear();startup_logo_update(state,in,pad,services);
        require(std::fegetround()==FE_UPWARD,"Prefix restores caller rounding");
        require(state.common.pressed_word==0x40&&state.common.tick_counter==frame,"Actual pad and counter preserved");
        require(state.common.image_alpha_bits==0xdeadbeef&&state.common.mode_state==0,"Unrelated task fields untouched");
        const auto expected=frame==270?std::vector<std::string>{"fade","initialize","image"}:
            frame==600?std::vector<std::string>{"fade","request"}:std::vector<std::string>{"fade","image"};
        require(order==expected,"Source callback order throughout timeline");
        if(frame==1||frame==270)require(state.common.generic_alpha_rate_bits==0x3ccccccd,"Positive rate raw bits");
        if(frame==180||frame==480)require(state.common.generic_alpha_rate_bits==0xbccccccd,"Negative rate raw bits");
    }
    std::fesetround(rounding);require(images==599&&initializations==1&&requests==1,"Noimage at600; real service once");
}
void failures_and_wrapping() {
    StartupTaskState state{{0,0,269,0,0,0xbccccccd,0},0};auto in=input();unsigned images=0,requests=0;
    StartupUpdateServices services{{[](float){},[&](auto){++requests;},[&](const auto&){++images;}},[]{throw std::runtime_error("asset failure");}};
    rejects([&]{startup_logo_update(state,in,pad,services);});
    require(state.common.tick_counter==270&&state.image_index==0&&state.common.generic_alpha_rate_bits==0xbccccccd&&images==0,"Failed service preserves actual prefix without pretending initialization completed");
    state.common.tick_counter=269;services.initialize_shared={};rejects([&]{startup_logo_update(state,in,pad,services);});
    require(state.common.tick_counter==270&&images==0,"Absent service is not successful initialization");
    state.common.tick_counter=0x7fffffffu;startup_logo_update(state,in,pad,services);
    require(state.common.tick_counter==0x80000000u&&images==1,"Signed negative counter still submits");
    state.common.tick_counter=0xffffffffu;startup_logo_update(state,in,pad,services);
    require(state.common.tick_counter==0&&images==2,"Counter wraps tozero");
    state.common.tick_counter=600;startup_logo_update(state,in,pad,services);
    require(images==2&&requests==0,"Above600 neither resubmits nor rerequests");
    state.common.submode=1;const auto prior=state.common.tick_counter;
    rejects([&]{startup_logo_update(state,in,pad,services);});require(state.common.tick_counter==prior,"Unsupported selector rejects before prefix");
    state.common.submode=0;state.common.tick_counter=269;
    services.initialize_shared=[&]{state.common.tick_counter=480;};startup_logo_update(state,in,pad,services);
    require(state.image_index==1&&state.common.generic_alpha_rate_bits==0xbccccccd,"Source rereads counter after270 service");
    state.common.tick_counter=599;services.common.request_mode=[&](auto){state.common.tick_counter=599;};
    const auto before=images;startup_logo_update(state,in,pad,services);require(images==before+1,"Source rereads counter after mode request");
}
}
int main(){try{timeline();failures_and_wrapping();std::cout<<"Startup logo tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
