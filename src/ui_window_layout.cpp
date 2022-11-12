#include "okage/ui_layout.hpp"
#include "okage/ui_layout_geometry.hpp"
#include "okage/ui_text_number.hpp"
#include "okage/font.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#include <string_view>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
using I=std::int32_t;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
I si(std::uint32_t v){return __builtin_bit_cast(I, (v));}
I add(I a,I b){return si(std::uint32_t(a)+std::uint32_t(b));}
I sub(I a,I b){return si(std::uint32_t(a)-std::uint32_t(b));}
std::uint32_t bits(float f){return __builtin_bit_cast(std::uint32_t, (f));}
float ordinary(float f){const auto b=bits(f),e=b&0x7f800000u;
    check(e!=0x7f800000u&&(e!=0||(b&0x7fffffu)==0),"UI mode2 exceptional float");return f;}
float number(I i){volatile I v=i;volatile float f=static_cast<float>(v);return f;}
I integer(float f){ordinary(f);check(double(f)>=-2147483648.0&&double(f)<2147483648.0,"UI mode2 integer conversion");return static_cast<I>(f);}
float op(float a,float b,char operation){volatile float x=ordinary(a),y=ordinary(b),v;
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI mode2 clear floating exceptions");
    if(operation=='+')v=x+y;else if(operation=='-')v=x-y;else v=x*y;
    check(!std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW),"UI mode2 float overflow/underflow");return ordinary(v);}
