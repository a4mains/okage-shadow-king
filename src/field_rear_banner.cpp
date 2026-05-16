#include "okage/field_rear_banner.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace okage {
namespace {
constexpr std::int32_t keep=RuwResource::preserve_component;
const RuwWidget& text_owner(const RuwResource& r) {
    (void)r.widget_index(FieldRearBanner::group,FieldRearBanner::text_widget);
    const auto& widget=r.widget(FieldRearBanner::group,FieldRearBanner::text_widget);
    if(widget.words[5]!=1||!widget.runtime||!widget.runtime->text)
        throw std::runtime_error("Rear encounter banner requires field RUW group16 text widget1");
    return widget;
}
}
FieldRearBanner::FieldRearBanner(RuwResource& r,std::string_view format,std::uint32_t& flag) {
    (void)text_owner(r);
    //115b20: 115ad8 writes the layer stream [14,1f9c00,6,1f9c2c], 115b18
    //terminates it, then 0x1f8608=0. 11f668 completes with one layer.
    flag=0;
    //115b50: flag first, then 123530(0x72) and 1f15a8 into a 128-byte stack
    //buffer. Only the reached single %s conversion is supported.
    flag=1;
    const auto conversion=format.find('%');
    if(conversion==format.npos||format.substr(conversion,2)!="%s"||
       format.find('%',conversion+2)!=format.npos)
        throw std::runtime_error("Rear encounter sysmes0x72 conversion is unrecovered");
    std::string text(format.substr(0,conversion));
    text+=argument;text+=format.substr(conversion+2);
    if(text.size()>=128)throw std::runtime_error("Rear encounter banner text exceeds115b50 stack buffer");
    r.set_text(group,text_widget,text);
    //1f1b70 strlen of the argument; SLL/ADDU/SUBU with 32-bit wrap.
    const auto x=std::bit_cast<std::int32_t>(0x140u-std::uint32_t(argument.size())*5u);
    r.set_position(group,text_widget,x,keep);
}
void FieldRearBanner::traverse(RuwResource& r,UiPartySelection& party,std::span<std::uint8_t> variables,
                               unsigned game_mode,std::uint32_t& accumulator0,std::uint32_t flag) {
    //11fec8: layer flags 0x40 were set by 11f7a0; type13 dispatches 115c48.
    //Its pad reads normalize stack copies only. Controller word +e4c[e8c]:
    if(state_==0) {
        accumulator0=0;r.prepare_group_binding(group);
        r.reset_class(1,-1);                          //1ab8b0(0,1,-1,0)
        r.set_class_rgba(group,-1,{keep,keep,keep,0}); //1ab410(0,16,-1,k,k,k,0)
        party.refresh(variables,game_mode);           //121688
        state_=1;                                     //+e4c[0]++, +a8=+a4=0
    } else if(state_<0)
        throw std::runtime_error("Rear encounter banner closing states -1/-2 have no reached writer");
    //120ad0 -> row+5c 0x115bd8 with the layer payload.
    layout_enabled_=false;                            //1a5488(win,0)
    if(flag==1) {
        layout_enabled_=true;                         //1a5488(win,1)
        r.set_class_rgba(group,-1,{keep,keep,keep,128}); //1ab410(0,16,-1,k,k,k,128)
    }
    ++traversals_;
}
void FieldRearBanner::mark() {
    if(marked_)throw std::runtime_error("Rear encounter banner marked twice");
    marked_=true;
}
std::vector<TextGlyph> FieldRearBanner::glyphs(const RuwResource& r,const Font& font) const {
    std::vector<TextGlyph> result;
    if(!bound())return result;
    const auto& widget=text_owner(r);
    const auto& runtime=*widget.runtime;
    const auto alpha=std::min<std::uint32_t>(runtime.words[4]>>24,128);
    if(!alpha||!(widget.words[4]&1))return result;
    std::string_view text(runtime.text->data(),strnlen(runtime.text->data(),runtime.text->size()));
    while(text.size()>=3&&text[0]=='\\'&&(text[1]=='f'||text[1]=='s')&&text[2]=='[') {
        const auto end=text.find(']');
        if(end==text.npos)throw std::runtime_error("Unterminated rear banner control");
        text.remove_prefix(end+1);
    }
    int x=std::bit_cast<std::int32_t>(runtime.words[6])-28;
    const int y=std::bit_cast<std::int32_t>(runtime.words[7])-27;
    for(unsigned char c:text) {
        const auto* found=font.find(c);
        if(c<32||c>=127||c=='\\'||!found)throw std::runtime_error("Unrecovered rear banner character");
        const auto& m=found->metrics;
        if(c!=' ') {
            const int px=x-std::bit_cast<std::int16_t>(m[2])/16+24;
            const int py=y-(std::bit_cast<std::int16_t>(m[1])>>2)/2+36;
            result.push_back({c,2,0x00ffffffu|(alpha<<24),px,py/2,int(m[8]/16),int((m[9]>>2)/4)});
        }
        x+=c==' '?10:m[8]/16;
    }
    return result;
}
}
