#include "okage/ruw.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Truncated RUW word");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
        (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
unsigned count(Bytes b,std::size_t at) {
    const auto value=__builtin_bit_cast(std::int16_t, (std::uint16_t(word(b,at))));
    if(value<0)throw std::runtime_error("Negative RUW count");
    return unsigned(value);
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
            state.words={w[7],w[4],w[0],w[1],w[2],w[3],w[11],w[12],w[13],w[14]};
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
ShadowScreenParameters initial_ui_projection() {
    return shadow_screen_parameters({256,__builtin_bit_cast(float, (0x3f828f5cu)),
        __builtin_bit_cast(float, (0x3f028f5cu)),2048,2048,-99,
        __builtin_bit_cast(float, (0x4b7fff9bu)),16,65536,16,65536,16,65536,320,112});
}
}
