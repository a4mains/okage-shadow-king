#include "okage/game_over.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace okage;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint32_t bits(float x){return std::bit_cast<std::uint32_t>(x);}
float value(std::uint32_t x){return std::bit_cast<float>(x);}

struct Fixture {
    GameOverTaskState task{4,0,0,0,0,0,0};
    GameOverInputState input{0,{0x1000,0x2000,0x4000,0x8000},{}, {0,0,0,0}};
    std::vector<std::string> events;
    std::vector<float> fades;
    std::vector<GameModeSelection> requests;
    unsigned draws=0;
    GameOverUpdateServices services{
        [&](float f){events.push_back("fade");fades.push_back(f);},
        [&](GameModeSelection s){events.push_back("request");requests.push_back(s);},
        [&](const GameOverImageCall& call){
            events.push_back("draw");++draws;
            check(call.a0==0&&call.fp_words==std::array<std::uint32_t,6>{0,0,0x3f800000,0x3f800000,0,0x3f800000},
                  "Exact 1b3710 register payload");
        }};
    GameOverPadSample pad{0,0,{128,128,0,0}};
    void tick(){game_over_submode4_update(task,input,pad,services);}
};

void initial_owners_are_explicit_and_prefix_runs_first(){
    Fixture f;f.task.mode_state=0;f.task.tick_counter=0;f.task.generic_alpha_bits=bits(.25f);
    f.task.generic_alpha_rate_bits=bits(.5f);f.task.image_alpha_bits=bits(.75f);
    f.tick();
    check(f.task.generic_alpha_bits==bits(.75f)&&f.input.analog_words==std::array<std::int32_t,4>{0,0,0,0},
          "Generic prefix consumes live alpha-rate and pad bytes");
    check(f.fades.size()==2&&f.fades[0]==.75f&&f.fades[1]==.75f,
          "Generic fade precedes mode fade with distinct source calls");
    check(f.events==std::vector<std::string>({"fade","fade","draw"})&&f.draws==1,
          "Prefix fade, mode fade, then image submission ordering");
    check(f.task.pressed_word==0,"1c0160 sample is stored before mode dispatch");
}

void strict_state_boundaries(){
    Fixture f;f.task.mode_state=0;f.task.tick_counter=59;f.tick();
    check(f.task.mode_state==0&&f.task.tick_counter==60,"State0 equality60 waits");
    f.tick();check(f.task.mode_state==10&&f.task.tick_counter==0,"State0 transitions only above60 and resets");

    Fixture input_edge;input_edge.task.mode_state=10;input_edge.task.tick_counter=119;
    input_edge.task.image_alpha_bits=bits(1.f);input_edge.pad.pressed_word=0x40;
    input_edge.tick();check(input_edge.task.mode_state==10&&input_edge.task.tick_counter==120,
                            "Input exit gate equality120 waits");
    input_edge.tick();check(input_edge.task.mode_state==0x14&&input_edge.task.tick_counter==121,
                            "Input exit is tested strictly above120");

    Fixture timeout;timeout.task.mode_state=10;timeout.task.tick_counter=719;
    timeout.task.image_alpha_bits=bits(1.f);timeout.tick();
    check(timeout.task.mode_state==10&&timeout.task.tick_counter==720,"Timeout equality720 waits");
    timeout.tick();check(timeout.task.mode_state==0x14&&timeout.task.tick_counter==721,
                         "Automatic transition occurs strictly above720");
    Fixture wrapped;wrapped.task.tick_counter=0x7fffffffu;wrapped.tick();
    check(wrapped.task.tick_counter==0x80000000u&&wrapped.task.mode_state==0,
          "Signed counter overflow does not become a large positive timeout");
    wrapped.task.mode_state=10;wrapped.task.image_alpha_bits=bits(1.f);wrapped.pad.pressed_word=0x40;
    wrapped.tick();check(wrapped.task.mode_state==10,"Negative counter does not open input or timeout gates");
}

void input_and_alpha_order(){
    Fixture f;f.task.mode_state=10;f.task.tick_counter=120;f.task.image_alpha_bits=bits(0.f);
    f.pad.repeat_word=0x40;f.pad.pressed_word=0x40;
    f.tick();
    check(f.task.mode_state==0x14&&f.task.image_alpha_bits==0x3b30f27cu,
          "Fade-in update precedes the input transition and uses authored step");
    check(f.task.pressed_word==0x40,"Mode input uses the distinct 1c0160 halfword");

    Fixture no_input;no_input.task.mode_state=10;no_input.task.tick_counter=120;
    no_input.task.image_alpha_bits=bits(1.f);no_input.pad.repeat_word=0x40;
    no_input.tick();check(no_input.task.mode_state==10,"Generic repeat halfword does not substitute for pressed word");

    Fixture analog;analog.task.generic_alpha_bits=bits(.5f);analog.task.generic_alpha_rate_bits=bits(.75f);
    analog.pad.analog_bytes={228,28,17,255};analog.tick();
    check(analog.input.analog_words==std::array<std::int32_t,4>{100,-100,17,255},
          "Generic prefix preserves centered axes and remaining raw bytes");
    check(analog.input.direction_flags==std::array<std::uint32_t,4>{0,0,0,0},
          "Analog threshold comparisons are strict at plus/minus100");
    check(analog.input.repeat_ticks==0,"No direction leaves repeat counter untouched");

    Fixture clamped;clamped.task.generic_alpha_bits=bits(.75f);clamped.task.generic_alpha_rate_bits=bits(.5f);
    clamped.task.image_alpha_bits=bits(1.f);clamped.tick();
    check(clamped.task.generic_alpha_bits==bits(1.f),"Only generic alpha is clamped to1");
}

