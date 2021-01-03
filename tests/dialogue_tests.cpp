#include "okage/dialogue.hpp"
#include <cfenv>
#include <iostream>
#include <stdexcept>

void require(bool value) {if(!value)throw std::runtime_error("Dialogue test failed");}
template<class F> void rejects(F f) {try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected dialogue rejection");}
int main() {try {
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
    c.stage=5;text.state.window="WINTYPE(2)";rejects([&]{c.update(text,{});});
    text.state.window.clear();text.state.option="Choice";rejects([&]{c.update(text,{});});
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
    std::cout<<"Dialogue input, pause/print/end transitions and opacity passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
