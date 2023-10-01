#include "okage/results_animation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string_view>

using namespace okage;
namespace {
void require(bool b,const std::source_location at=std::source_location::current()) {
    if(!b)throw std::runtime_error("Results animation assertion line "+std::to_string(at.line()));
}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=std::uint8_t(value>>(i*8));
}
void hex(std::vector<std::uint8_t>& b,unsigned at,std::string_view s) {
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<s.size()/2;++i)b.at(at+i)=std::uint8_t(digits.find(s[i*2])*16+digits.find(s[i*2+1]));
}
ResultsUiData data_fixture() {
    std::vector<std::uint8_t> b(1080);
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','S','U','I','1',0};
    std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1);put(b,12,28);put(b,16,16);put(b,20,49);
    hex(b,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    hex(b,56,"9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183");
    for(const auto [word,value]:std::array<std::pair<unsigned,unsigned>,11>{{
        {0,1},{1,44},{2,40},{3,1},{21,1},{22,1},{23,1},{24,11},{25,33},{26,17},{27,0}}})put(b,88+word*4,value);
    constexpr std::array<unsigned,16> ids={64,65,66,67,114,115,116,117,110,111,112,113,118,119,120,121};
    for(unsigned i=0;i<ids.size();++i)put(b,200+i*4,ids[i]);
    unsigned record=0;
    for(unsigned size:std::array<unsigned,4>{14,24,6,5}) {
        for(unsigned i=0;i<size;++i,++record) {
            const auto at=264+record*16;
            put(b,at,i+1==size?0xffffffffu:i);
            put(b,at+4,i+1==size?0xffffffffu:(i==0?1u:(size==24&&(i==2||i==4)?2u:0u)));
            put(b,at+8,i+1==size?0u:40u);
        }
    }
    constexpr std::array<std::uint32_t,8> streams={12,24,6,18,0xfffffc18u,10000,0,0x3f800000u};
    for(unsigned i=0;i<streams.size();++i)put(b,1048+i*4,streams[i]);
    return ResultsUiData(b);
}
RuwResource resource_fixture(unsigned cap=128,int missing=-1,std::uint32_t initial_width=0) {
    constexpr unsigned header=80,count=24;
    std::vector<std::uint8_t> b(header+count*68);
    put(b,12,1);put(b,16,14);put(b,76,count); //Empty template; widgets only group13.
    for(unsigned i=0;i<count;++i) {
        const auto at=header+i*68;
        put(b,at+8,(cap<<24)|0x123456);put(b,at+12,0x80112233);
        put(b,at+16,int(i)==missing?1:0x61);put(b,at+60,i);
        put(b,at+44,19);put(b,at+48,23);put(b,at+52,initial_width);put(b,at+56,71);
    }
    RuwResource result(b);
    result.set_class_rgba(13,0,{RuwResource::preserve_component,RuwResource::preserve_component,RuwResource::preserve_component,0});
    return result;
}
unsigned alpha(const RuwResource& r,unsigned id){return r.widget(13,id).runtime->words[4]>>24;}
void set_alpha(RuwResource& r,unsigned value) {
    constexpr auto k=RuwResource::preserve_component;r.set_class_rgba(13,0,{k,k,k,std::int32_t(value)});
}
void thresholds_and_completion() {
    const auto data=data_fixture();auto r=resource_fixture();ResultsAnimation a;
    a.timer=0xffffffffu;a.advance=7;a.heading(r,data);require(alpha(r,0)==0&&a.timer==0xffffffffu&&a.advance==7);
    a.timer=11;a.heading(r,data);require(alpha(r,0)==6&&alpha(r,1)==0&&a.timer==11);
    a.timer=12;a.heading(r,data);require(alpha(r,0)==12&&alpha(r,1)==6&&alpha(r,2)==0);
    r=resource_fixture(128,-1,39);set_alpha(r,122);a.timer=156;a.advance=0;
    a.heading(r,data);require(a.advance==0&&a.timer==156&&alpha(r,12)==128);
    // Extent already clamped on the first pass; alpha exact equality did not complete.
    a.heading(r,data);require(a.advance==1&&a.timer==0&&r.widget(13,0).runtime->words[8]==40);
    r=resource_fixture();set_alpha(r,128);constexpr auto k=RuwResource::preserve_component;
    r.set_rgba(13,12,{k,k,k,0});a.timer=156;a.advance=0;a.heading(r,data);
    require(a.advance==0&&alpha(r,12)==6);
    r=resource_fixture(60);set_alpha(r,54);a.heading(r,data);require(a.advance==0&&alpha(r,12)==60);
    a.heading(r,data);require(a.advance==1&&a.timer==0);
    r=resource_fixture(128,12);a.timer=156;a.advance=0;a.heading(r,data);require(a.advance==0&&a.timer==156);
}
void item_counter_and_tables() {
    const auto data=data_fixture();auto r=resource_fixture();ResultsAnimation a;
    a.timer=24;a.items(r,data,2);require(a.timer==24&&alpha(r,2)==6&&alpha(r,3)==0);
    a.timer=48;a.items(r,data,2);require(a.timer==240&&alpha(r,20)==6&&alpha(r,21)==0&&a.advance==0);
    r=resource_fixture();a.timer=24;a.items(r,data,0);require(a.timer==240&&alpha(r,20)==6);
    r=resource_fixture();a.timer=24;a.items(r,data,0xffffffffu);require(a.timer==240); //Source signed LW comparison.
    r=resource_fixture();set_alpha(r,128);a.timer=276;a.items(r,data,8);require(a.timer==0&&a.advance==1);
    r=resource_fixture();a.timer=0x80000000u;a.advance=9;a.items(r,data,0);
    require(a.timer==0x80000000u&&a.advance==9&&alpha(r,0)==0);
}
void quick_modes_and_preservation() {
    const auto data=data_fixture();auto r=resource_fixture();ResultsAnimation a;
    a.timer=0xffffffffu;a.currency(r,data,true);require(alpha(r,0)==0&&a.timer==0xffffffffu);
    a.timer=0;a.currency(r,data,true);require(alpha(r,0)==128&&alpha(r,1)==0&&a.advance==0);
    a.timer=60;a.advance=11;a.currency(r,data,true);require(a.timer==60&&a.advance==11&&alpha(r,4)==128);
    r=resource_fixture();a.timer=0xffffffffu;a.advance=0;
    const auto before=r.widget(13,0);a.party_heading(r,data,true);
    require(a.timer==0&&a.advance==1&&alpha(r,3)==128&&alpha(r,4)==0);
    auto expected=before.runtime->words;expected[4]=(expected[4]&0xffffff)|0x80000000u;expected[8]=40;
    auto expected_raw=before.words;expected_raw[4]|=1;
    require(r.widget(13,0).runtime->words==expected&&r.widget(13,0).words==expected_raw);
    r=resource_fixture(128,-1,0x7fffffffu);a.timer=0;a.currency(r,data,true);
    require(r.widget(13,0).runtime->words[8]==0x800001abu);
    r=resource_fixture();a.timer=0;a.advance=0;a.party_heading(r,data,false);
    require(alpha(r,0)==6&&alpha(r,1)==0&&a.advance==0);
    // A missing last runtime does not manufacture completion in quick mode.
    r=resource_fixture(128,3);a.timer=0xffffffffu;a.party_heading(r,data,true);require(a.advance==0&&a.timer==0xffffffffu);
}
}
int main() {
    try{thresholds_and_completion();item_counter_and_tables();quick_modes_and_preservation();
        std::cout<<"Results animation tests passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