void fade_out_and_request_order(){
    Fixture f;f.task.mode_state=0x14;f.task.tick_counter=10;f.task.image_alpha_bits=bits(.005f);
    f.tick();
    check(f.task.image_alpha_bits==bits(-.005f),
          "Fade-out subtracts without clamping");
    check(f.fades.size()==2&&f.fades[1]==value(f.task.image_alpha_bits)&&f.fades[1]<0,
          "Mode fade service receives raw negative overshoot");
    check(f.requests.empty()&&f.events==std::vector<std::string>({"fade","fade","draw"}),
          "Positive pre-update alpha subtracts and still draws");

    Fixture request;request.task.mode_state=0x14;request.task.image_alpha_bits=0;
    request.tick();
    check(request.requests==std::vector<GameModeSelection>{{8,1,0}},"Zero alpha requests authored next mode");
    check(request.events==std::vector<std::string>({"fade","request","fade","draw"}),
          "Parent request occurs between generic fade and image fade/draw");

    Fixture overshoot;overshoot.task.mode_state=10;overshoot.task.image_alpha_bits=0x3f7fffffu;
    overshoot.task.tick_counter=10;overshoot.tick();
    check(value(overshoot.task.image_alpha_bits)>1.f&&overshoot.fades[1]>1.f,
          "Mode fade-in alpha is source-unclamped and forwarded raw");
}

void repeat_and_direction_flags(){
    Fixture f;f.pad.repeat_word=0x1000;f.pad.analog_bytes={128,128,0,0};f.tick();
    check(f.input.direction_flags[0]==1&&f.input.repeat_ticks==8,"Mapped button sets its authored direction and repeat8");
    f.pad.repeat_word=0;f.pad.analog_bytes={128,28,0,0};f.tick();
    check(f.input.repeat_ticks==7&&f.input.direction_flags==std::array<std::uint32_t,4>{0,0,0,0},
          "Active repeat decrements and suppresses fresh direction flags");
    f.pad.analog_bytes={128,29,0,0};f.tick();
    check(f.input.repeat_ticks==0,"Centered Y in reset band clears repeat when X is in raw source interval");
}

void callbacks_keep_source_partial_writes_and_rounding(){
    Fixture f;f.task.mode_state=0;f.task.generic_alpha_bits=bits(.1f);f.task.generic_alpha_rate_bits=bits(.2f);
    std::fenv_t saved;check(std::fegetenv(&saved)==0,"save fenv");
    check(std::fesetround(FE_UPWARD)==0,"set host rounding");std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID);
    f.tick();
    check(std::fegetround()==FE_UPWARD&&std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID,
          "Helper restores full floating environment");std::fesetenv(&saved);
    check(f.task.generic_alpha_bits==0x3e999999u,"Generic ADD.S uses toward-zero, not caller upward mode");

    Fixture throwing;throwing.task.mode_state=0x14;throwing.task.image_alpha_bits=0;
    throwing.services.request_mode=[](GameModeSelection){throw std::runtime_error("request");};
    bool caught=false;try{throwing.tick();}catch(const std::runtime_error&){caught=true;}
    check(caught&&throwing.events==std::vector<std::string>({"fade"})&&throwing.draws==0,
          "Throwing request preserves earlier generic fade and prevents later mode fade/draw");

    Fixture throw_fade;throw_fade.services.fade=[](float){throw std::runtime_error("fade");};
    check(std::fesetround(FE_DOWNWARD)==0,"set rounding before throwing callback");
    caught=false;try{throw_fade.tick();}catch(const std::runtime_error&){caught=true;}
    check(caught&&std::fegetround()==FE_DOWNWARD,"Floating mode restores when an external service throws");
    std::fesetenv(&saved);

    Fixture no_draw;no_draw.task.mode_state=0;no_draw.services.submit_image={};
    caught=false;try{no_draw.tick();}catch(const std::bad_function_call&){caught=true;}
    check(caught&&no_draw.events==std::vector<std::string>({"fade","fade"}),
          "Missing draw service fails at its source call after both fades");
}

void unsupported_dispatch_is_not_silently_routed(){
    Fixture f;f.task.submode=3;const auto before=f.task;bool caught=false;
    try{f.tick();}catch(const std::runtime_error&){caught=true;}
    check(caught&&f.task.mode_state==before.mode_state&&f.task.tick_counter==before.tick_counter&&f.events.empty(),
          "Submodes other than4 are owned by separate implementations");
}
}

int main(){try{
    initial_owners_are_explicit_and_prefix_runs_first();strict_state_boundaries();input_and_alpha_order();
    fade_out_and_request_order();repeat_and_direction_flags();callbacks_keep_source_partial_writes_and_rounding();
    unsupported_dispatch_is_not_silently_routed();
    std::cout<<"Game-over update tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
