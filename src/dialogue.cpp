#include "okage/dialogue.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <charconv>
#include <cstdlib>
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
std::uint16_t DialoguePadRepeat::update(std::uint16_t held,std::uint16_t pressed) {
    if(!pressed && held && previous==held){
        ticks=std::uint16_t(ticks+100);
        if(ticks>delay){ticks=std::uint16_t(ticks-interval);return previous;}
        return 0;
    }
    previous=pressed?pressed:held;ticks=0;return pressed;
}
DialogueInputEffects DialogueController::update(DialogueText& text,DialogueButtons buttons) {
    if(stage!=5)throw std::runtime_error("Dialogue controller stage not implemented");
    DialogueInputEffects effects;
    choice_active=false;
    const bool option=!text.state.option.empty() && static_cast<unsigned char>(text.state.option[0])!=255;
    const bool comparison=option && text.state.option.starts_with("CMP(");
    const bool flag_branch=option && text.state.option.starts_with("FFF(");
    // 0x1fcbd8 option table: CC->1, BC->2, QC->3 (mode0); ANSWERCMP->0x10.
    const bool question=option && text.state.option=="QC";
    const bool answer=option && text.state.option=="ANSWERCMP";
    if(option && !comparison && !flag_branch && !question && !answer &&
       text.state.option!="CC" && text.state.option!="BC" && text.state.option!="BC=0")
        throw std::runtime_error("Dialogue option outside recovered scope");
    auto resume=[&]{text.resume();text.state.period=std::bit_cast<std::uint32_t>(normal_period);++effects.resume_calls;};
    auto& command=text.state.window;
    if(!command.empty() && static_cast<unsigned char>(command[0])!=255) {
        // 0x148240 parses concatenated metadata in order. Some commands
        // replace its return ID; SHADOW preserves it. Resume happens only once
        // after the complete sequence, according to the final returned ID.
        unsigned handler=0xffffffff;
        std::string_view rest=command;
        while(!rest.empty()) {
            const auto end=rest.find(')');
            if(end==rest.npos)throw std::runtime_error("Malformed dialogue metadata sequence");
            const auto part=rest.substr(0,end+1);
            if(part.starts_with("WINTYPE(")) {
                const auto next=arguments(part,"WINTYPE(",1)[0];
                if(next<0 || next>2)throw std::runtime_error("Dialogue window type not implemented");
                if(next==2)effects.scene_fade=.5f;
                else if(window_type==2)effects.scene_fade=1.f;
                window_type=next;handler=7;
            } else if(part.starts_with("FADE(")) {
                const auto value=arguments(part,"FADE(",1)[0];
                if(value<0 || value>100)throw std::runtime_error("Dialogue fade range");
                effects.scene_fade=float(double(value)/100.0);handler=7;
            } else if(part.starts_with("SET(")) {
                const auto values=arguments(part,"SET(",3);
                if(values[0]<0 || values[1]<0 || !set_variable)throw std::runtime_error("Dialogue variable service unavailable");
                set_variable(values[0],values[1],values[2]);handler=12;
            } else if(part.starts_with("ADD(")) {
                const auto values=arguments(part,"ADD(",3);
                const auto index=values[0],bank=values[1];
                if(index<0 || index>0xffff || bank<0 || (bank>4 && (bank<8 || bank>15)) ||
                   (bank>=8 && index>=0x28))
                    throw std::runtime_error("Dialogue variable addition storage unavailable");
                // 0x148240 case13 ->0x1259d0: bit banks are unchanged;
                // byte/short/word storage wraps. The shared setter truncates
                // to the selected storage width after unsigned addition.
                if(bank>1) {
                    if(!get_variable || !set_variable)throw std::runtime_error("Dialogue variable addition service unavailable");
                    const auto sum=std::bit_cast<std::uint32_t>(get_variable(index,bank))+
                        std::bit_cast<std::uint32_t>(values[2]);
                    set_variable(index,bank,std::bit_cast<std::int32_t>(sum));
                }
            } else if(part.starts_with("SHADOW(")) {
                const auto value=arguments(part,"SHADOW(",1)[0];
                if(!shadow_request)throw std::runtime_error("Dialogue shadow service unavailable");
                shadow_request(std::bit_cast<std::uint32_t>(value));
            } else if(part.starts_with("CHANI(")) {
                const auto values=arguments(part,"CHANI(",2);
                if(!actor_motion_request)throw std::runtime_error("Dialogue actor motion service unavailable");
                actor_motion_request(values[0],values[1]);
            } else if(part.starts_with("LA(")) {
                const auto values=arguments(part,"LA(",3);
                if(text.choice_result<0 || text.choice_result>=3 || !get_variable || !set_variable)
                    throw std::runtime_error("Dialogue relationship service/selection unavailable");
                const auto code=values[text.choice_result];
                if(code<0 || code>12)throw std::runtime_error("Dialogue relationship adjustment outside reviewed scope");
                // 0x148240 handler11: reviewed signed-byte rows0..12 are zero,
                // unit basis vectors and doubled basis vectors. Retail bank4 ADD
                // wraps at32 bits, then the handler clamps signed-negative values.
                for(unsigned i=0;i<6;++i) {
                    const unsigned delta=code && unsigned((code-1)%6)==i ? unsigned((code-1)/6+1) : 0;
                    const auto value=std::bit_cast<std::int32_t>(
                        std::bit_cast<std::uint32_t>(get_variable(16+i,4))+delta);
                    set_variable(16+i,4,std::max(0,value));
                }
                handler=11;
            } else throw std::runtime_error("Dialogue window command not implemented");
            rest.remove_prefix(end+1);
        }
        command.assign(1,char(255)); // Original 0x19aef0 marks it consumed.
        // Case 7 resumes; SET returns 12 and must preserve a concurrent delay.
        if(handler==7||handler==11){
            if(handler==7) {
                if(window_type)layer_flags&=~0x20u;
                else if(!(layer_flags&0x20)) {layer_flags|=0x20;effects.cue=11;}
            }
            resume();
        }
    }
    if(window_type<0 || window_type>2)throw std::runtime_error("Dialogue window type not implemented");
    const auto delay=std::bit_cast<std::int32_t>(text.state.delay);
    // Original status getter: positive delay=3, negative delay/pause=2, else 1.
    const unsigned status=delay>0 ? 3 : (delay<0 || (text.state.flags&0x910)) ? 2 : 1;
    if(status==2) {
        effects.stop_voice=true;
        auto filtered=[](unsigned mask){
            if((mask&0xa000)==0xa000)mask&=~0x8000u;
            if((mask&0x5000)==0x5000)mask&=~0x1000u;
            if(mask&0x60)mask&=~0xf000u;
            if((mask&0x60)==0x60)mask&=~0x40u;
            if(mask&0xc)mask&=~0x40u;
            if((mask&0xc)==0xc)mask&=~8u;
            return mask;
        };
        if(option && text.state.option=="CC") {
            choice_active=true;
            const auto repeat=filtered(buttons.repeat),pressed=filtered(buttons.pressed);
            if(!choice_armed){if(repeat&0x5000)choice_armed=true;}
            else {
                if(repeat&0x1000)text.move_choice(-1);
                if(repeat&0x4000)text.move_choice(1);
            }
            if(choice_armed && (layer_flags&0x10) && (pressed&0x40)){
                effects.accepted_choice=text.choice_cursor;
                effects.reset_text_clock=text.select_choice(text.choice_cursor);
                choice_armed=false;resume();++advance_count;
            }
        } else if(question) {
            choice_active=true;
            const auto repeat=filtered(buttons.repeat),pressed=filtered(buttons.pressed);
            if(!choice_armed){if(repeat&0x5000)choice_armed=true;}
            else {
                if(repeat&0x1000) {
                    text.move_choice(-1);
                    if(text.choice_cursor>2)text.move_choice(text.choice_cursor+4);
                }
                if(repeat&0x4000) {
                    text.move_choice(1);
                    if(text.choice_cursor==3)text.move_choice(-3);
                }
            }
            question_flash_row=0;question_flash=0;
            if(choice_armed && (layer_flags&0x10) && (pressed&0x40)) {
                question_flash=10;choice_armed=false;question_flash_row=text.choice_cursor;
                effects.reset_text_clock=text.select_choice(unsigned(text.choice_cursor));
                resume();++advance_count;
            }
        } else if(answer) {
            // 0x148dc8 return0x10: branch1 when 0x146288 left layer flag8 set
            // (all three bound positions equal the generated answers), else
            // branch0. Selection/resume without an input-counter increment.
            effects.reset_text_clock=text.select_choice((layer_flags&8)?1:0);resume();
        } else if(flag_branch) {
            const auto values=arguments(text.state.option,"FFF(",4);
            if(values[0]<0||values[1]<0||values[2]<0||values[3]<0||!get_variable)
                throw std::runtime_error("Dialogue FFF variable service unavailable");
            // 0x1485f4: exact ==1 on the second pair short-circuits the first.
            // Otherwise the first read supplies the packed return's high16,
            // and a separate second read supplies the widget cursor.
            int branch=2,cursor=2;
            if(get_variable(values[2],values[3])!=1) {
                const auto first=get_variable(values[0],values[1]);
                branch=std::bit_cast<std::int16_t>(std::uint16_t(first));
                cursor=get_variable(values[0],values[1]);
            }
            text.set_option_cursor(cursor);
            effects.reset_text_clock=text.select_choice(std::uint32_t(branch));resume();
        } else if(comparison) {
            const auto end=text.state.option.find(')');
            if(end==std::string::npos)throw std::runtime_error("Malformed dialogue comparison");
            const auto values=arguments(std::string_view(text.state.option).substr(0,end+1),"CMP(",3);
            const auto op=std::string_view(text.state.option).substr(end+1);
            if(values[0]<0||values[1]<0||!get_variable)throw std::runtime_error("Dialogue comparison variable unavailable");
            const auto value=get_variable(values[0],values[1]);bool matches;
            if(op=="<")matches=value<values[2];
            else if(op==">")matches=value>values[2];
            else if(op=="=")matches=value==values[2];
            else if(op=="!=")matches=value!=values[2];
            else throw std::runtime_error("Dialogue comparison operator outside recovered scope");
            // 0x148240 returns handler14 with branch0 on true, branch1 on
            // false. 0x148dc8 selects/resumes without incrementing advances.
            effects.reset_text_clock=text.select_choice(matches?0:1);resume();
        } else if(option && (text.state.option=="BC" || text.state.option=="BC=0")) {
            const auto branch=text.state.option=="BC"?text.choice_result:(text.choice_result==0?0:1);
            effects.reset_text_clock=text.select_choice(branch);resume();
        } else if(layer_flags&0x10) {
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
        text.state.period=std::bit_cast<std::uint32_t>(normal_period);
        effects.stop_voice=true;effects.close=true;
        if(window_type==0)effects.cue=12;
        ++stage;
    }
    return effects;
}
void SceneFade::request(float value) {
    if(!std::isfinite(value))throw std::runtime_error("Nonfinite scene fade");
    enabled=true;hold=value!=1;target=value;
    publish();
}
void SceneFade::immediate(float value) {
    if(!std::isfinite(value))throw std::runtime_error("Nonfinite immediate scene fade");
    current=value;request(value);
}
void SceneFade::publish() {
    const double scaled=double(current)*128;
    if(!std::isfinite(scaled)||scaled<-2147483648.0||scaled>2147483647.0)
        throw std::runtime_error("Scene fade conversion outside recovered range");
    const auto converted=std::int32_t(scaled);
    const auto alpha=(128u-std::uint32_t(std::min(127,converted)))&255u;
    published_rgba_=(alpha<<24)|color;
}
void SceneFade::script_request(unsigned percent,int color_mode) {
    if(percent>100)throw std::runtime_error("Unrecovered event fade percentage");
    const auto rounding=std::fegetround();
    if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Unable to set event fade rounding");
    const float target=float(percent)*.01f;
    std::fesetround(rounding);
    if(color_mode==0)color=0;
    else if(color_mode==1)color=0xffffff;
    request(target);
}
void SceneFade::tick() {
    if(!enabled)return;
    const auto rounding=std::fegetround();
    if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Unable to set fade rounding");
    if(current<target){current+=step;if(current>target)current=target;}
    else if(current>target){current-=step;if(current<target)current=target;}
    else if(!hold)enabled=false;
    std::fesetround(rounding);
    publish();
}
unsigned SceneFade::alpha() const {return enabled ? published_rgba_>>24 : 0;}
void SceneFade::apply(Texture& image) const {
    if(!enabled)return;
    if(published_rgba_==0xffffffffu)throw std::runtime_error("Per-grid scene fade colors are unrecovered");
    if(image.rgba.size()!=std::size_t(image.width)*image.height*4)
        throw std::runtime_error("Scene fade target extent");
    const int a=alpha();
    for(std::size_t i=0;i<image.rgba.size();i+=4) {
        for(unsigned c=0;c<3;++c) {
            const int source=(published_rgba_>>(c*8))&255;
            const int destination=image.rgba[i+c];
            image.rgba[i+c]=std::clamp(destination+((source-destination)*a>>7),0,255);
        }
        image.rgba[i+3]=a;
    }
}
void DialogueOpening::initialize(DialogueText& text,TextClock& clock,DialogueController& controller,
                                DialogueOpacity& body,DialogueClosure& closure) {
    // Text allocator/default style plus field descriptor: body font2,
    // glyph flag0x10, two successfully constructed ready layers (0x50).
    text.state={};text.state.flags=0x8645;text.state.font=2;text.state.period=1;
    clock={};clock.glyph_flags=0x10;
    controller={};controller.stage=0;controller.window_type=-1;
    closure={};body.set(0,.2f);closure.decoration.set(0,.2f);decoration_ticks=0;
}
void DialogueOpening::bootstrap(DialogueText& text,TextClock& clock) {
    tick_text_clock(text.state,clock);
    text.state.line_height=24;text.state.period=0xfffffffeu;
    text.state.flags=(text.state.flags|0xa2u)&~0x201u;
    text.state.x=24;
}
bool DialogueOpening::update(DialogueText& text,TextClock& clock,DialogueController& controller,
                             DialogueOpacity& body,DialogueClosure& closure) {
    switch(controller.stage) {
    case 0:
        text.seek({});text.page.clear();text.glyphs.clear();
        text.state.flags=(text.state.flags&0xfffff6c5u)|1u;
        text.state.color=0x80808080;text.state.phase=text.state.x=text.state.y=text.state.line_origin=0;
        text.state.progress=0;clock.revealed=0;clock.scroll=0;clock.scroll_target=0;
        body.set(1,8);closure.decoration.set(128,-128);controller.stage=2;return true;
    case 2: // Decoration2->3 followed by body3->4.
        controller.stage=4;return true;
    case 4:
        text.state.period=std::bit_cast<std::uint32_t>(controller.normal_period);
        controller.stage=5;return true;
    case 5:
        decoration_ticks=std::min(8u,decoration_ticks+1);
        if(closure.decoration.value==0)closure.decoration.set(1,8);
        return false;
    default:throw std::runtime_error("Unrecovered field dialogue opening stage");
    }
}
std::string presentation_name(std::string_view source,std::string_view name) {
    // USA sysmes[0xd5]. The original 0x147ad0/0x147418 loops compare
    // the copied destination, not the advancing source. Preserve that
    // distinction when a shortened name exposes another marker.
    constexpr std::string_view marker="############";
    if(name.empty() || name.size()>marker.size() || name.find('\0')!=name.npos)
        throw std::runtime_error("Invalid presentation player name");
    std::string scratch(source);scratch.push_back('\0');
    std::size_t input=0,output=0;
    while(scratch.at(output)) {
        if(std::string_view(scratch).substr(output).starts_with(marker)) {
            if(input+marker.size()>source.size())
                throw std::runtime_error("Unrecovered presentation name source overrun");
            input+=marker.size();
            for(char c:name)scratch.at(output++)=c;
        } else {
            if(input>source.size())throw std::runtime_error("Unrecovered presentation name source overrun");
            const char c=input==source.size() ? '\0' : source[input];
            ++input;scratch.at(output++)=c;
            if(!c){scratch.resize(output-1);return scratch;}
        }
    }
    scratch.resize(output);return scratch;
}
namespace {
int narration_advance(unsigned char c,const Font& font){
    const auto* glyph=font.find(c);
    if(c<32 || c>=127 || c=='\\' || !glyph)throw std::runtime_error("Unrecovered narration character");
    if(c==' ')return 15;
    if(c>='0' && c<='9')return 21;
    return glyph->metrics[8]*3/32;
}
}
void Narration::update(std::string_view description,const Font& font,std::string_view player_name){
    std::vector<std::string> parts;
    if(!description.empty())for(;;){
        const auto end=description.find("\\n");parts.emplace_back(description.substr(0,end));
        if(end==description.npos)break;description.remove_prefix(end+2);
    }
    if(parts.size()>3)throw std::runtime_error("Narration exceeds three recovered lines");
    for(unsigned i=0;i<3;++i){
        auto& line=lines[i];
        if(i>=parts.size()){line.alpha=line.alpha<8?0:line.alpha-8;continue;}
        auto displayed=presentation_name(parts[i],player_name);
        if(displayed.empty())displayed=" ";
        int width=-28;for(unsigned char c:displayed)width+=narration_advance(c,font);
        if(line.source!=parts[i])line.ticks=0;
        line.source=std::move(parts[i]);line.text=std::move(displayed);
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
                const int px=int(x-std::bit_cast<std::int16_t>(m[2])*3.f/32.f)+24;
                const int py=int(y-(std::bit_cast<std::int16_t>(m[1])>>2)*.75f)+36;
                const int height=int((m[9]>>2)*.75f);
                result.push_back({c,2,0x00ffffffu|(line.alpha<<24),px,py/2,int(m[8]*3/32),height/2});
            }
            x+=advance;
        }
    }
    return result;
}
void ChoiceMenu::update(std::string_view replacement,int selection,bool armed,std::string_view player_name) {
    std::vector<std::string> parts;
    if(!replacement.empty())for(;;){
        const auto end=replacement.find("\\n");parts.emplace_back(replacement.substr(0,end));
        if(end==replacement.npos)break;replacement.remove_prefix(end+2);
    }
    if(parts.size()==3){
        if(!accepted){
            for(unsigned i=0;i<3;++i){lines[i]=presentation_name(parts[i],player_name);if(lines[i].empty())lines[i]=" ";}
            cursor=selection;highlighted=armed;alpha=std::min(128u,alpha+8);
        }
    } else {
        highlighted=false;alpha=alpha<32?0:alpha-32;
        if(!alpha)accepted=0;
    }
}
std::vector<TextGlyph> ChoiceMenu::glyphs(const Font& font) const {
    std::vector<TextGlyph> result;
    for(unsigned i=0;i<3;++i){
        int x=164-28,y=(352+26*i+(i==2))-27;
        for(unsigned char c:lines[i]){
            const auto* found=font.find(c);
            if(c<32 || c>=127 || c=='\\' || !found)throw std::runtime_error("Unrecovered choice character");
            const auto& m=found->metrics;
            const int advance=c==' '?10:(c>='0' && c<='9')?14:m[8]/16;
            if(c!=' '){
                const int px=x-std::bit_cast<std::int16_t>(m[2])/16+24;
                const int py=y-(std::bit_cast<std::int16_t>(m[1])>>2)/2+36;
                result.push_back({c,2,0x00ffffffu|(alpha<<24),px,py/2,int(m[8]/16),int((m[9]>>2)/4)});
            }
            x+=advance;
        }
    }
    return result;
}
void QuestionChoice::initialize(std::int32_t id,std::span<const std::int32_t> sysval,
                                const std::function<std::uint32_t()>& random,
                                DialogueController& controller,ChoiceMenu& menu) {
    // 0x1468c8 prologue: 1fcb74 (CC acceptance) and 1fcb78 are cleared for
    // every message; 1fcb70/1fcb64 have no other native owner here.
    menu.accepted=0;controller.question_flash=0;
    // 0x125708 returns 0 for a missing table, so the scan would never end.
    std::int64_t found=-1;
    for(std::size_t i=0;;++i) {
        if(i>=sysval.size())throw std::runtime_error("Question-choice sysval table missing or unterminated");
        if(sysval[i]==999999999)break;
        if(sysval[i]==id)found=std::int64_t(i); // movz: the last match wins.
    }
    generated=false;
    if(found<0)return;
    auto at=[&](std::int64_t index) {
        if(index<0 || std::size_t(index)>=sysval.size())
            throw std::runtime_error("Question-choice record read outside sysval (0x125708 is unchecked)");
        return sysval[std::size_t(index)];
    };
    auto draw=[&]{
        if(!random)throw std::runtime_error("Question-choice random stream unavailable");
        const auto value=std::bit_cast<std::int32_t>(random());
        if(value<0)throw std::runtime_error("Question-choice rand produced a negative value");
        return value;
    };
    const auto sets=at(found+4);
    words.fill(-1);answers.fill(-1);
    std::array<std::int32_t,3> counts{};
    for(unsigned k=0;k<3;++k)counts[k]=at(found+1+k);
    if(sets==0)throw std::runtime_error("Question-choice set count zero (division trap)");
    // Exclusion and candidate arrays are 16-word stack buffers in the source.
    if(sets<0 || sets>16)throw std::runtime_error("Question-choice answer-set count outside source stack storage");
    const auto offset=std::int64_t(draw()%sets)*3+4;
    for(unsigned slot=0;slot<3;++slot) {
        answers[slot]=draw()%3;
        const auto correct_at=std::size_t(slot*3+unsigned(answers[slot]));
        words[correct_at]=at(found+1+offset+slot);
        std::array<std::int32_t,16> excluded;excluded.fill(-1);
        for(std::int32_t k=0;k<sets;++k)excluded[std::size_t(k)]=at(found+1+slot+4+3*std::int64_t(k));
        auto is_excluded=[&](std::int32_t word) {
            bool hit=false;for(std::int32_t k=0;k<sets;++k)if(excluded[std::size_t(k)]==word)hit=true;
            return hit;
        };
        std::int32_t first=-1;
        for(unsigned pick=0;pick<2;++pick) {
            std::vector<std::int32_t> candidates;
            for(std::int32_t word=0;word<counts[slot];++word) {
                if(words[correct_at]==word || (pick && first==word) || is_excluded(word))continue;
                if(candidates.size()==16)throw std::runtime_error("Question-choice candidates overflow source stack storage");
                candidates.push_back(word);
            }
            for(unsigned position=0;position<3;++position) {
                auto& target=words[slot*3+position];
                if(target!=-1)continue;
                if(candidates.empty())throw std::runtime_error("Question-choice has no distractor candidate (division trap)");
                target=candidates[std::size_t(draw()%std::int32_t(candidates.size()))];
                if(!pick)first=target;
                break;
            }
        }
    }
    generated=true;
}
void QuestionChoice::update(DialogueText& text,MessageBank& bank,DialogueController& controller,ChoiceMenu& menu) {
    active=false;
    const auto& replacement=text.state.replacement;
    if(replacement_segments(replacement)<=3)return; //0x19b8f8 > 3 only.
    active=true;
    if(!generated)throw std::runtime_error("Question-choice widget without a matching sysval record reads stale 0x2dc6f0 tables");
    const auto& bytes=text.state.replacement_bytes;
    // 1f0558 = strtol(segment0,0,10).
    const auto first=replacement_segment(bytes,0);
    char* end=nullptr;const long parsed=std::strtol(first.c_str(),&end,10);
    if(parsed<0 || parsed>2)throw std::runtime_error("Question-choice slot outside the three recovered table rows");
    slot=int(parsed);
    const int cursor=text.choice_cursor;
    if(controller.question_flash<1){controller.question_flash_row=0;highlight_row=cursor;}
    else {--controller.question_flash;highlight_row=controller.question_flash_row;}
    const bool armed=controller.choice_armed;
    const int scaled=controller.question_flash==0?cursor:controller.question_flash_row;
    static constexpr std::array<std::string_view,3> tags{"##########$$","########$$$$","######$$$$$$"}; //sysmes0xc9..0xcb
    for(unsigned i=0;i<3;++i) {
        const auto word=words[unsigned(slot)*3+i];
        if(word<0)throw std::runtime_error("Question-choice word index selects the slot header");
        lines[i]=replacement_segment(bytes,unsigned(word)+1);
        scale[i]=int(i)==scaled&&armed?std::array<float,2>{1.1f,1.2f}:std::array<float,2>{1.f,1.f};
        if(text.choice_result==int(i)) {
            // sysmes0xcc "\c[%d]%s\c[0]" with 0x78505050, bound by 0x19a5c8
            // sprintf "\r[%s]%s" into the tag's own PME storage.
            const auto value="\\c[2018529360]"+lines[i]+"\\c[0]";
            const auto index=bank.tagged_string(tags[unsigned(slot)]);
            if(!index)throw std::runtime_error("Question-choice answer tag missing from message bank");
            const auto size=4+tags[unsigned(slot)].size()+value.size()+1;
            if(*index>=bank.capacities.size() || size>bank.capacities[*index])
                throw std::runtime_error("Question-choice binding exceeds its PME string storage");
            bank.bind(tags[unsigned(slot)],value);
            chosen[unsigned(slot)]=int(i);
            if(answers==chosen)controller.layer_flags|=8u;else controller.layer_flags&=~8u;
        }
    }
    // Widgets0x12..0x14 absolute alpha0x80 after 0x146ee8's CC fade; widget3
    // alpha is 0x30 only when armed (else 0xf0,0xf0,100 at alpha0).
    menu.lines=lines;menu.alpha=0x80;menu.cursor=highlight_row;menu.highlighted=armed;
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
