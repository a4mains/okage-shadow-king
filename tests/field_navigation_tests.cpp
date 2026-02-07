#include "okage/field_navigation.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace okage;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
auto words(float x,float y,float sx,float sy,float angle,float alpha) {
    return std::array<std::uint32_t,6>{std::bit_cast<std::uint32_t>(x),std::bit_cast<std::uint32_t>(y),
        std::bit_cast<std::uint32_t>(sx),std::bit_cast<std::uint32_t>(sy),std::bit_cast<std::uint32_t>(angle),std::bit_cast<std::uint32_t>(alpha)};
}
}
int main(){try{
    const auto sine=make_sine_table();
    const auto identity=words(0,0,1,1,0,1);
    require(source_image_quad(640,448,identity,sine).xy16==axis_aligned_image_quad(640,448,identity).xy16,
            "general zero-pivot source lowering preserves exact title subset");
    const auto quarter=source_image_quad(32,16,words(0,0,1,1,.25f,.7f),sine);
    require(quarter.xy16==std::array<std::array<std::uint32_t,2>,4>{{{33024,32704},{33024,32832},{32512,32832},{32512,32704}}},
            "source nonsquare quarter turn scales each output axis, rather than rotating scaled rectangle");
    require(quarter.rgba==0x59808080&&quarter.z==0xffff,"source alpha truncation and z");
    require(source_image_quad(16,16,words(0,0,1,1,0,130.f/128),sine).rgba==0x82808080,
            "source alpha packing preserves above-one raw GS alpha without clamping");
    bool title_rejected=false;
    try{(void)axis_aligned_image_quad(16,16,words(0,0,1,1,0,130.f/128));}
    catch(const std::exception&){title_rejected=true;}
    require(title_rejected,"legacy title subset retains its recovered alpha guard");
    const auto negative=source_image_quad(32,16,words(0,0,1,1,-1.f/32768,1),sine);
    require(negative.xy16==source_image_quad(32,16,identity,sine).xy16,
            "direct16384 angle truncation differs from camera doubled-angle indexing");
    require(std::bit_cast<std::uint32_t>(sine[167])==0x3d831265&&std::bit_cast<std::uint32_t>(sine[4263])==0x3f7f79a5&&
            std::bit_cast<std::uint32_t>(sine[11680])==0xbf791298&&std::bit_cast<std::uint32_t>(sine[15776])==0xbe6c9a7f,
            "source-derived native sine entries match retained source table witnesses");
    const auto fractional=source_image_quad(235,86,{0x43f3af75,0x43ea0ee6,0x3fe0b857,0x3facf445,0x3c27dd44,0x3f333333},sine);
    require(fractional.xy16==std::array<std::array<std::uint32_t,2>,4>{{{30172,32742},{36760,32802},{36337,33729},{29750,33670}}},
            "fractional source golden detects product distribution/reassociation");
    const auto centered=source_image_quad(16,16,{0xc554f4aa,0x44486148,0x3f800000,0x3f800000,0x3f368203,0x3f000000},sine);
    require(centered.xy16==std::array<std::array<std::uint32_t,2>,4>{{{29265,33646},{29206,33522},{29455,33492},{29514,33616}}},
            "fractional source golden detects center-after-local ordering");
    FieldDefinition definition;definition.type=0;definition.navigation_flags=2;
    definition.navigation_scale=std::bit_cast<float>(0x3fca3d71u);
    definition.navigation_offset_x=64;definition.navigation_offset_z=128;
    FieldNavigation navigation(definition,56,112,16,16);
    const int rounding=std::fegetround();
    auto commands=navigation.queue({0,0,0,1},{0,0,0,0},false,sine);
    require(std::fegetround()==rounding,"source packet arithmetic restores caller rounding");
    require(commands.size()==2&&commands[0].xy16[0]==std::array<std::uint32_t,2>{28032,32320}&&
            commands[1].xy16[0]==std::array<std::uint32_t,2>{29120,33408}&&commands[1].st[0]==.5f/1024&&commands[1].st[2]==15.5f/1024,
            "owned e_field mapping queues NAVI then previous-pose marker with source center and scale");
    require(commands[0].rgba==0x59808080&&commands[1].rgba==0x40808080&&navigation.pulse()>.5f,
            "marker uses current pulse before advancing source state");
    const float pulse=navigation.pulse();
    commands=navigation.queue({-1000,0,0,1},{0,0,0,0},true,sine);
    require(commands.empty()&&!navigation.visible()&&navigation.pulse()==pulse&&navigation.center()>-3840,
            "hidden NAVI still adjusts next-frame center but neither draws nor advances pulse");
    commands=navigation.queue({0,0,0,1},{0,0,0,0},true,sine);
    require(commands.size()==2&&navigation.visible(),"ordinary toggle reenables source image commands");
    bool reversed=false;
    for(unsigned i=0;i<100&&!reversed;++i){navigation.queue({0,0,0,1},{0,0,0,0},false,sine);reversed=navigation.delta()<0;}
    require(reversed&&navigation.pulse()>=1,"source pulse reverses at crossed upper bound without clamping");
    require(std::bit_cast<unsigned>(navigation.pulse())==0x3f828f58,
            "source TZ pulse retains the first upper overshoot word");
    bool overshoot_drawn=false,lower_reversed=false;
    for(unsigned i=0;i<200;++i) {
        const auto next=navigation.queue({0,0,0,1},{0,0,0,0},false,sine);
        overshoot_drawn|=(next[1].rgba>>24)==130;
        if(!lower_reversed&&navigation.delta()>0&&navigation.pulse()<=.4f) {
            require(std::bit_cast<unsigned>(navigation.pulse())==0x3ecccc98,
                    "source TZ pulse retains the first lower reversal word");
            lower_reversed=true;
        }
    }
    require(overshoot_drawn&&lower_reversed,"complete source pulse cycles draw upper overshoot and reverse at both bounds");
    std::cout<<"Field navigation source mapping, image packet ordering, hidden state and pulse passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
