#include "okage/stage_settings.hpp"
#include <bit>
#include <cfenv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#pragma STDC FENV_ACCESS ON

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F&& f) {
    try {f();} catch(const std::runtime_error&){return;}
    throw std::runtime_error("expected unsupported stage setting rejection");
}
okage::StageSettingRecord record(std::uint32_t kind,okage::Float4 values={}) {
    okage::StageSettingRecord result;result.kind=kind;
    for(unsigned i=0;i<4;++i)result.value_bits[i]=__builtin_bit_cast(std::uint32_t, (values[i]));
    return result;
}
void apply(okage::StageSettings& s,okage::FieldProjection& p,std::uint32_t kind,okage::Float4 v={}) {
    s.apply(record(kind,v),p);
}
void finish_frame(okage::FieldProjection& p) {
    p.begin_field();p.begin_effects();p.begin_deferred();p.end_effects();
}
void defaults() {
    okage::StageSettings s;okage::FieldProjection p;p.reset_scene();
    require(s.bucket_shift==9&&s.bucket_base==19&&s.selector==0xffffffff,"startup bucket defaults");
    require(s.cached_depth_min==101&&s.cached_depth_max==16777115,"reset cached depth");
    require(s.signed_setting==0&&s.fog_rgb==0x404040,"retirement scalar defaults");
    require(s.shadows.threshold==0&&s.shadows.direction==okage::Float4{1,1,0,0},"battle reset differs from field phase1 threshold10");
    require(s.shadows.tint==std::array<std::uint32_t,3>{255,64,192},"shadow reset tint");
    require(s.lighting.base==okage::Float4{1,1,1,1}&&s.lighting.colors[3]==okage::Float4{.25f,.25f,.25f,1},"reset base/ambient");
    require(s.lighting.colors[0]==okage::Float4{1,1,1,0}&&s.lighting.colors[1]==okage::Float4{}&&s.lighting.colors[2]==okage::Float4{},"reset direct light colors");
    require(s.lighting.directions==std::array<okage::Float4,3>{{{0,0,1,0},{0,1,0,0},{1,0,0,0}}},"reset light axes");
    require(s.sky==okage::Float4{.5f,0,.5f,0},"reset sky");
    require(!s.filters[1]&&!s.filters[2],"capture/fade memory was invented");
    for(unsigned i=0;i<9;++i)if(i!=1&&i!=2)
        require(s.filters[i]&&*s.filters[i]==std::array<std::uint32_t,4>{},"retirement filter reset");
}
void ordered_projection() {
    okage::StageSettings s;okage::FieldProjection p;p.reset_scene();
    const auto before=okage::field_projection_words(p.current());
    apply(s,p,0x47,{110,0,0,0});
    require(s.bucket_base==16&&s.selector==1,"enabled bucket/selector");
    require(s.cached_depth_min==355&&__builtin_bit_cast(std::uint32_t, (s.cached_depth_max))==0x49ccc9ac,"enabled cache range");
    require(okage::field_projection_words(p.current())==before,"47 prematurely changed current descriptor");
    apply(s,p,0x44,{1,.5f,800,0});
    require(p.current().depth_min==101&&p.current().clip_far==1024,"44 w0 prematurely applied cache range");
    apply(s,p,0x43,{120,350,0,0});
    require(p.effect_parameters()==std::array<float,4>{3,1024,4096,16384},"effects changed before101260 sync");
    apply(s,p,0x44,{1,.5f,800,900});
    require(p.current().depth_min==355&&p.current().clip_far==900,"44 w>=10 did not consume current cache");
    require(p.effect_parameters()==std::array<float,4>{3,1024,4096,16384},"far changed before effect sync");
    p.synchronize_stage_effects();
    require(p.effect_parameters()==std::array<float,4>{3,900,120,350},"101260 did not synchronize far/fog");
    apply(s,p,0x47,{0,0,0,0});
    require(s.bucket_base==19&&s.cached_depth_min==101&&p.current().depth_min==355,"disable cache/current separation");
    apply(s,p,0x44,{1,.5f,800,0});
    require(p.current().depth_min==355,"44 w0 erased previous current range");
    apply(s,p,0x44,{1,.5f,800,800});
    require(p.current().depth_min==101&&p.current().depth_max==16777115,"ordered disabled cache consumption");
}
void independent_caches_after_submission() {
    okage::StageSettings s;okage::FieldProjection p;p.reset_scene();p.synchronize_stage_effects();
    apply(s,p,0x44,{1,1,800,900});
    finish_frame(p);
    require(p.current().clip_far==1024&&p.current().fog_near==0,"test requires effect-owned current descriptor");
    apply(s,p,0x43,{120,350,0,0});
    p.synchronize_stage_effects();
    require(p.effect_parameters()==std::array<float,4>{3,900,120,350},"43 copied unrelated current far into field cache");
    finish_frame(p);
    require(p.current().fog_near==0&&p.current().fog_far==0,"deferred fog baseline");
    apply(s,p,0x44,{1,.5f,700,0});
    p.synchronize_stage_effects();
    require(p.effect_parameters()==std::array<float,4>{3,900,120,350},"44 w0 copied unrelated deferred fog into cache");
    p.begin_field();p.begin_effects();p.select_effect_near(10);p.begin_deferred();p.end_effects();
    p.synchronize_stage_effects();
    require(p.effect_parameters()[0]==10,"101260 reset shared effect near");
}
void filters_and_packing() {
    okage::StageSettings s;okage::FieldProjection p;
    apply(s,p,0x12340047,{110,256,2,3});
    require(*s.filters[6]==std::array<std::uint32_t,4>{1,1,0x80030300,0},"47 wrapping color or low16 opcode");
    auto preserved=*s.filters[6];preserved[0]=0;
    apply(s,p,0x47,{200,5,6,7});
    require(*s.filters[6]==preserved&&*s.filters[8]==std::array<std::uint32_t,4>{1,0,0,0},"200 selector disable/slot8 default");
    apply(s,p,0x47,{300,1,2,3});
    require((*s.filters[8])[0]==0&&*s.filters[7]==std::array<std::uint32_t,4>{1,0,0x030201,0},"300 extra filter7 color");
    apply(s,p,0x47,{150,0,0,0});require(s.selector==0x100,"150 saturation selector");
    apply(s,p,0x47,{400,0,0,0});require(s.selector==0,"400 selector resets0");
    apply(s,p,0x48,{129,-1,2,3});
    require(*s.filters[4]==std::array<std::uint32_t,4>{1,1,0xffffffff,0},"48 signed color OR/mask");
    auto slot4=*s.filters[4];slot4[0]=0;apply(s,p,0x48,{128,0,0,0});
    require(*s.filters[4]==slot4,"48 disable erased retained parameters");
    apply(s,p,0x49,{-2,0,0,0});require((*s.filters[5])[1]==0xfffffffe,"49 clamped negative value");
    apply(s,p,0x49,{200,0,0,0});require((*s.filters[5])[1]==127,"49 upper clamp");
    auto slot5=*s.filters[5];slot5[0]=0;apply(s,p,0x49);
    require(*s.filters[5]==slot5,"49 disable erased parameters");
    require(!s.filters[1]&&!s.filters[2],"stage operations touched separate capture/fade owners");
    apply(s,p,0x42,{-1,258,513,0});require(s.fog_rgb==0x0102ff,"42 wrapper byte narrowing");
    apply(s,p,0x60,{256,2,3,4});
    require(s.clear_word==0x030300&&s.clear.rgb==std::array<std::uint8_t,3>{0,2,3},"60 raw OR global versus clear byte channels");
    apply(s,p,0x4d,{-40,0,0,0});require(s.signed_setting==0xffffff58,"authored signed setting");
    apply(s,p,0x4d,{64,0,0,0});require(s.signed_setting==0xffffffc0,"later signed setting");
    apply(s,p,0x4c,{-1,0,0,0});require(s.shadows.threshold==0xffffffff,"signed conversion threshold bits");
    rejects([&]{apply(s,p,0xffff0047u|0x8000u);}); //Negative low16 is not47.
}
void guards_and_rounding() {
    const int saved=std::fegetround();
    for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        require(std::fesetround(mode)==0,"cannot set host rounding mode");
        okage::StageSettings s;okage::FieldProjection p;
        apply(s,p,0x4b,{1,0,1,0});require(std::fegetround()==mode,"success changed caller rounding");
        require(s.shadows.tint==std::array<std::uint32_t,3>{255,0,255},"exact tint values");
        for(const auto& bad:std::vector<okage::StageSettingRecord>{
            record(0x49,{.5f,0,0,0}),record(0x49,{2147483648.f,0,0,0}),
            record(0x49,{-2147483904.f,0,0,0}),record(0x42,{0,0,std::numeric_limits<float>::infinity(),0}),
            record(0x47,{std::numeric_limits<float>::quiet_NaN(),0,0,0}),
            record(0x4b,{.5f,0,0,0}),record(0x4b,{100000.0078125f,0,0,0}),
            record(0x4d,{-2147483648.f,0,0,0}),record(0x60,{0,0,0,.5f})}) {
            rejects([&]{s.apply(bad,p);});require(std::fegetround()==mode,"rejection changed caller rounding");
        }
        apply(s,p,0x49,{-2147483648.f,0,0,0});
        require((*s.filters[5])[1]==0x80000000,"exact int32 lower bound was rejected/changed");
    }
    require(std::fesetround(saved)==0,"restore original rounding");
}
void diagnostic(const std::string& path) {
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("cannot read owned STG");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
    auto stage=okage::StageData::parse(bytes);require(bytes.size()==868&&stage.settings().size()==18,"expected owned B_ROOM1 layout");
    okage::StageSettings s;okage::FieldProjection p;p.reset_scene();
    for(const auto& r:stage.settings())s.apply(r,p);
    require(s.fog_rgb==0x0a0a0a&&s.signed_setting==0xffffff58,"authored stage scalar sequence");
    require(*s.filters[6]==std::array<std::uint32_t,4>{1,1,0x80000000,0},"authored stage filter6");
    require(s.cached_depth_min==355&&p.current().depth_min==101,"authored cache/current distinction");
    p.synchronize_stage_effects();require(p.effect_parameters()==std::array<float,4>{3,1024,120,350},"authored effect synchronization");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--diagnostic")diagnostic(argv[2]);
        else {require(argc==1,"usage: stage-settings-tests [--diagnostic owned-B_ROOM1-a.stg]");
            defaults();ordered_projection();independent_caches_after_submission();filters_and_packing();guards_and_rounding();}
        std::cout<<"stage settings tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
