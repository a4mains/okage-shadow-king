#include "okage/title.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
constexpr std::uint32_t one=0x3f800000, step7=0x3be56042,
    step15=0x3c75c28f, step25=0x3ccccccd, pulse_top=0x3f4ccccd;
float value(std::uint32_t word){return std::bit_cast<float>(word);}
std::uint32_t bits(float value){return std::bit_cast<std::uint32_t>(value);}
std::int32_t signed_word(std::uint32_t word){return std::bit_cast<std::int32_t>(word);}
std::uint32_t add(std::uint32_t a,std::uint32_t b){
    volatile float x=value(a),y=value(b),z=x+y;return bits(z);
}
std::uint32_t sub(std::uint32_t a,std::uint32_t b){
    volatile float x=value(a),y=value(b),z=x-y;return bits(z);
}
std::uint32_t mul(std::uint32_t a,std::uint32_t b){
    volatile float x=value(a),y=value(b),z=x*y;return bits(z);
}
void increase(std::uint32_t& word,std::uint32_t step){
    word=add(word,step);if(value(word)>1.f)word=one;
}
void decrease(std::uint32_t& word,std::uint32_t step){
    word=sub(word,step);if(value(word)<0.f)word=0;
}
class Rounding {
    std::fenv_t saved_{};
public:
    Rounding(){
        if(std::fegetenv(&saved_)!=0)throw std::runtime_error("Cannot save title floating environment");
        if(std::fesetround(FE_TOWARDZERO)!=0){
            std::fesetenv(&saved_);throw std::runtime_error("Cannot set title rounding");
        }
    }
    ~Rounding(){std::fesetenv(&saved_);}
};
}

