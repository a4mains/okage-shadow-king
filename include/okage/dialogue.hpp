#pragma once
#include "okage/text.hpp"
#include <functional>
#include <optional>

namespace okage {
struct DialogueButtons {std::uint16_t held=0,pressed=0;};
struct DialogueInputEffects {
    unsigned resume_calls=0;
    bool accelerated=false,stop_voice=false,stop_effect=false,close=false;
    int cue=-1;
    std::optional<float> scene_fade;
};
// Input/progression portion of 0x148dc8 case 5. Rendering, choice dispatch,
// character-sound generation and sound/effect handles are separate services.
struct DialogueController {
    std::uint32_t layer_flags=0x50,advance_count=0,layer_advance=0;
    std::int32_t normal_period=1,window_type=0;
    bool automatic=false;
    unsigned stage=5;
    std::function<void(unsigned,unsigned,std::int32_t)> set_variable;
    DialogueInputEffects update(DialogueText& text,DialogueButtons buttons);
};
// Original 0x114c20/0x114cc0/0x114b58 scene-darkening layer. Initial state
// precedes startup; a diagnostic entering an already lit scene must say so.
struct SceneFade {
    float current=0,target=0,step=.02f;
    bool enabled=false,hold=false;
    void request(float);
    void tick();
    unsigned alpha() const;
};
// Three description widgets from 0x146ee8, using the USA sysmes[0x106]
// font-2 style. Diagnostic rasterization omits GS filtering/subpixel coverage.
struct Narration {
    struct Line {std::string text;unsigned ticks=0,alpha=0;int width=0,x=0;};
    std::array<Line,3> lines;
    void update(std::string_view description,const Font& font);
    std::vector<TextGlyph> glyphs(const Font& font) const;
};
struct DialogueOpacity {
    float value=128,upper=128,lower=0,delta=0;
    void set(float initial,float step,float minimum=0,float maximum=128);
    void tick(); // Original 0x1998a8; strict comparisons at the end points.
    bool visible() const {return value!=0 || delta!=0;}
};
struct DialogueClosure {
    DialogueOpacity decoration;
    std::uint32_t message_flags=0;
    void update(unsigned& shared_stage); // Call after both layers' opacity ticks.
    bool finished() const {return (message_flags&0x80000000u)!=0;}
};
}
