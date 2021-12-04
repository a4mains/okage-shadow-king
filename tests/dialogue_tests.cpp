#include "okage/dialogue.hpp"
#include <cfenv>
#include <iostream>
#include <stdexcept>

void require(bool value) {if(!value)throw std::runtime_error("Dialogue test failed");}
template<class F> void rejects(F f) {try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected dialogue rejection");}
int main() {try {
    {
        okage::Font font;font.first=32;font.last=126;font.glyphs.resize(95);
        for(auto& glyph:font.glyphs)glyph.metrics[8]=32;
        okage::Narration narration;
        const std::string raw="############ arrived.\\nHello, ############!\\n############";
        narration.update(raw,font,"Ari");
        require(narration.lines[0].text=="Ari arrived." && narration.lines[1].text=="Hello, Ari!" && narration.lines[2].text=="Ari");
        require(narration.lines[0].source=="############ arrived." && raw.starts_with("############"));
        narration.update(raw,font,"Ari");narration.update(raw,font,"Ari");
        require(narration.lines[0].ticks==11 && narration.lines[0].alpha==10);
        narration.update(raw,font,"Z");
        require(narration.lines[0].text=="Z arrived." && narration.lines[0].ticks==11 && narration.lines[0].alpha==18);
        narration.update("############",font,"ABCDEFGHIJKL");
        require(narration.lines[0].text=="ABCDEFGHIJKL" && narration.lines[0].ticks==1 && narration.lines[0].alpha==2);
        require(narration.lines[1].alpha==10);
        rejects([&]{narration.update("########################",font,"Ari");});
        okage::ChoiceMenu menu;
        menu.update("Ask ############\\n\\nLeave",1,true,"Ari");
        require(menu.lines==std::array<std::string,3>{"Ask Ari"," ","Leave"} && menu.cursor==1 && menu.highlighted);
        menu.accepted=1;menu.update("New\\nNew\\nNew",0,false,"Ari");
        require(menu.lines[0]=="Ask Ari");
    }
    okage::DialogueText text({"Hello\\v"});text.state.flags=0x8001;
    okage::DialogueController c;
    auto effect=c.update(text,{0x40,0});require(!effect.accelerated && text.state.period==2);
    effect=c.update(text,{0x40,0x40});require(effect.accelerated && text.state.period==0xfffffffc);
    text.state.flags|=0x910;text.state.delay=0;text.state.period=1;
    effect=c.update(text,{0x40,0x40});require(effect.resume_calls==2 && c.advance_count==2 && c.layer_advance==2);
    require(text.state.period==1 && !(text.state.flags&0x910));
    text.state.delay=3;effect=c.update(text,{0x40,0x40});require(!effect.resume_calls && !effect.accelerated && effect.stop_voice);
    text.state.delay=0;c.layer_flags&=~0x10u;text.state.flags|=0x910;
    effect=c.update(text,{0x40,0x40});require(!effect.resume_calls && c.advance_count==2);
    c.layer_flags|=0x10;effect=c.update(text,{0x44,0x44});require(effect.resume_calls==2); // Paused branch reads raw confirm.
    text.state.period=1;effect=c.update(text,{0x44,0x44});require(!effect.accelerated);
    text.state.window="WINTYPE(0)";text.state.flags|=0x910;
    effect=c.update(text,{});require(effect.resume_calls==1 && effect.cue==11 && c.layer_flags==0x70);
    effect=c.update(text,{});require(effect.resume_calls==0 && effect.cue==-1);
    text.state.window="WINTYPE(1)";effect=c.update(text,{});require(c.window_type==1 && c.layer_flags==0x50);
    text.state.window.clear();text.state.flags|=2;
    effect=c.update(text,{});require(effect.close && c.stage==6 && effect.cue==-1);
    rejects([&]{c.update(text,{});});
    c.stage=5;text.state.flags&=~2u;text.state.window="WINTYPE(2)";
    effect=c.update(text,{});require(c.window_type==2 && effect.scene_fade==.5f && effect.resume_calls==1 && c.layer_flags==0x50);
    text.state.window="SET(2/0/1)";text.state.delay=240;
    rejects([&]{c.update(text,{});});require(text.state.window=="SET(2/0/1)");
    unsigned writes=0;c.set_variable=[&](unsigned index,unsigned bank,std::int32_t value){require(index==2 && bank==0 && value==1);++writes;};
    effect=c.update(text,{});require(writes==1 && !effect.resume_calls && text.state.delay==240);
    c.update(text,{});require(writes==1);
    text.state.window="FADE(100)";effect=c.update(text,{});require(effect.scene_fade==1 && effect.resume_calls==1);
    text.state.window="SET(2/0/)";rejects([&]{c.update(text,{});});
    text.state.window="WINTYPE(3)";rejects([&]{c.update(text,{});});
    {
        okage::DialogueText t({""});okage::DialogueController control;
        t.state.flags=0x8910;t.state.delay=7;t.state.phase=9;
        t.state.period=0xfffffffe;t.state.window="SHADOW(0)";
        rejects([&]{control.update(t,{});});
        require(t.state.window=="SHADOW(0)"&&t.state.delay==7);
        std::vector<std::uint32_t> requests;
        control.shadow_request=[&](auto value){requests.push_back(value);};
        auto result=control.update(t,{});
        require(requests==std::vector<std::uint32_t>{0}&&result.resume_calls==0&&result.cue==-1);
        require(t.state.flags==0x8910&&t.state.delay==7&&t.state.phase==9&&t.state.period==0xfffffffe);
        control.update(t,{});require(requests.size()==1);
        t.state.window="SHADOW(-1)";control.update(t,{});require(requests.back()==0xffffffff);
        t.state.window="SHADOW(256)";control.update(t,{});require(requests.back()==256);
        for(auto command:{"SHADOW()","SHADOW(0/1)","SHADOW(x)","SHADOW(2147483648)"}) {
            t.state.window=command;rejects([&]{control.update(t,{});});
        }
        require(requests.size()==3);
        std::vector<std::array<int,3>> writes;
        control.set_variable=[&](auto index,auto bank,auto value){writes.push_back({int(index),int(bank),value});};
        t.state.window="SET(35/1/1)SET(10/4/1)";
        result=control.update(t,{});
        require(writes==std::vector<std::array<int,3>>{{35,1,1},{10,4,1}});
        require(!result.resume_calls&&t.state.delay==7);
        std::vector<std::array<int,2>> motions;
        t.state.window="CHANI(-27/1)";rejects([&]{control.update(t,{});});
        require(t.state.window=="CHANI(-27/1)"&&t.state.delay==7);
        control.actor_motion_request=[&](int alias,int entry){motions.push_back({alias,entry});};
        result=control.update(t,{});
        require(motions==std::vector<std::array<int,2>>{{-27,1}}&&!result.resume_calls&&t.state.delay==7);
        control.update(t,{});require(motions.size()==1);
        t.state.window="CHANI(8/2048)";control.update(t,{});require(motions.back()==std::array<int,2>{8,2048});
        for(auto command:{"CHANI()","CHANI(-27)","CHANI(-27/1/0)","CHANI(-27/x)"}) {
            t.state.window=command;rejects([&]{control.update(t,{});});
        }
        t.state.window="WINTYPE(1)CHANI(-27/1)SHADOW(0)";
        result=control.update(t,{});require(result.resume_calls==1&&t.state.delay==0);
        t.state.delay=7;t.state.window="WINTYPE(0)SET(35/1/1)SHADOW(0)";
        result=control.update(t,{});require(!result.resume_calls&&t.state.delay==7&&result.cue==-1);
        t.state.window="SET(35/1/1)WINTYPE(0)";
        result=control.update(t,{});require(result.resume_calls==1&&result.cue==11&&t.state.delay==0);
    }
    text.state.window.clear();text.state.option="Choice";rejects([&]{c.update(text,{});});
    okage::DialogueController relation;okage::DialogueText relation_text({""});
    relation_text.state.window="LA(0/3/0)";relation_text.choice_result=1;
    rejects([&]{relation.update(relation_text,{});});
    std::array<std::int32_t,6> relationship_values={-2,0,0x7fffffff,1,2,3};unsigned relationship_writes=0;
    relation.get_variable=[&](unsigned index,unsigned bank){require(bank==4&&index>=16&&index<22);return relationship_values[index-16];};
    relation.set_variable=[&](unsigned index,unsigned bank,std::int32_t value){require(bank==4&&index>=16&&index<22);relationship_values[index-16]=value;++relationship_writes;};
    relation_text.state.flags=0x8910;relation_text.state.delay=7;relation_text.state.phase=9;
    const auto relationship_effect=relation.update(relation_text,{});
    require(relationship_values==std::array<std::int32_t,6>{0,0,0,1,2,3});
    require(relationship_effect.resume_calls==1&&relationship_effect.cue==-1&&relation.layer_flags==0x50);
    require(relation_text.state.delay==0&&relation_text.state.phase==0&&relation_text.state.period==1);
    relation.update(relation_text,{});require(relationship_writes==6);
    for(const auto* command:{"LA(0/13/0)","LA(0/-1/0)","LA(0/3)","LA(0/3/)"}) {
        relation_text.state.window=command;rejects([&]{relation.update(relation_text,{});});
        require(relationship_writes==6);
    }
    relation_text.state.window="LA(0/3/0)";relation_text.choice_result=3;
    rejects([&]{relation.update(relation_text,{});});require(relationship_writes==6);
    okage::MessageBank bank;
    bank.strings={"system","42","\\z[One\\nTwo\\nThree]\\o[CC]\\v","A\\e","B\\e","C\\e","After\\v"};
    bank.nodes={{0,0},{0,1},{1,2},{2,3},{2,4},{2,5},{1,6}};
    {
        auto conditional_bank=bank;
        conditional_bank.strings[2]="\\o[CMP(0/4/-1)=]\\v";
        okage::DialogueText conditional(conditional_bank,42);okage::DialogueController controller;
        conditional.state.flags=0x8001;conditional.step();
        rejects([&]{controller.update(conditional,{});});
        unsigned reads=0;
        controller.get_variable=[&](unsigned index,unsigned storage){require(index==0&&storage==4);++reads;return -1;};
        conditional.state.delay=3;const auto waiting=controller.update(conditional,{});
        require(reads==0&&!waiting.resume_calls);
        conditional.state.delay=0;const auto chosen=controller.update(conditional,{});
        require(reads==1&&chosen.reset_text_clock&&chosen.resume_calls==1&&chosen.accepted_choice==-1);
        require(controller.advance_count==0&&!controller.choice_active&&conditional.choice_result==0);
        require(conditional.step()==okage::TextStep::glyph&&conditional.page=="A");
        for(const auto* option:{"CMP(0/4/-1)<=","CMP(0/4/-1)","CMP(0/4)=","CMP(-1/4/0)=","CMP(0/4/0="}) {
            okage::DialogueText malformed(conditional_bank,42);malformed.state.flags=0x8001;malformed.step();
            malformed.state.option=option;rejects([&]{controller.update(malformed,{});});
        }
    }
    for(unsigned selected=0;selected<3;++selected){
        okage::DialogueText choice(bank,42);okage::DialogueController controller;
        choice.state.flags=0x8001;choice.state.progress=9;
        require(choice.step()==okage::TextStep::pause);
        auto e=controller.update(choice,{0x40,0x40,0x40});
        require(e.resume_calls==0 && !controller.choice_armed && controller.choice_active);
        e=controller.update(choice,{0x4000,0x4000,0x4000});
        require(controller.choice_armed && choice.choice_cursor==0 && !e.resume_calls);
        for(unsigned i=0;i<selected;++i)controller.update(choice,{0x4000,0x4000,0x4000});
        e=controller.update(choice,{0x40,0x40,0x40});
        require(e.resume_calls==1 && e.reset_text_clock && e.accepted_choice==int(selected));
        require(controller.advance_count==1 && controller.layer_advance==1 && !controller.choice_armed);
        require(choice.choice_result==int(selected) && choice.choice_cursor==0 && choice.state.progress==9);
        require(choice.position().fragment==0 && !choice.position().loaded);
        require(choice.step()==okage::TextStep::glyph && choice.page==std::string(1,'A'+selected));
    }
    okage::DialogueText wrap(bank,42);wrap.step();wrap.move_choice(-1);require(wrap.choice_cursor==2);
    wrap.move_choice(1);require(wrap.choice_cursor==0);
    okage::DialogueController gated;
    gated.update(wrap,{0x4040,0x4040,0x4040});require(!gated.choice_armed); // Confirm filters direction.
    gated.update(wrap,{0x5000,0x5000,0x5000});require(gated.choice_armed && wrap.choice_cursor==0);
    gated.update(wrap,{0x5000,0x5000,0x5000});require(wrap.choice_cursor==1); // Down wins conflict.
    gated.layer_flags&=~0x10u;require(!gated.update(wrap,{0x40,0x40,0x40}).resume_calls);
    okage::DialoguePadRepeat pad;require(pad.update(0x1000,0x1000)==0x1000);
    for(unsigned i=0;i<24;++i)require(pad.update(0x1000,0)==0);
    require(pad.update(0x1000,0)==0x1000 && pad.ticks==2020);
    require(pad.update(0,0)==0 && pad.ticks==0 && pad.previous==0);
    okage::DialogueOpacity opacity;opacity.set(120,8);opacity.tick();require(opacity.value==128 && opacity.delta==8);
    opacity.tick();require(opacity.value==128 && opacity.delta==0);
    opacity.set(4,-4);opacity.tick();require(opacity.value==0 && opacity.visible());
    opacity.tick();require(opacity.value==0 && !opacity.visible());
    const auto round=std::fegetround();opacity.set(.1f,.2f);opacity.tick();require(std::fegetround()==round);
    rejects([&]{opacity.set(0,1,128,0);});
    okage::DialogueClosure closure;unsigned stage=6;
    closure.update(stage);require(stage==7 && closure.decoration.value==128 && !closure.finished());
    for(unsigned i=0;i<31;++i){closure.decoration.tick();closure.update(stage);require(stage==7 && !closure.finished());}
    closure.decoration.tick();closure.update(stage);
    require(stage==9 && closure.decoration.value==0 && closure.decoration.delta==-4 && !closure.finished());
    closure.decoration.tick();closure.update(stage);require(stage==9 && closure.finished());
    // At zero the shared stage advances before the strict-bound fade stops.
    stage=5;rejects([&]{closure.update(stage);});
    okage::DialogueText starting({"\\l\\m[Speaker]\\y[WINTYPE(0)]\\v","Body"});
    okage::TextClock starting_clock;okage::DialogueController starting_controller;
    okage::DialogueOpacity starting_opacity;okage::DialogueClosure starting_closure;okage::DialogueOpening opening;
    opening.initialize(starting,starting_clock,starting_controller,starting_opacity,starting_closure);
    require(starting_controller.stage==0&&starting_controller.window_type==-1);
    starting_opacity.tick();starting_closure.decoration.tick();opening.bootstrap(starting,starting_clock);
    require(starting.state.flags==0x84e6&&starting.state.period==0xfffffffeu);
    require(opening.update(starting,starting_clock,starting_controller,starting_opacity,starting_closure));
    require(starting_controller.stage==2&&starting.state.flags==0x84c5&&starting_opacity.value==1);
    for(unsigned i=0;i<2;++i){
        starting_opacity.tick();starting_closure.decoration.tick();starting.frame(starting_clock);
        require(opening.update(starting,starting_clock,starting_controller,starting_opacity,starting_closure));
    }
    require(starting_controller.stage==5&&starting.state.period==1&&starting_controller.advance_count==0);
    require(starting.state.window=="WINTYPE(0)"&&(starting.state.flags&0x910));
    require(!opening.update(starting,starting_clock,starting_controller,starting_opacity,starting_closure));
    const auto start_effects=starting_controller.update(starting,{});
    require(starting_controller.window_type==0&&start_effects.cue==11&&starting_controller.layer_flags==0x70);
    okage::SceneFade fade;require(!fade.enabled && fade.alpha()==0 && fade.current==0);
    fade.current=1;fade.request(.5f);for(unsigned i=0;i<26;++i)fade.tick();
    require(fade.current==.5f && fade.enabled && fade.alpha()==64);
    fade.request(1);for(unsigned i=0;i<26;++i)fade.tick();
    require(fade.current==1 && fade.enabled && fade.alpha()==1);
    fade.tick();require(!fade.enabled && fade.alpha()==0);
    rejects([&]{fade.request(2);});
    fade.current=.5f;fade.color=0x123456;fade.script_request(0,0);
    require(fade.target==0&&fade.color==0&&fade.enabled&&fade.hold);
    fade.script_request(50,1);require(fade.color==0xffffff&&fade.target>.49f&&fade.target<=.5f);
    fade.script_request(0,-1);require(fade.color==0xffffff);
    rejects([&]{fade.script_request(101,0);});require(fade.color==0xffffff&&fade.target==0);
    std::cout<<"Dialogue input, pause/print/end transitions and opacity passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
