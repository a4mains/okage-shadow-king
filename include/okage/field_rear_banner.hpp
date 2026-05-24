#pragma once
#include "okage/battle_interface.hpp"
#include "okage/battle_ui.hpp"
#include "okage/font.hpp"
#include "okage/ruw.hpp"
#include "okage/text.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace okage {
class FieldRearBannerData {
public:
    explicit FieldRearBannerData(Bytes);
    const std::array<std::uint32_t,28>& row() const {return row_;}
    std::span<const UiWindow::AtlasTuple> atlas() const {return atlas_;}
private:
    std::array<std::uint32_t,28> row_{};
    std::vector<UiWindow::AtlasTuple> atlas_;
};

class FieldRearBanner {
public:
    static constexpr std::size_t group=16;
    static constexpr std::int32_t text_widget=1;
    static constexpr std::string_view argument="surprise attack"; // ELF 0x2284a0
    FieldRearBanner(BattleUiShared&,std::shared_ptr<UiWindowStream> layer_stream,
                    const FieldRearBannerData&,std::shared_ptr<RuwResource>,
                    std::shared_ptr<const void> texture,std::string_view system_message_72,
                    std::uint32_t& flag);
    // 0x120b48 -> 0x199458: the window leaves the registry (if still there).
    ~FieldRearBanner();
    FieldRearBanner(const FieldRearBanner&)=delete;
    FieldRearBanner& operator=(const FieldRearBanner&)=delete;
    void traverse(UiPartySelection&,std::span<std::uint8_t> variables,
                  unsigned game_mode,std::uint32_t flag);
    // Phase10 (0x1340ac..0x1340c4): +1c |= 0x80000000.
    void mark();
    bool marked() const {return marked_;}
    // 0x115c48 controller word +e4c[0]: 0 before the first traversal, then 1.
    std::int32_t controller_state() const {return state_;}
    bool bound() const {return state_>0;}
    // Window +1900 bit0x1000 as last written by 1a5488 (row word20=1 at build).
    bool layout_enabled() const;
    unsigned traversals() const {return traversals_;}
    unsigned window_slot() const {return slot_;}
    const UiWindow& window() const;
    std::vector<TextGlyph> glyphs(const Font&) const;
private:
    UiWindow& live_window() const;
    BattleUiShared& ui_;
    std::shared_ptr<RuwResource> ruw_;
    unsigned slot_=0;
    const UiWindow* window_=nullptr;
    std::int32_t state_=0;
    bool marked_=false;
    unsigned traversals_=0;
};
}
