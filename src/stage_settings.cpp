#include "okage/stage_settings.hpp"
#include <bit>
#include <cmath>
#include <cfenv>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Stage rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
std::int32_t integer(float value) {
    if(!std::isfinite(value)||value<-2147483648.f||value>=2147483648.f)
        throw std::runtime_error("STG integer conversion range");
    if(std::trunc(value)!=value)throw std::runtime_error("Fractional STG conversion rounding is unrecovered");
    return std::int32_t(value);
}
std::uint32_t bits(float value){return std::uint32_t(integer(value));}
}
StageSettings::StageSettings(SceneClearColor color):clear(color) {
    //1002f8 ->1bb040(3,521,517,9);101058 ->18fb00/1b8028.
    for(unsigned i=0;i<filters.size();++i)if(i!=1&&i!=2)filters[i]=std::array<std::uint32_t,4>{};
    shadows.threshold=0;
    clear_word=unsigned(clear.rgb[0])|(unsigned(clear.rgb[1])<<8)|(unsigned(clear.rgb[2])<<16);
}
void StageSettings::initialize_filter(unsigned slot) {
    if(slot==0||slot>=filters.size()||!filters[slot])throw std::runtime_error("Unowned stage filter initialization");
    std::uint32_t color=0x40808080;
    if(slot==1||slot==6)color=0x80808080;
    if(slot==7)color=0xffffff;
    if(slot==8)color=0;
    filters[slot]=std::array<std::uint32_t,4>{1,slot==5?0x40u:0u,color,0};
}
void StageSettings::disable_filter(unsigned slot) {
    if(slot>=filters.size()||!filters[slot])throw std::runtime_error("Unowned stage filter disable");
    (*filters[slot])[0]=0;
}
void StageSettings::apply(const StageSettingRecord& record,FieldProjection& projection) {
    const auto kind=__builtin_bit_cast(std::int16_t, (std::uint16_t(record.kind)));
    Float4 v;for(unsigned c=0;c<4;++c)v[c]=__builtin_bit_cast(float, (record.value_bits[c]));
    for(float f:v)if(!std::isfinite(f))throw std::runtime_error("Nonfinite stage setting");
    Rounding rounding;
    if(kind>=1&&kind<=3){lighting.colors[kind-1]=v;return;}
    if(kind>=0x11&&kind<=0x13){v[3]=0;lighting.directions[kind-0x11]=v;return;}
    switch(kind) {
    case 0x21:case 0x22:case 0x23:return; //Explicit original no-op cases.
    case 0x40:lighting.base=v;return;
    case 0x41:lighting.colors[3]=v;return;
    case 0x42: {
        //Only RGB24 is initialized by the authored wrapper's stack packing.
        const auto blue=bits(v[2])&255,red=bits(v[0])&255,green=bits(v[1])&255;
        fog_rgb=red|(green<<8)|(blue<<16);return;
    }
    case 0x43:case 0x44:
        projection.apply_stage_setting(kind,v,cached_depth_min,cached_depth_max);return;
    case 0x45:
        lighting.base=v;for(unsigned c=0;c<3;++c)v[c]=v[c]*.25f;
        lighting.colors[3]=v;return;
    case 0x46:sky=v;return;
    case 0x47: {
        const auto input=integer(v[0]);int selected=-1;unsigned extra=0;
        if(input>=100&&input<200){selected=(input-100)/10;if(selected>4)selected=0x100;}
        else if(input>=200&&input<300)extra=8;
        else if(input>=300&&input<400)extra=7;
        else if(input>=400)selected=0;
        const auto w=bits(v[3]),z=bits(v[2]),y=bits(v[1]);const auto color=(w<<16)|(z<<8)|y;
        bucket_base=(selected==-1?28u:25u)-bucket_shift;selector=std::uint32_t(selected);
        if(selected==-1) {
            cached_depth_min=__builtin_bit_cast(float, (0x42ca0000u));cached_depth_max=__builtin_bit_cast(float, (0x4b7fff9bu));
            disable_filter(6);
        } else {
            cached_depth_min=__builtin_bit_cast(float, (0x43b18000u));cached_depth_max=__builtin_bit_cast(float, (0x49ccc9acu));
            initialize_filter(6);(*filters[6])[1]=selector;(*filters[6])[2]=color|0x80000000;
        }
        disable_filter(7);disable_filter(8);
        if(extra){initialize_filter(extra);if(extra==7)(*filters[7])[2]=color;}
        return;
    }
    case 0x48: {
        auto value=integer(v[0]);const bool flag=value>127;if(flag)value&=127;
        const auto w=bits(v[3]),z=bits(v[2]),y=bits(v[1]);
        const auto color=(std::uint32_t(value)<<24)|y|(z<<8)|(w<<16);
        if(!value)disable_filter(4);
        else {initialize_filter(4);(*filters[4])[1]=flag;(*filters[4])[2]=color;}
        return;
    }
    case 0x49: {
        auto value=integer(v[0]);if(!value)disable_filter(5);
        else {if(value>127)value=127;initialize_filter(5);(*filters[5])[1]=std::uint32_t(value);}
        return;
    }
    case 0x4a:shadows.direction=v;return;
    case 0x4b:
        for(unsigned c=0;c<3;++c) {
            const float product=v[c]*255.f;
            if(double(product)!=double(v[c])*255.)throw std::runtime_error("Inexact STG tint rounding is unrecovered");
            shadows.tint[c]=bits(product);
        }
        return;
    case 0x4c:shadows.threshold=bits(v[0]);return;
    case 0x4d: {
        const float difference=v[0]-128.f;
        if(double(difference)!=double(v[0])-128.)throw std::runtime_error("Inexact STG signed-setting rounding is unrecovered");
        signed_setting=bits(difference);return;
    }
    case 0x60: {
        std::array<std::uint32_t,4> converted;
        for(unsigned c=0;c<4;++c)converted[c]=bits(v[c]);
        clear_word=converted[0]|(converted[1]<<8)|(converted[2]<<16);
        for(unsigned c=0;c<3;++c)clear.rgb[c]=std::uint8_t(converted[c]);
        return;
    }
    default:throw std::runtime_error("Unrecovered stage setting "+std::to_string(kind));
    }
}
}
