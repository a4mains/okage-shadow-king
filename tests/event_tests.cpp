#include "okage/event.hpp"
#include <iostream>
#include <stdexcept>

void require(bool value) {if(!value)throw std::runtime_error("Event test assertion failed");}
template<class F> void rejects(F f) {bool failed=false;try {f();}catch(const std::runtime_error&){failed=true;}require(failed);}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=value>>(8*i);
}
int main() {
    try {
        std::vector<std::uint8_t> variables(0x1000),state(0x800,0x55),script(64);
        variables[0x10]=0x81;variables[0x210]=0x80;variables[0x410]=0xff;
        put(variables,0x510,0x80000000);
        require(okage::event_variable(variables,0)==1 && okage::event_variable(variables,1)==0);
        require(okage::event_variable(variables,7)==1);
        require(okage::event_variable(variables,0x20000)==-128);
        require(okage::event_variable(variables,0x30000)==-1);
        require(okage::event_variable(variables,0x40000)==(-2147483647-1));
        rejects([&]{okage::event_variable(variables,0x80000);});
        rejects([&]{okage::event_variable(variables,0x800);});
        rejects([&]{okage::event_variable(variables,0x31000);});
        rejects([&]{okage::event_variable(variables,0x4ffff);});
        put(script,0,0x544d58);put(script,4,32);put(script,16,0xffffffff);
        // Signed -128 < 0: take body. False branch skips to end at byte 60.
        put(script,32,(28u<<20)|(4u<<8)|11);put(script,36,0x20000);put(script,40,0);
        put(script,44,0xff800150);
        put(script,48,0x20300059);put(script,52,0xdeadbeef);
        put(script,56,0);put(script,60,0);
        auto pcs=okage::run_event_setup(script,variables,state);
        require(pcs==std::vector<std::uint32_t>({32,44,48,56}));
        require(state[0x1a8]==1 && state[0x40c]==0xef && state[0x40f]==0xde);
        variables[0x210]=0;const auto previous=state;
        require(okage::run_event_setup(script,variables,state)==std::vector<std::uint32_t>({32,60}));
        require(state==previous);
        variables[0x210]=0x80;
        auto bad=script;put(bad,48,0xdeadbeef);
        rejects([&]{okage::run_event_setup(bad,variables,state);});require(state==previous);
        bad=script;put(bad,32,1);put(bad,36,0xfffffffc);
        rejects([&]{okage::run_event_setup(bad,variables,state);});require(state==previous);
        bad=script;put(bad,36,0x80000);
        rejects([&]{okage::run_event_setup(bad,variables,state);});require(state==previous);
        for(std::size_t n=0;n<script.size();++n)
            rejects([&]{okage::run_event_setup(okage::Bytes(script.data(),n),variables,state);});
        okage::DialogueWaitState wait{5,1,0,0,0};
        std::vector<std::uint32_t> layers{0x50,0x70};
        okage::DialogueObservation dialogue{true,0,layers,4};
        require(okage::poll_dialogue_wait(wait,2,dialogue)==1);
        require(wait.observed==1 && wait.accumulated==1 && wait.countdown==0);
        require(okage::poll_dialogue_wait(wait,2,dialogue)==1);
        require(wait.observed==2 && wait.accumulated==0 && wait.countdown==2);
        require(okage::poll_dialogue_wait(wait,2,dialogue)==-1 && wait.countdown==1);
        require(okage::poll_dialogue_wait(wait,2,dialogue)==0 && wait.countdown==0);
        require(okage::poll_dialogue_wait(wait,-1,dialogue)==0);
        wait.active=0;require(okage::poll_dialogue_wait(wait,-1,dialogue)==-1);
        wait={};layers[1]=0x10;
        require(okage::poll_dialogue_wait(wait,1,dialogue)==1 && wait.observed==0);
        dialogue.present=false;
        rejects([&]{okage::poll_dialogue_wait(wait,1,dialogue);});
        require(okage::poll_dialogue_wait(wait,0,dialogue)==-1 && wait.countdown==2);
        require(okage::poll_dialogue_wait(wait,0,dialogue)==-1);
        require(okage::poll_dialogue_wait(wait,0,dialogue)==0);
        std::cout<<"Event signed variables, branch paths, state writes and atomic failure passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
