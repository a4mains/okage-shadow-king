#pragma once
#include "okage/battle_ui.hpp"
#include "okage/font.hpp"
#include "okage/ruw.hpp"
#include "okage/text.hpp"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace okage {
class FieldRearBanner {
public:
    static constexpr std::size_t group=16;
    static constexpr std::int32_t text_widget=1;
    static constexpr std::string_view argument="surprise attack"; // ELF 0x2284a0
    // 0x11f5f8 -> 11f668 -> 11f7a0 -> row+58 0x115b20 (flag=0), then the
    // phase-11 caller's 0x115b50(0x2284a0): flag=1, sysmes 0x72 %s format,
    // 1aa2b8(0,16,1,text), 1a9e88(0,16,1,0x140-5*strlen,keep).
    FieldRearBanner(RuwResource&,std::string_view system_message_72,std::uint32_t& flag);
    void traverse(RuwResource&,UiPartySelection&,std::span<std::uint8_t> variables,
                  unsigned game_mode,std::uint32_t& resource_accumulator0,std::uint32_t flag);
    // Phase10 (0x1340ac..0x1340c4): +1c |= 0x80000000.
    void mark();
    bool marked() const {return marked_;}
    // 0x115c48 controller word +e4c[0]: 0 before the first traversal, then 1.
    std::int32_t controller_state() const {return state_;}
    bool bound() const {return state_>0;}
    // Window +1900 bit0x1000 as last written by 1a5488 (row word20=1 at build).
    bool layout_enabled() const {return layout_enabled_;}
    unsigned traversals() const {return traversals_;}
    std::vector<TextGlyph> glyphs(const RuwResource&,const Font&) const;
private:
    std::int32_t state_=0;
    bool marked_=false,layout_enabled_=true;
    unsigned traversals_=0;
};
}