void title_update(TitleTaskState& t,GameOverInputState& input,GameOverPadSample pad,
                  std::uint32_t confirm_mask,const TitleScratch& scratch,
                  const TitleUpdateServices& s) {
    if(t.common.submode!=1)throw std::runtime_error("Title helper requires mode8 selector1");
    Rounding rounding;
    mode8_update_prefix(t.common,input,pad,s.common);
    auto& c=t.common;
    const auto image=[&](unsigned index,std::uint32_t x,std::uint32_t y,std::uint32_t alpha){
        s.common.submit_image({index,{x,y,one,one,0,alpha}});
    };
    s.set_camera(0,{0,0,0xc2c80000,scratch.camera_w});
    s.set_camera(1,{0,0,0,scratch.camera_w});
    t.yaw=add(t.yaw,0x3ac49ba6);c.generic_alpha_rate_bits=step25;
    if(value(t.yaw)>1.f)t.yaw=sub(t.yaw,one);
    const auto base=s.model_channel_base();
    auto position=s.read_model_channel(base+1);
    position[0]=0;position[1]=0x42480000;position[2]=0x43480000;
    s.set_model_channel(base+1,position);
    auto rotation=s.read_model_channel(base);
    rotation[0]=0x3ca3d70a;rotation[1]=t.yaw;rotation[2]=0;
    s.set_model_channel(base,rotation);
    image(0,0,0,c.image_alpha_bits); //old alpha, before common title fade
    const auto rgb=mul(c.image_alpha_bits,0x3f666666);
    const auto alpha=mul(c.image_alpha_bits,0x3f400000);
    if(signed_word(c.mode_state)>0)s.set_water_color(t.water,{rgb,rgb,rgb,alpha});
    s.set_water_clamp(t.water,2);
    if(signed_word(c.mode_state)<50){
        if(value(c.image_alpha_bits)<1.f){
            increase(c.image_alpha_bits,0x3b449ba6);
            s.common.fade(value(c.image_alpha_bits));
        }
        // Re-read source state after the service call before the second block.
        if(signed_word(c.mode_state)<50){
            for(auto i:{1u,2u,6u})increase(t.alpha[i],step7);
            if(t.pulse_rising==1){
                t.pulse=add(t.pulse,step7);
                if(value(t.pulse)>value(pulse_top))t.pulse_rising=0;
            }else{
                t.pulse=sub(t.pulse,step7);
                if(value(t.pulse)<value(0x3e4ccccd))t.pulse_rising=1;
            }
            t.alpha.at(t.pulse_channel)=t.pulse;
        }
    }
    for(unsigned i=0;i<3;++i)if(t.visible[i]==1)image(i+1,t.x[i],t.y[i],t.alpha[i]);
    switch(c.mode_state){
    case 0:
        t.y[0]=0x444f8000;c.tick_counter=0;t.x[0]=0;c.mode_state=10;t.visible[0]=1;
        t.water=s.create_water({106,
            {0x3e4ccccd,0x40d00000,0,scratch.water_transform_w[0]},
            {0x3e800000,0,0,scratch.water_transform_w[1]},
            {0x40c00000,one,0x3ee66666,scratch.water_transform_w[2]}});
        s.rebind_water(t.water,0,t.texture);
        s.set_water_color(t.water,{scratch.water_rgb[0],scratch.water_rgb[1],scratch.water_rgb[2],0});
        return;
    case 10:
        ++c.tick_counter;
        if(c.pressed_word&0x800){
            c.tick_counter=0;c.mode_state=15;t.alpha[0]=one;s.play_sound(3);
        }
        if(signed_word(c.tick_counter)>1500)c.mode_state=12;
        break;
    case 12:case 13:
        if(value(c.image_alpha_bits)>value(0x3c23d70a)){
            decrease(c.image_alpha_bits,step15);s.common.fade(value(c.image_alpha_bits));return;
        }
        if(c.mode_state==13){s.reset_game();s.reset_progress();s.common.request_mode({9,0,0});}
        else s.common.request_mode({11,0,0});
        break;
    case 15:
        decrease(t.alpha[0],step15);t.pulse_channel=3;++c.tick_counter;
        t.y[0]=sub(t.y[0],0x40400000);
        if(signed_word(c.tick_counter)>=40){
            t.visible[6]=1;c.mode_state=20;t.alpha[6]=0;c.tick_counter=0;
            t.visible[0]=0;t.alpha[1]=0;t.visible[1]=1;t.alpha[2]=0;t.visible[2]=1;
        }
        break;
    case 20:
        ++c.tick_counter;
        for(auto i:{1u,2u,3u,4u,6u})increase(t.alpha[i],0x3c449ba6);
        for(auto i:{1u,2u,3u,4u})t.y[i]=0x444f8000;
        t.x[1]=0xc4af0000;t.x[2]=0x44ac8000;t.x[4]=0xc2a00000;t.x[3]=0xc51c4000;
        image(4,t.x[3],t.y[3],t.alpha[1]);image(5,t.x[4],t.y[4],t.alpha[2]);
        if(signed_word(c.tick_counter)>=40){
            t.selection=1;c.mode_state=30;t.pulse=pulse_top;t.pulse_channel=2;c.tick_counter=0;t.pulse_rising=0;
        }
        break;
    case 30:{
        const auto selected=[&]{
            t.pulse_channel=t.selection==2?6:t.selection+1;
            t.pulse=t.alpha.at(t.pulse_channel);
            if(value(t.pulse)>value(pulse_top))t.pulse=pulse_top;
            t.pulse_rising=0;s.play_sound(7);
        };
        if(input.direction_flags[3]==1){
            --t.selection;if(signed_word(t.selection)<0)t.selection=0;selected();
        }
        if(input.direction_flags[1]==1){
            ++t.selection;if(signed_word(t.selection)>1)t.selection=1;selected();
        }
        image(t.selection==0?5:4,t.x[3],t.y[3],one);
        image(t.selection==0?4:5,t.x[4],t.y[4],one);
        if(c.pressed_word&confirm_mask){
            c.tick_counter=0;s.play_sound(3);
            if(t.selection==0)c.mode_state=50;
            if(t.selection==1)c.mode_state=55;
            if(t.selection==2)c.mode_state=57;
        }
        break;}
    case 50:
        decrease(t.alpha[2],step25);++c.tick_counter;t.x[1]=add(t.x[1],0x41700000);t.alpha[1]=one;
        decrease(t.alpha[6],step25);
        if(value(t.x[1])>0.f){c.tick_counter=0;c.mode_state=60;}
        break;
    case 55:
        decrease(t.alpha[1],step25);++c.tick_counter;t.x[2]=sub(t.x[2],0x41700000);
        decrease(t.alpha[6],step25);t.alpha[2]=one;
        if(value(t.x[2])<0.f){c.tick_counter=0;c.mode_state=60;}
        break;
    case 57:
        decrease(t.alpha[1],step25);++c.tick_counter;t.x[6]=sub(t.x[6],0x41c80000);
        decrease(t.alpha[2],step25);t.alpha[6]=one;
        if(value(t.x[6])<0.f){c.tick_counter=0;c.mode_state=60;}
        break;
    case 60:
        s.common.fade(value(c.image_alpha_bits));c.image_alpha_bits=sub(c.image_alpha_bits,step25);
        if(value(c.image_alpha_bits)<=0.f){
            c.image_alpha_bits=0;s.stop_water(t.water);
            if(t.selection==1)s.common.request_mode({10,0,0});
            else if(t.selection==0){s.reset_game();s.reset_progress();s.common.request_mode({9,0,0});}
            else if(t.selection==2){
                s.select_saved_field(0);const auto field=s.read_saved_field(2);
                s.common.request_mode({2,field[0],field[1]});
            }
        }
        break;
    default:break;
    }
}
} // namespace okage
