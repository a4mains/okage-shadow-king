#include "okage/dialogue.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <charconv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
std::vector<int> arguments(std::string_view command,std::string_view name,unsigned count) {
    if(!command.starts_with(name) || command.back()!=')')throw std::runtime_error("Malformed dialogue metadata");
    auto rest=command.substr(name.size(),command.size()-name.size()-1);std::vector<int> result;
    while(!rest.empty()){
        const auto end=rest.find('/');const auto part=rest.substr(0,end);int value;
        const auto parsed=std::from_chars(part.data(),part.data()+part.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=part.data()+part.size())throw std::runtime_error("Malformed dialogue number");
        result.push_back(value);if(end==rest.npos)break;
        rest.remove_prefix(end+1);if(rest.empty())throw std::runtime_error("Missing dialogue argument");
    }
    if(result.size()!=count)throw std::runtime_error("Dialogue argument count");return result;
}
}
DialogueInputEffects DialogueController::update(DialogueText& text,DialogueButtons buttons) {
    if(stage!=5)throw std::runtime_error("Dialogue controller stage not implemented");
    if(!text.state.option.empty())throw std::runtime_error("Dialogue choices not implemented");
    DialogueInputEffects effects;
    auto resume=[&]{text.resume();text.state.period=__builtin_bit_cast(std::uint32_t, (normal_period));++effects.resume_calls;};
    auto& command=text.state.window;
    if(!command.empty() && static_cast<unsigned char>(command[0])!=255) {
        bool resuming=true;
        if(command.starts_with("WINTYPE(")) {
            const auto next=arguments(command,"WINTYPE(",1)[0];
            if(next<0 || next>2)throw std::runtime_error("Dialogue window type not implemented");
            if(next==2)effects.scene_fade=.5f;
            else if(window_type==2)effects.scene_fade=1.f;
            window_type=next;
        } else if(command.starts_with("FADE(")) {
            const auto value=arguments(command,"FADE(",1)[0];
            if(value<0 || value>100)throw std::runtime_error("Dialogue fade range");
            effects.scene_fade=float(double(value)/100.0);
        } else if(command.starts_with("SET(")) {
            const auto values=arguments(command,"SET(",3);
            if(values[0]<0 || values[1]<0 || !set_variable)throw std::runtime_error("Dialogue variable service unavailable");
            set_variable(values[0],values[1],values[2]);resuming=false;
        } else throw std::runtime_error("Dialogue window command not implemented");
        command.assign(1,char(255)); // Original 0x19aef0 marks it consumed.
        // Case 7 resumes; SET returns 12 and must preserve a concurrent delay.
        if(resuming){
            if(window_type)layer_flags&=~0x20u;
            else if(!(layer_flags&0x20)) {layer_flags|=0x20;effects.cue=11;}
            resume();
        }
    }
    if(window_type<0 || window_type>2)throw std::runtime_error("Dialogue window type not implemented");
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
void SceneFade::request(float value) {
    if(!std::isfinite(value)||value<0||value>1)throw std::runtime_error("Scene fade range");
    enabled=true;hold=value!=1;target=value;
}
void SceneFade::tick() {
    if(!enabled)return;
    const auto rounding=std::fegetround();
    if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Unable to set fade rounding");
    if(current<target){current+=step;if(current>target)current=target;}
    else if(current>target){current-=step;if(current<target)current=target;}
    else if(!hold)enabled=false;
    std::fesetround(rounding);
}
unsigned SceneFade::alpha() const {return enabled ? 128-std::min(127,int(current*128.f)) : 0;}
namespace {
int narration_advance(unsigned char c,const Font& font){
    const auto* glyph=font.find(c);
    if(c<32 || c>=127 || c=='\\' || !glyph)throw std::runtime_error("Unrecovered narration character");
    if(c==' ')return 15;
    if(c>='0' && c<='9')return 21;
    return glyph->metrics[8]*3/32;
}
}
void Narration::update(std::string_view description,const Font& font){
    std::vector<std::string> parts;
    if(!description.empty())for(;;){
        const auto end=description.find("\\n");parts.emplace_back(description.substr(0,end));
        if(end==description.npos)break;description.remove_prefix(end+2);
    }
    if(parts.size()>3)throw std::runtime_error("Narration exceeds three recovered lines");
    for(unsigned i=0;i<3;++i){
        auto& line=lines[i];
        if(i>=parts.size()){line.alpha=line.alpha<8?0:line.alpha-8;continue;}
        if(parts[i].empty())parts[i]=" ";
        int width=-28;for(unsigned char c:parts[i])width+=narration_advance(c,font);
        if(line.text!=parts[i])line.ticks=0;
        line.text=std::move(parts[i]);
        // 0x1aa4a8 reads dimensions from the preceding render. The first two
        // low-alpha ticks hide the initial/stale width when a line changes.
        line.x=line.width<0?0:300-line.width/2;line.width=width;
        line.alpha=line.ticks<2?2:std::min(128u,line.alpha+8);
        if(++line.ticks>1)line.ticks=11;
    }
}
std::vector<TextGlyph> Narration::glyphs(const Font& font) const {
    std::vector<TextGlyph> result;
    for(unsigned i=0;i<3;++i){
        const auto& line=lines[i];int x=line.x-28,y=80+40*i-27;
        for(unsigned char c:line.text){
            const auto* found=font.find(c);if(!found)throw std::runtime_error("Missing narration glyph");
            const auto& m=found->metrics;const auto advance=narration_advance(c,font);
            if(c!=' '){
                const int px=int(x-__builtin_bit_cast(std::int16_t, (m[2]))*3.f/32.f)+24;
                const int py=int(y-(__builtin_bit_cast(std::int16_t, (m[1]))>>2)*.75f)+36;
                const int height=int((m[9]>>2)*.75f);
                result.push_back({c,2,0x00ffffffu|(line.alpha<<24),px,py/2,int(m[8]*3/32),height/2});
            }
            x+=advance;
        }
    }
    return result;
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
