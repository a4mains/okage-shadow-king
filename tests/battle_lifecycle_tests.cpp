#include "okage/battle_lifecycle.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>
using namespace okage;
void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
void mark_dead(BattleState& root){root.bytes[0x1f]|=0x80;}
int main(){try {
    BattleState root;BattleRootLifetime life;GameModeParent parent({3,1,17});
    std::vector<std::string> calls;bool resource=true;unsigned normal=0,requests=0;
    const auto request=[&](GameModeSelection s){++requests;check(s==GameModeSelection{2,0xffffffffu,0},"Exact wrapper request");parent.request(s);calls.push_back("request");};
    const auto normal_root=[&]{++normal;calls.push_back("root.normal");mark_dead(root);};
    const auto destroy_root=[&]{check((root.bytes[0x1f]&0xc0)==0xc0,"Destruction toggle precedes destructor");calls.push_back("root.destroy");};
    const auto free_root=[&]{check((root.bytes[0x1f]&0xc0)==0x80,"Unlink toggle precedes allocation release");calls.push_back("root.free");};
    const auto outer=[&]{parent.tick([&]{check(!life.linked(),"Root freed before later resource destructor");resource=false;calls.push_back("resource.destroy");},[&]{calls.push_back("wrapper.free");},[&](GameModeSelection s){check(s[0]==2,"Outer constructs queued mode");calls.push_back("field.construct");});};
    outer();life.wrapper_update(root,request);calls.push_back("resource.update");life.traverse_root(root,normal_root,destroy_root,free_root);
    check(requests==0&&life.linked()&&resource,"Root death does not immediately request mode or destroy resource");
    outer();life.wrapper_update(root,request);calls.push_back("resource.update");life.traverse_root(root,normal_root,destroy_root,free_root);
    check(requests==1&&!life.linked()&&resource&&normal==1,"Next wrapper queues request before final resource update and root unlink");
    outer();check(!resource&&!parent.has_child(),"Outer next callback destroys wrapper resource");
    outer();check(parent.phase()==1&&!parent.has_dead_allocation(),"Distinct outer free traversal");
    outer();check(parent.has_child()&&parent.current()[0]==2,"Field constructor follows real free traversal");
    check(calls==std::vector<std::string>{"resource.update","root.normal","root.destroy","request","resource.update","root.free","resource.destroy","wrapper.free","field.construct"},"Complete scheduler ordering");
    BattleState failure;BattleRootLifetime failed;bool threw=false;
    try{failed.traverse_root(failure,[&]{mark_dead(failure);},[]{throw std::runtime_error("destructor");},[]{});}catch(const std::runtime_error&){threw=true;}
    check(threw&&failed.linked()&&(failure.bytes[0x1f]&0xc0)==0xc0,"Destructor failure retains preceding source flags and linked allocation");
    unsigned retried=0;
    try{failed.traverse_root(failure,[&]{++retried;},[&]{++retried;},[&]{++retried;});}catch(const std::runtime_error&){}
    try{failed.wrapper_update(failure,[&](GameModeSelection){++retried;});}catch(const std::runtime_error&){}
    check(!retried&&(failure.bytes[0x1f]&0xc0)==0xc0,"Failed teardown cannot resume with partially retired owners");
    threw=false;try{life.wrapper_update(root,request);}catch(const std::runtime_error&){threw=true;}
    check(threw&&requests==1,"Unlinked root cannot be visited by wrapper");
    BattleState live;BattleRootLifetime untouched;unsigned visits=0;
    for(unsigned i=0;i<3;++i){untouched.wrapper_update(live,{});untouched.traverse_root(live,[&]{++visits;},[]{throw std::runtime_error("unexpected destroy");},[]{});}
    check(visits==3&&untouched.linked(),"Live root continues without requiring transition service");
    std::cout<<"Battle root scheduler lifecycle tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