struct Rounding {std::fenv_t saved;
    Rounding(){check(std::fegetenv(&saved)==0,"UI mode2 save fenv");if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI mode2 rounding");}}
    ~Rounding(){std::fesetenv(&saved);}};
bool space(unsigned char c){return c==' '||(c>=9&&c<=13);}
//19bd00: whitespace then unsigned decimal digits; sign is not consumed.
std::uint32_t decimal(std::string_view text){std::size_t i=0;while(i<text.size()&&space(text[i]))++i;
    std::uint32_t v=0;while(i<text.size()&&text[i]>='0'&&text[i]<='9')v=v*10+unsigned(text[i++]-'0');return v;}
}
bool UiWindow::prepare_mode2(UiLayoutContext context) {
    auto flags=word(0x1900);
    if(!(flags&0x8000))return false;
    if(flags&0x1000)put(0x1900,(flags&~0x22u)|0x201u);
    const auto display=word(0x25c)&255u;
    if(((display>>2)&3)&&(word(0x1900)&0x200)){
        build_background_packet();put(0x1900,word(0x1900)&~0x200u);
    }
    check(!(display&3)||!(word(0x1900)&0x400),"UI border cache construction is unrecovered");
    bool laid_out=false;
    flags=word(0x1900);
    if(((display>>4)&3)&&!(flags&0x20)&&(flags&1)){
        put(0x268c,word(0x268c)^1u);put(0x2694,word(0x2694)^1u);
        if(ordinary(__builtin_bit_cast(float, (word(0x27c))))!=0||
           ordinary(__builtin_bit_cast(float, (word(0x270))))!=0){
            layout_mode2(context);laid_out=true;
        }
        put(0x1900,word(0x1900)&~1u);
    }
    put(0x1904,0);return laid_out;
}
void UiWindow::layout_mode2(UiLayoutContext context) {
    check(word(0x3bec)==2,"UI layout is not mode2");
    check(!context.fonts.empty()&&bool(context.random)&&context.sine.size()>=360,"UI layout owners missing");
    check(context.packets.fonts&&context.fonts.data()==context.packets.fonts->data()&&
          context.fonts.size()==context.packets.fonts->size(),"UI layout and packet font owners differ");
    const unsigned capacity=word(0x3c00);
    check(capacity>0,"UI mode2 zero glyph capacity");
    Rounding rounding;
    auto& shared=context.shared;
    const auto floating=[&](unsigned at){return ordinary(__builtin_bit_cast(float, (word(at))));};
    const auto put_i=[&](unsigned at,I v){put(at,std::uint32_t(v));};
    put(0x22c,0x80808080);put(0xcf8,0);put(0xcfc,0);put(0xcb0,24);
    put(0x224,bits(1));put(0x228,bits(1));
    for(auto at:{0xcdc,0x269c,0xcb4,0x1968,0x196c})put(at,0);
    shared.sprite_groups=0;shared.layer=1;shared.sprite_dirty=false;shared.glyph_metric.reset();
    for(unsigned g=0;g<capacity;++g)for(unsigned j=0;j<16;++j){
        glyph_[1].put_word((g*16+j)*16+8,bits(-1));put(0x3c08+j*4,word(0x3c08+j*4)|4);}
    reset_glyph_packets();reset_sprite_packets();
    put(0xce8,300000);put(0x268,word(0x26c));put(0x258,word(0x250));
    put(0x1900,(word(0x1900)&0xfffff6c5u)|1);put(0x3be4,0xffffffff);
    for(auto at:{0x264,0x260,0xcd0})put(at,0);put(0x254,word(0x250));
    for(unsigned g=0;g<capacity;++g)put(0x3c08+g*4,word(0x3c08+g*4)|5);
    for(auto& buffer:glyph_)for(std::size_t at=0;at<buffer.size();at+=4)buffer.put_word(at,0);
    for(unsigned j=0;j<capacity*16;++j)glyph_[1].put_word(j*16+8,bits(-1));

    unsigned glyph_group=0,glyph_slot=0,glyph_groups=1,next_slot=1,unchanged=0;
    unsigned sprite_group=0,sprite_slot=0;
    I x=0,y=0,line=0,width=0,height=0;
    std::uint32_t previous_color=0x80808080,source_characters=0;
    std::optional<UiWidgetReference> text_owner;
    const auto placement=[&](){return UiPlacementContext{shared.layer,context.sine,context.random};};
    const auto set_cursor=[&](){
        put_i(0xcb0,add(x,24));const auto last=sub(si(word(0x250)),1);
        put_i(0xcb4,line<last?y:si(std::uint32_t(last)*word(0x21c)));
    };
    const auto text=[&](std::span<const std::uint8_t> data){
        std::size_t at=0;
        while(true){
            check(at<data.size(),"UI mode2 unterminated text");
            if(data[at]==0)break;
            ++source_characters;
            const auto start=at;
            if(data[at]=='\\'){
                check(at+1<data.size(),"UI mode2 truncated control");
                const auto control=data[at+1];at+=2;
                check(control=='c'||control=='f'||control=='s',"UI mode2 unrecovered text control");
                check(at<data.size(),"UI mode2 truncated control arguments");
                if(data[at]=='['){
                    const auto begin=++at;
                    while(at<data.size()&&data[at]!=']'&&data[at]!=0)++at;
                    check(at<data.size()&&data[at]==']',"UI mode2 unterminated control arguments");
                    const std::string_view argument(reinterpret_cast<const char*>(data.data()+begin),at-begin);
                    ++at;
                    if(control=='c'){
                        const auto color=decimal(argument);
                        if(color==0)put(0x22c,previous_color);
                        else{previous_color=word(0x22c);put(0x22c,color+0x80u);}
                    }else if(control=='f')put(0x20c,decimal(argument));
                    else{
                        const auto comma=argument.find(',');check(comma!=argument.npos,"UI mode2 scale pair");
                        put(0x224,bits(parse_ui_text_number(argument.substr(0,comma))));
                        put(0x228,bits(parse_ui_text_number(argument.substr(comma+1))));
                    }
                }
                if(at<data.size()&&data[at]<32)++at;
                source_characters+=std::uint32_t(at-start);
                continue;
            }
            if(data[at]<32){++at;++source_characters;continue;}
            check(data[at]<128,"UI mode2 multibyte text is unrecovered");
            const auto font_index=si(word(0x20c));
            check(font_index>=0&&std::size_t(font_index)<context.fonts.size(),"UI mode2 font handle");
            const auto* metric=context.fonts[font_index].find(data[at]);
            I code=I(data[at]);
            if(!metric){code=-1;shared.glyph_metric.reset();}
            else{
                shared.glyph_metric=std::array{std::uint32_t(font_index),std::uint32_t(data[at])};
                if(data[at]==32){put_i(0xcb0,add(x,10));const auto last=sub(si(word(0x250)),1);
                    put_i(0xcb4,line<last?y:si(std::uint32_t(last)*word(0x21c)));height=si(word(0x21c));width=10;
                }else{
                    height=integer(op(number(metric->metrics[9]),.0625f,'*'));
                    width=integer(op(op(number(metric->metrics[8]),floating(0x224),'*'),.0625f,'*'));
                }
            }
            const auto flags=word(0x1900);
            if((flags&0x40)&&si(word(0x24c)*24u)<add(x,width)){
                x=0;y=add(y,si(word(0x21c)));
                //20 is cleared by1a4ac0 and no supported control enables it.
                check((flags&0x20)==0,"UI mode2 auto extent is unrecovered");
                line=add(line,1);
                if(si(word(0x254))<=line){put(0x254,word(0x254)+1);put(0x260,word(0x260)+word(0x21c));}
            }
            check((flags&0x20)==0,"UI mode2 auto extent is unrecovered");
            if(number(y)<=op(floating(0x264),24.f,'-'))code=-1;
            check(si(word(0x3be4))<1,"UI mode2 line break marker is unrecovered");
            const auto offset=(glyph_group*16+glyph_slot)*16;
            const auto old=integer(__builtin_bit_cast(float, (glyph_[1].word(offset+8))));
            glyph_[1].put_word(offset+8,bits(number(code)));
            if(old==code)++unchanged;
            place_glyph(glyph_group,glyph_slot,x,y,width,height,context.fonts,placement());
            const auto style=word(0x4f8);
            if((style&0x10)&&code==32)x=integer(op(number(x),op(floating(0x224),10.f,'*'),'+'));
            else if((style&0x10)&&std::uint32_t(code)-48u<10u)x=integer(op(number(x),op(floating(0x224),14.f,'*'),'+'));
            else if(code==32)x=add(x,10);
            else if(style&0x80)x=integer(op(number(x),op(number(si(word(0x218))),floating(0x224),'*'),'+'));
            else x=add(x,width);
            put(0xcdc,word(0xcdc)+1);
            check(si(word(0xce8))>=si(word(0xcdc)),"UI mode2 reveal cutoff is unrecovered");
            glyph_slot=next_slot;
            if(next_slot>=16){
                set_cursor();put(0xcf0,glyph_group);put(0xcec,next_slot-1);
                const auto flag_at=0x3c08+glyph_group*4;
                put(flag_at,unchanged==16?word(flag_at)|1:word(flag_at)&~1u);
                unchanged=0;glyph_group=glyph_groups%capacity;++glyph_groups;glyph_slot=0;
            }
            if(text_owner){
                const auto& payload=text_owner->resource->widgets()[text_owner->group][text_owner->index].runtime->words;
                auto delta=sub(y,si(payload[7]));if(delta<0)delta=si(0u-std::uint32_t(delta));
                text_owner->resource->set_layout_text_extent(text_owner->group,text_owner->index,sub(x,si(payload[6])),delta);
            }
            next_slot=glyph_slot+1;++at;
        }
    };
    for(const auto& entry:stream_->entries){
        if(const auto* command=std::get_if<UiStreamCommand>(&entry)){
            check(command->opcode>=1&&command->opcode<=27,"UI mode2 command extent");
            ++source_characters;
            bool text_command=false;
            if(command->opcode==23){
                const auto* ref=std::get_if<UiWidgetReference>(&command->argument);
                check(ref&&ref->resource&&ref->group<ref->resource->widgets().size()&&
                      ref->index<ref->resource->widgets()[ref->group].size(),"UI mode2 widget reference");
                const auto& widget=ref->resource->widgets()[ref->group][ref->index];
                if(widget.words[4]&1){
                    if(widget.words[5]==1){
                        check(widget.runtime&&widget.runtime->text,"UI mode2 text payload absent");
                        const auto& w=widget.runtime->words;text_owner=*ref;text_command=true;
                        put(0x22c,w[4]);x=sub(si(w[6]),28);y=sub(si(w[7]),27);
                        put_i(0x1968,x);put_i(0x196c,y);put(0x224,w[2]);put(0x228,w[3]);
                        const auto& bytes=*widget.runtime->text;
                        text({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()});
                    }else{
                        auto batch=layout_ruw_sprites(*ref->resource,widget,context.resource_y_offset,
                            {floating(0x224),floating(0x228),{word(0x22c),UiLayoutColorConversion::Direct}});
                        for(const auto& sprite:batch.sprites){
                            shared.sprite_dirty=place_sprite(sprite_group,sprite_slot,sprite)||shared.sprite_dirty;
                            if(++sprite_slot==16){sprite_slot=0;++sprite_group;++shared.sprite_groups;}
                        }
                        put(0x224,bits(batch.final_style.scale_x));put(0x228,bits(batch.final_style.scale_y));
                        put(0x22c,ui_layout_color_rgba(batch.final_style.color));
                    }
                }
            }
            put(0x269c,word(0x269c)+(text_command?1u:2u));
        }else{
            const auto& bytes=std::get<UiStreamText>(entry).bytes;text(bytes);put(0x269c,word(0x269c)+1);
        }
    }
    put(0x1900,word(0x1900)|0x22);set_cursor();put(0xcec,glyph_slot);
    put_i(0xcf4,integer(__builtin_bit_cast(float, (glyph_[1].word((glyph_group*16+glyph_slot)*16+8)))));
    for(unsigned j=next_slot;j<16;++j)glyph_[1].put_word((glyph_group*16+j)*16+8,bits(-1));
    put(0xcf0,glyph_group);
    reset_glyph_packets();reset_sprite_packets();put(0x3e24,0);
    for(unsigned i=0;i<glyph_groups;++i)build_glyph_packet(context.packets);
    put(0xcc0,word(0x3e24));
    for(unsigned slot=sprite_slot;slot<16;++slot)
        shared.sprite_dirty=place_icon(sprite_group,slot,0,0,0,-1,placement())||shared.sprite_dirty;
    if(!shared.sprite_dirty){
        check(word(0x3e28)<32,"UI mode2 previous sprite groups");
        for(unsigned group=0;group<=word(0x3e28);++group)put(0x3c08+group*4,word(0x3c08+group*4)&~8u);
    }else{
        put(0x3e28,0);put(0x2690,word(0x2690)^1u);
        for(unsigned group=0;group<=shared.sprite_groups;++group)build_sprite_packet(context.packets);
    }
    put(0xcc4,word(0x3e28));shared.layer=0;put(0x1900,word(0x1900)|0x80);
}
}
