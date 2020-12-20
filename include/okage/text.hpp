#pragma once
#include "okage/assets.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace okage {
struct MessageNode { std::uint32_t depth, string_index; };
struct MessageBank {
    std::vector<std::string> strings;
    std::vector<MessageNode> nodes;
    // Original message-ID lookup and positional fallback (0x19a150).
    std::vector<std::uint32_t> message_indices(std::int32_t id) const;
    std::vector<std::string> message(std::int32_t id) const;
};
MessageBank read_pme(Bytes bytes);

// Logical text state recovered from 0x19d9a0. No font metrics, drawing or clock.
struct TextState {
    std::uint32_t flags=0, progress=0, color=0x80808080, previous_color=0;
    std::uint32_t x=0, y=0, line_origin=0, line_height=24, font=0;
    std::uint32_t period=2, phase=0, delay=0;
    std::string speaker, window, option, replacement, description;
};
unsigned consume_text_control(std::string_view text, std::size_t& cursor, TextState& state);

enum class TextStep { glyph, pause, end };
class DialogueText {
    std::vector<std::string> fragments_;
    std::size_t fragment_=0, cursor_=0;
public:
    explicit DialogueText(std::vector<std::string> fragments):fragments_(std::move(fragments)) {}
    TextState state;
    std::string page;
    TextStep step();
    void resume(); // Original 0x1985c0; caller owns input/readiness/timing policy.
};
}
