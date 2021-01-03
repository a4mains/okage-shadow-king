#include "okage/dialogue.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
DialogueInputEffects DialogueController::update(DialogueText& text,DialogueButtons buttons) {
    if(stage!=5)throw std::runtime_error("Dialogue controller stage not implemented");
    if(!text.state.option.empty())throw std::runtime_error("Dialogue choices not implemented");
    DialogueInputEffects effects;
    auto resume=[&]{text.resume();text.state.period=__builtin_bit_cast(std::uint32_t, (normal_period));++effects.resume_calls;};
    auto& command=text.state.window;
    if(!command.empty() && static_cast<unsigned char>(command[0])!=255) {
        if(command=="WINTYPE(0)")window_type=0;
        else if(command=="WINTYPE(1)")window_type=1;
        else throw std::runtime_error("Dialogue window command not implemented");
        command.assign(1,char(255)); // Original 0x19aef0 marks it consumed.
        if(window_type)layer_flags&=~0x20u;
        else if(!(layer_flags&0x20)) {layer_flags|=0x20;effects.cue=11;}
        resume();
    }
    if(window_type!=0 && window_type!=1)throw std::runtime_error("Dialogue window type not implemented");
    const auto delay=__builtin_bit_cast(std::int32_t, (text.state.delay));
    // Original status getter: positive delay=3, negative delay/pause=2, else 1.
    const unsigned status=delay>0 ? 3 : (delay<0 || (text.state.flags&0x910)) ? 2 : 1;
    if(status==2) {
        effects.stop_voice=true;
        if(layer_flags&0x10) {
            if(buttons.pressed&0x40) {
                resume();++advance_count;
                resume();++advance_count;effects.stop_effect=true;
            }
        }
    } else if(status==1 && !automatic && !((buttons.held|buttons.pressed)&4) &&
              (layer_flags&0x10) && (buttons.pressed&0x40)) {
        text.state.period=0xfffffffcu;effects.accelerated=true;effects.stop_voice=true;
    }
    layer_advance=advance_count;
    if(status==3)effects.stop_voice=true;
    // Re-evaluate status after a resume, as the original does for end handling.
    if((text.state.flags&2) && !text.state.delay && !(text.state.flags&0x910)) {
        text.state.period=__builtin_bit_cast(std::uint32_t, (normal_period));
        effects.stop_voice=true;effects.close=true;
        if(window_type==0)effects.cue=12;
        ++stage;
    }
    return effects;
}
void DialogueOpacity::set(float initial,float step,float minimum,float maximum) {
    if(!std::isfinite(initial)||!std::isfinite(step)||!std::isfinite(minimum)||!std::isfinite(maximum)||minimum>maximum)
        throw std::runtime_error("Invalid dialogue opacity");
    value=initial;delta=step;lower=minimum;upper=maximum;
}
void DialogueOpacity::tick() {
    if(!std::isfinite(value)||!std::isfinite(delta)||!std::isfinite(lower)||!std::isfinite(upper)||lower>upper)
        throw std::runtime_error("Invalid dialogue opacity");
    if(delta==0)return;
    const auto rounding=std::fegetround();
    if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Unable to set opacity rounding");
    value+=delta;
    if(delta>0 && value>upper) {delta=0;value=upper;}
    else if(delta<0 && value<lower) {delta=0;value=lower;}
    std::fesetround(rounding);
}
void DialogueClosure::update(unsigned& stage) {
    if(stage<6 || stage>9)throw std::runtime_error("Dialogue closure stage outside recovered scope");
    // Decoration controller 0x148958 does not advance the stage-slot index;
    // body controller 0x148dc8 therefore reads the same, possibly changed word.
    if(stage==6) {decoration.set(128,-4);++stage;}
    else if(stage==7 && decoration.value==0)++stage;
    // Body stages 6/7 are passive. 8 advances, 9 marks the message for removal.
    if(stage==8)++stage;
    else if(stage==9)message_flags|=0x80000000u;
}
}
