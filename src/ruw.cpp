#include "okage/ruw.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
void RuwResource::set_layout_text_extent(std::size_t group,std::size_t index,
                                        std::int32_t width,std::int32_t height) {
    if(group>=widgets_.size()||index>=widgets_[group].size())
        throw std::runtime_error("RUW layout text index");
    auto& widget=widgets_[group][index];
    if(widget.words[5]!=1||!widget.runtime||!widget.runtime->text)
        throw std::runtime_error("RUW layout text owner");
    widget.runtime->words[8]=std::uint32_t(width);
    widget.runtime->words[9]=std::uint32_t(height);
}
namespace {
struct FillRounding {
    int old=std::fegetround();
    FillRounding() {
        if(old<0||(old!=FE_TOWARDZERO&&std::fesetround(FE_TOWARDZERO)))
            throw std::runtime_error("RUW fill rounding unavailable");
    }
    ~FillRounding(){if(old!=FE_TOWARDZERO)std::fesetround(old);}
};
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Truncated RUW word");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
        (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
unsigned count(Bytes b,std::size_t at) {
    const auto value=std::bit_cast<std::int16_t>(std::uint16_t(word(b,at)));
    if(value<0)throw std::runtime_error("Negative RUW count");
    return unsigned(value);
}
void copy_runtime_words(RuwWidget& widget) {
    const auto& w=widget.words;
    widget.runtime->words={w[7],w[4],w[0],w[1],w[2],w[3],w[11],w[12],w[13],w[14]};
}
void update_show(RuwWidget& widget,std::uint32_t alpha) {
    auto& flags=widget.words[4];
    if(flags&0x8000)flags=alpha?flags|1u:flags&~1u;
}
void set_color(RuwWidget& widget,std::array<std::int32_t,4> values,unsigned color) {
    if(!widget.runtime)return;
    auto& target=widget.runtime->words[4+color];
    for(unsigned c=0;c<3;++c)if(values[c]!=RuwResource::preserve_component)
        target=(target&~(255u<<(8*c)))|(std::uint32_t(values[c])<<(8*c));
    if(values[3]==RuwResource::preserve_component)return;
    auto alpha=std::uint32_t(values[3]);
    if(widget.words[4]&0x20)alpha=std::min(alpha,widget.words[2+color]>>24);
    target=(target&0xffffff)|(alpha<<24);update_show(widget,alpha);
}
std::uint32_t add_color(RuwWidget& widget,const std::array<std::int32_t,4>& delta,bool both_alpha,
                        unsigned color=0) {
    if(!widget.runtime)return 0;
    auto& w=widget.runtime->words;std::uint32_t clipped=0;
    for(unsigned c=0;c<4;++c) {
        if(delta[c]==RuwResource::preserve_component)continue;
        // EE ADDU wraps before signed comparisons, including extreme deltas.
        const auto sum=std::bit_cast<std::int32_t>(((w[4+color]>>(8*c))&255u)+std::uint32_t(delta[c]));
        std::uint32_t value;
        if(sum<0){value=0;clipped|=1u<<c;}else value=std::uint32_t(sum);
        if(c==3&&(widget.words[4]&0x20)) {
            const auto cap=widget.words[2+color]>>24;
            if(value>cap){value=cap;if(color==0)clipped|=8;}
        }
        const auto maximum=c==3?128u:255u;
        if(value>maximum){value=maximum;clipped|=1u<<c;}
        w[4+color]=(w[4+color]&~(255u<<(8*c)))|(value<<(8*c));
        if(c==3) {
            //1ab600 changes both colors;1aa938 changes only the first.
            if(both_alpha)w[5]=(w[5]&0xffffff)|(value<<24);
            update_show(widget,value);
        }
    }
    return clipped;
}
std::uint32_t add_pair(RuwWidget& widget,unsigned first,std::array<std::int32_t,2> delta,
                       std::array<std::int32_t,2> limit) {
    if(!widget.runtime)return 0;
    std::uint32_t clipped=0;
    for(unsigned axis=0;axis<2;++axis) {
        if(delta[axis]==RuwResource::preserve_component)continue;
        auto& target=widget.runtime->words[first+axis];
        target+=std::uint32_t(delta[axis]); // EE ADDU before signed comparison.
        if(limit[axis]==RuwResource::preserve_component)continue;
        const auto value=std::bit_cast<std::int32_t>(target);
        if(delta[axis]>0?value>limit[axis]:value<limit[axis]) {
            target=std::uint32_t(limit[axis]);clipped|=1u<<axis;
        }
    }
    return clipped;
}
}
RuwResource::RuwResource(Bytes b) {
    const auto styles=count(b,8),templates=count(b,12),widgets=count(b,16);
    if(!templates)throw std::runtime_error("RUW requires a template group");
    std::size_t at=20;
    std::vector<unsigned> template_counts,widget_counts;
    for(unsigned i=0;i<templates;++i,at+=4)template_counts.push_back(count(b,at));
    for(unsigned i=0;i<widgets;++i,at+=4)widget_counts.push_back(count(b,at));
    if(at>b.size()||std::uint64_t(styles)*16>b.size()-at)
        throw std::runtime_error("RUW style bounds");
    styles_.resize(styles);
    for(auto& style:styles_)for(auto& w:style){w=word(b,at);at+=4;}
    const auto read_groups=[&](const std::vector<unsigned>& counts,auto& groups) {
        for(const auto n:counts) {
            if(at>b.size()||std::uint64_t(n)*68>b.size()-at)
                throw std::runtime_error("RUW widget bounds");
            auto& group=groups.emplace_back(n);
            for(auto& widget:group)for(auto& w:widget.words){w=word(b,at);at+=4;}
        }
    };
    read_groups(template_counts,templates_);read_groups(widget_counts,widgets_);
    for(auto& group:widgets_)for(auto& widget:group) {
        auto& w=widget.words;
        if(w[5]>3)continue;
        if(w[5]==1||(w[4]&0x40)) {
            auto& state=widget.runtime.emplace();
            copy_runtime_words(widget);
            if(w[5]==1) {
                auto& text=state.text.emplace();
                // Owned ELF literal0x24a190, copied by0x1f15a8. The unused tail
                // is host-initialized; original allocation tail is unspecified.
                constexpr char initial[]="\\f[1]abcdefg";
                std::copy(std::begin(initial),std::end(initial),text.begin());
            }
        }
        // Native references live in optional<> rather than source pointer+40.
        w[16]=0;
        if(w[4]&1)w[4]|=0x8000;
    }
}
std::vector<RuwWidget>& RuwResource::mutable_group(std::size_t group) {
    if(group>=widgets_.size())throw std::runtime_error("RUW widget group bounds");
    return widgets_[group];
}
void RuwResource::prepare_group_binding(std::size_t group) {
    for(auto& widget:mutable_group(group)) {
        const auto& w=widget.words;
        if(widget.runtime&&w[9]==1&&w[8]!=0xffffffffu)
            widget.runtime->words[4]=w[w[10]?3:2];
    }
}
void RuwResource::select_class_color(std::size_t group,std::int32_t class_id,std::int32_t selection) {
    for(auto& widget:mutable_group(group))
        if(widget.runtime&&widget.words[8]==std::uint32_t(class_id))
            widget.runtime->words[4]=widget.words[widget.words[10]==std::uint32_t(selection)?2:3];
}
std::size_t RuwResource::widget_index(std::size_t group,std::int32_t id) const {
    if(group>=widgets_.size()||id<0)throw std::runtime_error("RUW authored widget ID bounds");
    const auto& records=widgets_[group];const auto key=std::uint32_t(id);
    if(key<records.size()&&records[key].words[15]==key)return key;
    for(std::size_t i=0;i<records.size();++i)if(records[i].words[15]==key)return i;
    throw std::runtime_error("RUW authored widget ID missing");
}
const RuwWidget& RuwResource::widget(std::size_t group,std::int32_t id) const {
    const auto index=widget_index(group,id);return widgets_[group][index];
}
RuwWidget& RuwResource::mutable_widget(std::size_t group,std::int32_t id) {
    const auto index=widget_index(group,id);return widgets_[group][index];
}
void RuwResource::set_rgba(std::size_t group,std::int32_t id,std::array<std::int32_t,4> values) {
    set_color(mutable_widget(group,id),values,0);
}
void RuwResource::set_secondary_rgba(std::size_t group,std::int32_t id,std::array<std::int32_t,4> values) {
    set_color(mutable_widget(group,id),values,1);
}
void RuwResource::set_class_rgba(std::size_t group,std::int32_t class_id,std::array<std::int32_t,4> values) {
    //1ab410 carries the alpha register across records: an authored cap on an
    //earlier matching widget also limits every later matching widget.
    auto alpha=std::uint32_t(values[3]);
    for(auto& widget:mutable_group(group)) {
        if(!widget.runtime||widget.words[8]!=std::uint32_t(class_id))continue;
        auto& w=widget.runtime->words;
        for(unsigned c=0;c<3;++c)if(values[c]!=preserve_component)
            w[4]=(w[4]&~(255u<<(8*c)))|(std::uint32_t(values[c])<<(8*c));
        if(values[3]==preserve_component)continue;
        if(widget.words[4]&0x20)alpha=std::min(alpha,widget.words[2]>>24);
        w[4]=(w[4]&0xffffff)|(alpha<<24);w[5]=(w[5]&0xffffff)|(alpha<<24);
        update_show(widget,alpha);
    }
}
std::uint32_t RuwResource::add_class_rgba(std::size_t group,std::int32_t class_id,std::array<std::int32_t,4> delta) {
    std::uint32_t clipped=0;
    for(auto& widget:mutable_group(group))if(widget.words[8]==std::uint32_t(class_id))
        clipped|=add_color(widget,delta,true);
    return clipped;
}
std::uint32_t RuwResource::add_rgba(std::size_t group,std::int32_t id,std::array<std::int32_t,4> delta) {
    return add_color(mutable_widget(group,id),delta,false);
}
std::uint32_t RuwResource::add_secondary_rgba(std::size_t group,std::int32_t id,std::array<std::int32_t,4> delta) {
    return add_color(mutable_widget(group,id),delta,false,1);
}
void RuwResource::reset_class(std::size_t group,std::int32_t class_id) {
    for(auto& widget:mutable_group(group)) {
        if(!widget.runtime||widget.words[8]!=std::uint32_t(class_id))continue;
        copy_runtime_words(widget);
        widget.runtime->words[4]=widget.words[widget.words[10]?3:2];
        update_show(widget,widget.runtime->words[4]>>24);
    }
}
void RuwResource::reset_widget(std::size_t group,std::int32_t id) {
    auto& widget=mutable_widget(group,id);if(!widget.runtime)return;
    copy_runtime_words(widget);update_show(widget,widget.runtime->words[4]>>24);
}
void RuwResource::set_text(std::size_t group,std::int32_t id,std::string_view text) {
    auto& widget=mutable_widget(group,id);
    if(!widget.runtime||!widget.runtime->text)return;
    if(widget.words[5]!=1)throw std::runtime_error("RUW text requires a type1 widget");
    const auto nul=text.find('\0');const auto length=nul==text.npos?text.size():nul;
    if(length>=128)throw std::runtime_error("RUW text exceeds128-byte owner");
    auto& target=*widget.runtime->text;
    if(length)std::memmove(target.data(),text.data(),length);
    target[length]=0;
}
void RuwResource::set_position(std::size_t group,std::int32_t id,std::int32_t x,std::int32_t y) {
    auto& widget=mutable_widget(group,id);if(!widget.runtime)return;
    if(x!=preserve_component)widget.runtime->words[6]=std::uint32_t(x);
    if(y!=preserve_component)widget.runtime->words[7]=std::uint32_t(y);
}
void RuwResource::set_runtime_float_pair(std::size_t group,std::int32_t id,float first,float second) {
    auto& widget=mutable_widget(group,id);if(!widget.runtime)return;
    if(first!=-3000.0f)widget.runtime->words[2]=std::bit_cast<std::uint32_t>(first);
    if(second!=-3000.0f)widget.runtime->words[3]=std::bit_cast<std::uint32_t>(second);
}
void RuwResource::set_extent(std::size_t group,std::int32_t id,std::int32_t width,std::int32_t height) {
    auto& widget=mutable_widget(group,id);if(!widget.runtime)return;
    if(width!=preserve_component)widget.runtime->words[8]=std::uint32_t(width);
    if(height!=preserve_component)widget.runtime->words[9]=std::uint32_t(height);
}
std::uint32_t RuwResource::add_extent(std::size_t group,std::int32_t id,
    std::int32_t dx,std::int32_t dy,std::int32_t lx,std::int32_t ly) {
    return add_pair(mutable_widget(group,id),8,{dx,dy},{lx,ly});
}
std::uint32_t RuwResource::add_position(std::size_t group,std::int32_t id,
    std::int32_t dx,std::int32_t dy,std::int32_t lx,std::int32_t ly) {
    return add_pair(mutable_widget(group,id),6,{dx,dy},{lx,ly});
}
std::uint32_t RuwResource::add_class_position(std::size_t group,std::int32_t class_id,
    std::int32_t dx,std::int32_t dy,std::int32_t lx,std::int32_t ly) {
    std::uint32_t clipped=0;
    for(auto& widget:mutable_group(group))if(widget.words[8]==std::uint32_t(class_id))
        clipped|=add_pair(widget,6,{dx,dy},{lx,ly});
    return clipped;
}
void RuwResource::set_runtime_word0(std::size_t group,std::int32_t id,std::uint32_t value) {
    auto& widget=mutable_widget(group,id);if(widget.runtime)widget.runtime->words[0]=value;
}
void RuwResource::set_fill_fraction(std::size_t group,std::int32_t id,float fraction) {
    auto& widget=mutable_widget(group,id);if(!widget.runtime)return;
    if(!std::isfinite(fraction)||std::fpclassify(fraction)==FP_SUBNORMAL)
        throw std::runtime_error("Unsupported RUW fill fraction");
    if(fraction>1.f)fraction=1.f;
    if(fraction<-1.f)fraction=-1.f;
    const auto capacity=std::bit_cast<std::int32_t>(widget.words[fraction<0.f?14:13]);
    FillRounding rounding;
    // Preserve separate CVT.S.W then MUL.S, including capacities beyond the
    // exactly representable integer range. UI FCSR observed 0x01000001.
    const float converted=float(capacity);
    const float product=converted*fraction;
    if(!std::isfinite(product)||product<-2147483648.f||product>=2147483648.f)
        throw std::runtime_error("RUW fill integer conversion range");
    widget.words[6]=std::uint32_t(std::int32_t(product)); // CVT.W.S toward zero.
}
ShadowScreenParameters initial_ui_projection() {
    return shadow_screen_parameters({256,std::bit_cast<float>(0x3f828f5cu),
        std::bit_cast<float>(0x3f028f5cu),2048,2048,-99,
        std::bit_cast<float>(0x4b7fff9bu),16,65536,16,65536,16,65536,320,112});
}
}
