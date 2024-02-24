#include "okage/title.hpp"
#include "okage/image_quad.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace okage;
namespace {
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
std::uint32_t bits(float v){return std::bit_cast<std::uint32_t>(v);}
float value(std::uint32_t v){return std::bit_cast<float>(v);}
struct Fixture {
    TitleTaskState t{};
    GameOverInputState input{0,{0x1000,0x2000,0x4000,0x8000},{},{}};
    GameOverPadSample pad{0,0,{128,128,0,0}};
    TitleScratch scratch{0x12345678,{11,12,13},{21,22,23}};
    std::uint32_t confirm=0x40;
    std::vector<std::string> events;
    std::vector<GameOverImageCall> images;
    std::vector<float> fades;
    std::vector<GameModeSelection> requests;
    std::vector<TitleVector> colors;
    std::vector<unsigned> sounds;
    std::array<TitleVector,2> cameras{},model{};
    TitleWaterCall created{};
    TitleUpdateServices s;
    Fixture(){
        t.common.submode=1;t.texture=91;
        model[0]={1,2,3,0x87654321};model[1]={4,5,6,0xfedcba98};
        s.common.fade=[&](float f){events.push_back("fade");fades.push_back(f);};
        s.common.request_mode=[&](GameModeSelection m){events.push_back("request");requests.push_back(m);};
        s.common.submit_image=[&](const GameOverImageCall& c){events.push_back("image"+std::to_string(c.a0));images.push_back(c);};
        s.set_camera=[&](unsigned i,const TitleVector& v){events.push_back("camera"+std::to_string(i));cameras.at(i)=v;};
        s.model_channel_base=[&]{events.push_back("model-base");return 20;};
        s.read_model_channel=[&](std::uint32_t i){events.push_back("read"+std::to_string(i));return model.at(i-20);};
        s.set_model_channel=[&](std::uint32_t i,const TitleVector& v){events.push_back("write"+std::to_string(i));model.at(i-20)=v;};
        s.create_water=[&](const TitleWaterCall& w){events.push_back("create");created=w;return 17;};
        s.rebind_water=[&](std::uint32_t id,unsigned element,std::uint32_t texture){
            events.push_back("rebind");check(id==17&&element==0&&texture==91,"Water rebind identity");};
        s.set_water_color=[&](std::uint32_t,const TitleVector& color){events.push_back("color");colors.push_back(color);};
        s.set_water_clamp=[&](std::uint32_t,unsigned clamp){events.push_back("clamp");check(clamp==2,"Water clamp mode");};
        s.stop_water=[&](std::uint32_t){events.push_back("stop");};
        s.play_sound=[&](unsigned id){events.push_back("sound"+std::to_string(id));sounds.push_back(id);};
        s.reset_game=[&]{events.push_back("reset-game");};
        s.reset_progress=[&]{events.push_back("reset-progress");};
        s.select_saved_field=[&](unsigned i){check(i==0,"Saved field selector");events.push_back("select-field");};
        s.read_saved_field=[&](unsigned i){check(i==2,"Saved field data index");events.push_back("read-field");return std::array<std::uint32_t,2>{0xffffffff,123};};
    }
    void tick(){title_update(t,input,pad,confirm,scratch,s);}
    void clear(){events.clear();images.clear();fades.clear();colors.clear();sounds.clear();}
};
void first_callback_preserves_lanes_and_source_order(){
    Fixture f;f.t.common.image_alpha_bits=bits(.5f);f.tick();
    check(f.events==std::vector<std::string>{"fade","camera0","camera1","model-base","read21","write21","read20","write20","image0","clamp","fade","create","rebind","color"},"Title constructor callback ordering");
    check(f.images[0].fp_words[5]==bits(.5f),"Background queues old alpha");
    check(f.t.common.mode_state==10&&f.t.common.tick_counter==0&&f.t.visible[0]==1,"Initial state opens Start prompt for next callback");
    check(f.cameras[0]==TitleVector{0,0,0xc2c80000,0x12345678}&&f.cameras[1]==TitleVector{0,0,0,0x12345678},"Camera scratch W is explicit");
    check(f.model[1]==TitleVector{0,bits(50),bits(200),0xfedcba98}&&f.model[0][3]==0x87654321,"Model setters preserve evaluated W");
    check(f.created.definition==106&&f.created.position==TitleVector{0x3e4ccccd,bits(6.5f),0,11}&&f.created.rotation==TitleVector{bits(.25f),0,0,12}&&f.created.scale==TitleVector{bits(6),bits(1),0x3ee66666,13},"Water authored XYZ and explicit scratch W");
    check(f.colors.back()==TitleVector{21,22,23,0},"First color preserves unknown RGB input and authors alpha zero");
    f.clear();f.tick();
    check(f.images.size()==2&&f.images[1].a0==1&&f.images[1].fp_words[1]==bits(830),"Start prompt uses its actual position on following callback");
    check(f.colors.size()==1&&f.colors[0][0]==f.colors[0][1]&&f.colors[0][1]==f.colors[0][2],"Next callback replaces complete color");
}
void start_timeout_and_signed_counter(){
    Fixture f;f.t.common.mode_state=10;f.t.common.tick_counter=1500;f.pad.pressed_word=0x800;f.tick();
    check(f.t.common.mode_state==15&&f.t.common.tick_counter==0&&f.t.alpha[0]==bits(1)&&f.sounds==std::vector<unsigned>{3},"Start resets counter before timeout comparison");
    Fixture timeout;timeout.t.common.mode_state=10;timeout.t.common.tick_counter=1499;timeout.tick();
    check(timeout.t.common.mode_state==10,"Timeout equality1500 waits");timeout.tick();
    check(timeout.t.common.mode_state==12,"Timeout above1500 enters phase12");
    Fixture wrapped;wrapped.t.common.mode_state=10;wrapped.t.common.tick_counter=0x7fffffff;wrapped.tick();
    check(wrapped.t.common.tick_counter==0x80000000&&wrapped.t.common.mode_state==10,"Timer uses signed compare after wrapping");
}
void ordinary_menu_sequence_and_navigation(){
    Fixture f;f.tick();f.pad.pressed_word=0x800;f.tick();f.pad.pressed_word=0;
    for(unsigned i=0;i<39;++i)f.tick();check(f.t.common.mode_state==15,"Prompt animation lasts40callbacks");
    f.clear();f.tick();check(f.t.common.mode_state==20&&f.t.visible[0]==0&&f.t.alpha[1]==0&&f.t.alpha[2]==0,"Prompt transition resets menu alphas after common submission");
    check(f.images.size()==2&&f.images[1].a0==1,"Transition callback still queues old prompt");
    for(unsigned i=0;i<40;++i)f.tick();
    check(f.t.common.mode_state==30&&f.t.selection==1&&f.t.pulse_channel==2&&f.t.pulse==bits(.8f),"Menu defaults to Restore after40callbacks");
    f.clear();f.pad.repeat_word=0xa000;f.tick();
    check(f.t.selection==1&&f.sounds==std::vector<unsigned>{7,7},"Simultaneous left/right is processed left first then right");
    f.clear();f.input.repeat_ticks=0;f.pad.repeat_word=0x8000;f.tick();
    check(f.t.selection==0&&f.images[f.images.size()-2].a0==5&&f.images.back().a0==4,"Newgame selection swaps authored marker order");
    f.confirm=0x20;f.pad.repeat_word=0;f.pad.pressed_word=0x40;f.tick();
    check(f.t.common.mode_state==30,"Confirm uses mutable input-owner mask");
    f.pad.pressed_word=0x20;f.tick();check(f.t.common.mode_state==50,"Authored confirm chooses selected phase");
}
void fade_and_exit_requests(){
    Fixture f;f.t.common.mode_state=12;f.t.common.image_alpha_bits=bits(.02f);f.tick();
    check(f.fades.size()==3&&f.fades[1]>.02f&&f.fades[2]<.01f&&f.requests.empty(),"Attract fade runs after common fade-in and does not request early");
    f.tick();check(f.requests.empty(),"Next common fade-in can raise alpha above request threshold");
    f.tick();check(f.requests==std::vector<GameModeSelection>{{11,0,0}},"Attract request follows source repeated prefix arithmetic");
    for(unsigned selection=0;selection<3;++selection){
        Fixture x;x.t.common.mode_state=60;x.t.selection=selection;x.t.common.image_alpha_bits=bits(.01f);x.tick();
        check(x.fades.back()==.01f&&x.t.common.image_alpha_bits==0,"Exit publishes old positive fade before clamp");
        const std::vector<std::string> tail=selection==0?std::vector<std::string>{"stop","reset-game","reset-progress","request"}:
            selection==1?std::vector<std::string>{"stop","request"}:std::vector<std::string>{"stop","select-field","read-field","request"};
        check(std::vector<std::string>(x.events.end()-tail.size(),x.events.end())==tail,"Exit services retain source ordering");
        check(x.requests==std::vector<GameModeSelection>{selection==0?GameModeSelection{9,0,0}:selection==1?GameModeSelection{10,0,0}:GameModeSelection{2,0xffffffff,123}},"Exit requests exact mode and full-width saved fields");
    }
    Fixture reset;reset.t.common.mode_state=13;reset.tick();
    check(reset.requests==std::vector<GameModeSelection>{{9,0,0}},"Phase13 retains recovered reset/newgame branch");
}
void slide_zero_boundaries_and_rounding(){
    for(auto phase:{50u,55u,57u}){
        Fixture f;f.t.common.mode_state=phase;
        unsigned i=phase==50?1:phase==55?2:6;
        f.t.x[i]=bits(phase==50?-15.f:phase==55?15.f:25.f);f.tick();
        check(f.t.common.mode_state==phase&&value(f.t.x[i])==0,"Menu slide equality zero waits");
        f.tick();check(f.t.common.mode_state==60&&f.t.common.tick_counter==0,"Menu slide crosses zero on next callback");
    }
    Fixture f;f.t.common.mode_state=99;f.t.common.generic_alpha_bits=bits(.1f);f.t.common.generic_alpha_rate_bits=bits(.2f);
    std::fenv_t saved;std::fegetenv(&saved);std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID);
    f.tick();check(f.t.common.generic_alpha_bits==0x3e999999&&std::fegetround()==FE_UPWARD&&std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID,"Prefix rounds toward zero and restores complete caller environment");
    f.s.set_camera=[](unsigned,const TitleVector&){throw std::runtime_error("stop");};
    bool caught=false;try{f.tick();}catch(const std::runtime_error&){caught=true;}
    check(caught&&std::fegetround()==FE_UPWARD,"Exception restores rounding");std::fesetenv(&saved);
    Fixture invalid;invalid.t.common.submode=4;caught=false;
    try{invalid.tick();}catch(const std::runtime_error&){caught=true;}
    check(caught&&invalid.events.empty(),"Wrong selector fails before mutation");
}
void image_descriptors_keep_authored_size_and_position(){
    const std::array<std::uint32_t,6> identity{0,0,bits(1),bits(1),0,bits(1)};
    const auto gameover=axis_aligned_image_quad(640,448,identity);
    check(gameover.xy16==std::array<std::array<std::uint32_t,2>,4>{{{27648,30976},{37888,30976},{37888,34560},{27648,34560}}},"Image descriptor reproduces independently recovered game-over packet");
    const auto title=axis_aligned_image_quad(640,488,identity);
    check(title.xy16[0][1]==30816&&title.xy16[2][1]==34720&&title.st[3]==487.5f/1024,"Title image keeps488 source rows rather than fitting448");
    auto moved=identity;moved[0]=bits(-1400);moved[1]=bits(830);moved[5]=bits(.5f);
    const auto option=axis_aligned_image_quad(112,16,moved);
    check(option.xy16[0]==std::array<std::uint32_t,2>{30472,33534}&&option.xy16[2]==std::array<std::uint32_t,2>{32264,33662}&&option.rgba==0x40808080,"Newgame uses actual112x16 descriptor and authored translation");
    moved[5]=bits(-.5f);check(axis_aligned_image_quad(16,16,moved).rgba==0xc0808080,"Source alpha packing is wrapping, not clamp-to-zero");
    moved[0]=bits(.5f);bool caught=false;
    try{axis_aligned_image_quad(16,16,moved);}catch(const std::runtime_error&){caught=true;}
    check(caught,"Unrecovered fractional transform remains explicit");
}
}
int main(){try{
    first_callback_preserves_lanes_and_source_order();start_timeout_and_signed_counter();
    ordinary_menu_sequence_and_navigation();fade_and_exit_requests();slide_zero_boundaries_and_rounding();
    image_descriptors_keep_authored_size_and_position();
    std::cout<<"Title callback tests passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
