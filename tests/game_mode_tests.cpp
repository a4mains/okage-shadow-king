#include "okage/game_mode.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
void require(bool b){if(!b)throw std::runtime_error("Game mode lifecycle assertion failed");}
}
int main() {
    try {
        auto startup=okage::GameModeParent::startup();
        const okage::GameModeSelection initial{8,3,0};
        require(startup.current()==okage::GameModeSelection{}&&startup.phase()==0);
        require(!startup.has_child()&&!startup.has_dead_allocation());
        require(startup.requested()==initial&&startup.queue_words()==initial);
        unsigned constructed=0,child_updates=0;
        auto forbidden=[] {throw std::runtime_error("Startup manufactured retirement/free");};
        auto build=[&](okage::GameModeSelection selection){
            require(selection==initial&&startup.current()==initial&&startup.requested()==initial);
            require(startup.phase()==1&&!startup.has_child()&&!startup.has_dead_allocation());
            ++constructed;
        };
        auto boot_tick=[&]{startup.tick(forbidden,forbidden,build);if(startup.has_child())++child_updates;};
        boot_tick();
        require(startup.phase()==1&&!startup.has_child()&&constructed==0&&child_updates==0);
        require(startup.current()==okage::GameModeSelection{}&&startup.requested()==initial);
        boot_tick();
        require(startup.phase()==0&&startup.has_child()&&constructed==1&&child_updates==1);
        require(!startup.requested()&&startup.queue_words()==okage::GameModeSelection{0,3,0});
        boot_tick();require(constructed==1&&child_updates==2);
        auto blocked=okage::GameModeParent::startup();
        blocked.tick(forbidden,forbidden,[](auto){throw std::runtime_error("early constructor");});
        bool blocked_error=false;
        try{blocked.tick(forbidden,forbidden,[](auto){throw std::runtime_error("unsupported selector3");});}
        catch(const std::runtime_error& e){blocked_error=std::string(e.what())=="unsupported selector3";}
        require(blocked_error&&blocked.phase()==1&&!blocked.has_child()&&!blocked.has_dead_allocation());
        require(blocked.current()==initial&&blocked.requested()==initial&&blocked.queue_words()==initial);
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
        require(parent.queue_words()==okage::GameModeSelection{0,1,17});
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
        require(replacement.queue_words()==okage::GameModeSelection{3,7,23});
        bool zero=false;try {replacement.request({0,1,2});}catch(const std::runtime_error&){zero=true;}
        require(zero);
        std::cout<<"Game mode lifecycle tests passed\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
