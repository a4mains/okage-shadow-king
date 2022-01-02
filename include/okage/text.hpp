#pragma once
#include "okage/font.hpp"
#include "okage/assets.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace okage {
struct MessageNode { std::uint32_t depth, string_index; };
struct MessagePath {
    std::uint32_t root=0;
    std::vector<std::uint32_t> strings;
    std::array<std::uint32_t,257> nodes{};
};
struct MessageBank {
    std::vector<std::string> strings;
    std::vector<MessageNode> nodes;
    // Original message-ID lookup and positional fallback (0x19a150).
    std::vector<std::uint32_t> message_indices(std::int32_t id) const;
    std::vector<std::string> message(std::int32_t id) const;
    MessagePath message_path(std::int32_t id) const;
    // True when a multi-child branch rewrites the remaining fragment stream.
    bool select_branch(MessagePath&,std::size_t fragment,unsigned selection) const;
    unsigned branch_count(const MessagePath&,std::size_t fragment) const;
    // Original 0x19a5c8 replaces the first matching tagged system child.
    // Shared string indices make the binding visible to every referring message.
    bool bind(std::string_view tag,std::string_view value);
};
MessageBank read_pme(Bytes bytes);

// Logical text state recovered from 0x19d9a0. No font metrics or drawing.
struct TextState {
    std::uint32_t flags=0, progress=0, color=0x80808080, previous_color=0;
    std::uint32_t x=0, y=0, line_origin=0, line_height=24, font=0;
    std::uint32_t period=2, phase=0, delay=0;
    std::string speaker, window, option, replacement, description;
};
unsigned consume_text_control(std::string_view text, std::size_t& cursor, TextState& state);

struct TextClock {
    std::uint8_t display_modes=0x10;
    std::uint32_t revealed=0;
    std::int32_t scroll_target=0;
    float scroll=0, scroll_step=1, scroll_base_step=1;
    std::uint32_t glyph_flags=0; // Object +0x4f8; bit 0x20 makes spaces consume a tick.
};
// Text pacing portion of 0x1980f8, excluding window animation/color/opacity.
void tick_text_clock(TextState& state, TextClock& clock);
struct TextPosition { std::size_t fragment=0, cursor=0; bool loaded=false; };
struct TextGlyph {
    std::uint32_t code=0,font=0,color=0;
    std::int32_t x=0,y=0,width=0,height=0;
};

enum class TextStep { glyph, pause, end };
class DialogueText {
    std::vector<std::string> fragments_;
    const MessageBank* bank_=nullptr;
    MessagePath path_;
    std::size_t fragment_=0, cursor_=0;
    bool loaded_=false;
public:
    explicit DialogueText(std::vector<std::string> fragments):fragments_(std::move(fragments)) {}
    // Bank must outlive the dialogue, including any branch selections.
    DialogueText(const MessageBank& bank,std::int32_t id);
    DialogueText(MessageBank&&,std::int32_t)=delete;
    TextState state;
    std::string page;
    // Caller owns fonts in retail handle order. Empty retains cursor-only mode.
    std::span<const Font> fonts;
    std::vector<TextGlyph> glyphs;
    int choice_cursor=0,choice_result=0;
    void move_choice(int delta);
    void set_option_cursor(int selection); // 0x19b800 examines the preceding path slot.
    bool select_choice(unsigned selection); // 0x19b448; true resets the text clock.
    const MessagePath& path() const {return path_;}
    TextStep step();
    void resume(); // Original 0x1985c0; caller owns input/readiness/timing policy.
    TextPosition position() const {return {fragment_,cursor_,loaded_};}
    void seek(TextPosition position);
    void frame(TextClock& clock, bool visible=true);
    // Normal dialogue confirmation from the case-5 path in 0x148dc8. Choice,
    // window-command and controller-poll policy remain the caller's responsibility.
    void confirm(std::int32_t normal_period);
};
Texture rasterize_text(std::span<const Font> fonts,std::span<const TextGlyph> glyphs,
                       unsigned width,unsigned height);
}
