#pragma once
#include "okage/dialogue.hpp"
#include <functional>
#include <optional>

namespace okage {
struct ComposedFrame {
    unsigned frame=0;
    bool retain_game=false;
    Texture diagnostic;
    std::optional<Texture> live;
};
struct FrameComposition {
    unsigned frame=0;
    bool retain_game=false,game_view=true,live_enabled=false,apply_fade=true;
    SceneFade fade;
    std::optional<int> choice_cursor;
    Texture dialogue{},narration{}; // Required 640x224, including transparent pixels.
    std::optional<Texture> live_dialogue; // 640x448, glyphs already shifted by (25,308).
    float dialogue_alpha=0;
    unsigned transition_alpha=0;
    std::optional<Texture> transition_diagnostic,transition_live;
    std::function<void(Texture&)> capture_pass;
    ComposedFrame compose(Texture scene) const;
};
}
