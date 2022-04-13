#include "okage/game_mode.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
void require(bool b){if(!b)throw std::runtime_error("Game mode lifecycle assertion failed");}
}
int main() {
    try {
        okage::GameModeParent parent({2,96,0});
        std::vector<unsigned> callbacks;
        unsigned field_ticks=0,battle_ticks=0;
        auto destroy=[&]{
            require(!parent.has_child() && parent.phase()==0);
            require(parent.current()[0]==2 && parent.requested()->at(0)==3);
            // Resource/player/field destructors, then unlinking on next pass.
            callbacks.insert(callbacks.end(),{1,2,3});
        };
        auto release=[&]{require(parent.phase()==1&&!parent.has_child());callbacks.push_back(4);};
        auto construct=[&](okage::GameModeSelection selection){
            require(selection==okage::GameModeSelection{3,1,17});
            require(parent.current()==selection && parent.requested()==selection);
            require(!parent.has_dead_allocation());callbacks.push_back(5);
        };
        auto traversal=[&]{
            parent.tick(destroy,release,construct);
            if(parent.has_child()) {
                if(parent.current()[0]==2)++field_ticks;
                else {++battle_ticks;callbacks.push_back(6);}
            }
        };
        traversal();require(field_ticks==1&&callbacks.empty());
        parent.request({3,1,17});
        // Queuing from a field callback does not retire during that callback.
        require(parent.has_child()&&parent.current()[0]==2);
        traversal();require(callbacks==std::vector<unsigned>{1,2,3});
        require(parent.has_dead_allocation()&&parent.phase()==0&&field_ticks==1);
        traversal();require(callbacks==std::vector<unsigned>{1,2,3,4});
        require(!parent.has_dead_allocation()&&parent.phase()==1&&battle_ticks==0);
        traversal();require(callbacks==std::vector<unsigned>{1,2,3,4,5,6});
        require(!parent.requested()&&parent.phase()==0&&battle_ticks==1);
        traversal();require(battle_ticks==2&&field_ticks==1);

        okage::GameModeParent replacement({2,0,0});
        replacement.request({3,2,8});replacement.request({3,7,23});
        replacement.tick([]{},[]{},[](auto){throw std::runtime_error("early construction");});
        replacement.tick([]{},[]{},[](auto){throw std::runtime_error("early construction");});
        bool failed=false;
        try {replacement.tick([]{},[]{},[&](auto s){
            require(s==okage::GameModeSelection{3,7,23});
            throw std::runtime_error("unrecovered constructor");
        });}catch(const std::runtime_error& e){failed=std::string(e.what())=="unrecovered constructor";}
        require(failed&&!replacement.has_child()&&replacement.requested()&&replacement.phase()==1);
        bool zero=false;try {replacement.request({0,1,2});}catch(const std::runtime_error&){zero=true;}
        require(zero);
        std::cout<<"Game mode lifecycle tests passed\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
