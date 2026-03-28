#include "okage/dialogue.hpp"
#include "okage/event.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Missing owned input "+path.string());
    return {std::istreambuf_iterator<char>(file),{}};
}
constexpr std::uint16_t up=0x1000,down=0x4000,confirm=0x40;

struct Harness {
    okage::MessageBank& bank;
    okage::DialogueText text;
    okage::TextClock clock;
    okage::DialogueController controller;
    okage::DialogueOpacity opacity;
    okage::DialogueClosure closure;
    okage::DialogueOpening opening;
    okage::DialoguePadRepeat repeat;
    std::array<std::uint8_t,4096> variables{};
    std::uint16_t previous=0;
    unsigned frames=0;
    Harness(okage::MessageBank& b,std::int32_t id):bank(b),text(b,id) {
        text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        opacity.set(1,8);
        opening.initialize(text,clock,controller,opacity,closure);
        controller.get_variable=[this](unsigned index,unsigned bank){return okage::event_variable(variables,(bank<<16)|index);};
        controller.set_variable=[this](unsigned index,unsigned bank,std::int32_t value){okage::set_event_variable(variables,index,bank,value);};
    }
    bool paused() const {
        return std::bit_cast<std::int32_t>(text.state.delay)<0 || (text.state.flags&0x910);
    }
    void tick(std::uint16_t held) {
        if(++frames>20000)throw std::runtime_error("Roar dialogue did not terminate");
        okage::DialogueButtons input{held,std::uint16_t(held&~previous),0};previous=held;
        input.repeat=repeat.update(input.held,input.pressed);
        opacity.tick();closure.decoration.tick();
        if(controller.stage==0)opening.bootstrap(text,clock);
        else text.frame(clock,opacity.visible());
        const bool opening_tick=controller.stage<6 && opening.update(text,clock,controller,opacity,closure);
        if(controller.stage>=6)closure.update(controller.stage);
        else if(!opening_tick && controller.stage==5) {
            const auto effects=controller.update(text,input);
            if(effects.reset_text_clock){clock.revealed=0;clock.scroll=0;clock.scroll_target=0;clock.scroll_step=clock.scroll_base_step;}
            if(effects.close)opacity.set(128,-4);
        }
    }
    // Press/release one mask; the release tick lets the repeat clock reset.
    void press(std::uint16_t mask) {tick(mask);tick(0);}
};
}

int main(int argc,char** argv) {
    try {
        if(argc<2)throw std::runtime_error("usage: stan-roar-dialogue-tests <game-data-stage>");
        const fs::path stage=argv[1];
        const auto pme=stage/"FLDDATA/MES/F2.PME";
        if(!fs::exists(pme)) {std::cout<<"stan-roar-dialogue-tests: owned stage absent, skipped\n";return 0;}
        auto bank=okage::read_pme(read(pme));
        Harness h(bank,100013);
        while(!h.closure.finished()) {
            if(h.controller.stage==5 && h.paused())h.press(confirm);
            else h.tick(0);
        }
        require(okage::event_variable(h.variables,0x4000d)==0,"Unexpected roar result");
        std::cout<<"stan-roar-dialogue-tests passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"stan-roar-dialogue-tests: "<<e.what()<<'\n';return 1;
    }
}
