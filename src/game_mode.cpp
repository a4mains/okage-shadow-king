#include "okage/game_mode.hpp"
#include <stdexcept>
namespace okage {
GameModeParent GameModeParent::startup() {
    GameModeParent parent({0,0,0});
    parent.child_=false;
    parent.request({8,3,0});
    return parent;
}
void GameModeParent::request(GameModeSelection selection) {
    if(!selection[0])throw std::runtime_error("Game mode zero is not a request");
    requested_=selection;queue_words_=selection; //0x124930 replaces the pending selection.
}
void GameModeParent::tick(const std::function<void()>& destroy_child,
                          const std::function<void()>& release_child,
                          const std::function<void(GameModeSelection)>& construct_child) {
    bool retire=false;
    if(phase_==0) {
        if(requested_) {
            if(child_) {child_=false;retire=true;}
            else phase_=1;
        }
    } else if(phase_==1) {
        if(!requested_||dead_)throw std::runtime_error("Invalid game mode construction state");
        current_=*requested_; //124818: +40 = +3c, +44 = +48, +4c = +50
        //1246b0 cases 9/10 write parent+4c = mode before 1151b8 runs the 124548 constructor (which reads it).
        if(current_[0]==9||current_[0]==10)current_[2]=current_[0];
        construct_child(current_);
        requested_.reset();queue_words_[0]=0;child_=true;--phase_;
    } else throw std::runtime_error("Unrecovered game mode parent phase");
    if(retire) {destroy_child();dead_=true;}
    else if(dead_) {release_child();dead_=false;}
}
}
