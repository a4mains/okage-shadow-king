#include "okage/headless_control.hpp"
#include <iostream>
static void require(bool value){if(!value)throw std::runtime_error("headless control assertion");}
int main(){try {
    using K=okage::HeadlessCommand::Kind;
    auto step=okage::parse_headless_command("step 6000 f840",6000);
    require(step.kind==K::Step&&step.ticks==6000&&step.held==0xf840);
    require(okage::parse_headless_command("step 1 0000",1).held==0);
    require(okage::parse_headless_command("capture after-save_2",0).name=="after-save_2");
    require(okage::parse_headless_command("status",0).kind==K::Status);
    require(okage::parse_headless_command("view post-chest_2",0).kind==K::View);
    require(okage::parse_headless_command("quit",0).kind==K::Quit);
    require(okage::parse_headless_command("observe",0).kind==K::Observe);
    for(auto line:{"", "step 0 40", "step -1 40", "step 6001 0", "step 2 0",
                  "step 1 10000", "step 1 -1", "step 1 0x40", "step 1 40x",
                  "step 1 40 extra", "capture ../save", "capture /tmp/x", "capture final",
                  "capture FINAL", "capture Final", "capture foo.png", "capture foo bar",
                  "status now", "view ../save", "view /tmp/x", "view final", "view FINAL",
                  "view foo.png", "view foo bar", "quit now", "load snapshot", "observe now"}) {
        bool rejected=false;try{okage::parse_headless_command(line,1);}catch(const std::runtime_error&){rejected=true;}
        require(rejected);
    }
    std::cout<<"Headless command bounds and capture isolation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
