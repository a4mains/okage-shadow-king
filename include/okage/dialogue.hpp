#pragma once
#include "okage/text.hpp"
#include <functional>
#include <optional>

namespace okage {
struct DialogueButtons {std::uint16_t held=0,pressed=0,repeat=0;};
// Digital repeat from 0x1bf7c8; the NTSC setup uses 60*40 and 60*8.
struct DialoguePadRepeat {
    std::uint16_t previous=0,ticks=0,delay=2400,interval=480;
    std::uint16_t update(std::uint16_t held,std::uint16_t pressed);
};
struct DialogueInputEffects {
    unsigned resume_calls=0;
    bool accelerated=false,stop_voice=false,stop_effect=false,close=false;
    bool reset_text_clock=false;
    int accepted_choice=-1;
    int cue=-1;
    std::optional<float> scene_fade;
};
// Input/progression portion of 0x148dc8 case 5. Rendering, choice dispatch,
// character-sound generation and sound/effect handles are separate services.
struct DialogueController {
    std::uint32_t layer_flags=0x50,advance_count=0,layer_advance=0;
    std::int32_t normal_period=1,window_type=0;
    bool automatic=false;
    bool choice_active=false,choice_armed=false;
    unsigned stage=5;
    std::function<void(unsigned,unsigned,std::int32_t)> set_variable;
    std::function<std::int32_t(unsigned,unsigned)> get_variable;
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
// Choice text positions/opacity from 0x146ee8. The runtime's flat diagnostic
// highlight uses widget 3's recovered rectangle; full RUW styling is separate.
struct ChoiceMenu {
    std::array<std::string,3> lines;
    unsigned alpha=0,accepted=0;
    int cursor=0;
    bool highlighted=false;
    void update(std::string_view replacement,int selection,bool armed);
    std::vector<TextGlyph> glyphs(const Font&) const;
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
struct DialogueOpening {
    unsigned decoration_ticks=0;
    void initialize(DialogueText&,TextClock&,DialogueController&,DialogueOpacity&,DialogueClosure&);
    void bootstrap(DialogueText&,TextClock&);
    // Returns true if this tick handled an opening stage rather than input.
    bool update(DialogueText&,TextClock&,DialogueController&,DialogueOpacity&,DialogueClosure&);
};
}
