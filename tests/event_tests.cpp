#include "okage/event.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void require(bool value) {if(!value)throw std::runtime_error("Event test assertion failed");}
template<class F> void rejects(F f) {bool failed=false;try {f();}catch(const std::runtime_error&){failed=true;}require(failed);}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=value>>(8*i);
}
int main() {
    try {
        {
            std::vector<std::uint8_t> backing(0x1020,0xa5);
            auto storage=std::span<std::uint8_t>(backing).subspan(16,0x1000);
            okage::initialize_game_variables(storage);
            require(storage[0xe]==1 && storage[0xc]==0 && storage[0xd]==0);
            require(okage::event_variable(storage,0x40018)==3);
            require(okage::event_variable(storage,0x4001e)==3);
            require(okage::event_variable(storage,0x40003)==1000);
            for(unsigned i=25;i<30;++i)require(okage::event_variable(storage,0x40000|i)==0);
            require(std::equal(storage.begin()+0x844,storage.begin()+0x848,"Ari"));
            require(std::equal(storage.begin()+0x864,storage.begin()+0x868,"???"));
            constexpr unsigned levels[]={1,5,10,25,30,40,1};
            for(unsigned i=0;i<7;++i) {
                const auto bank=(8+i)<<16;
                require(okage::event_variable(storage,bank)==1);
                require(okage::event_variable(storage,bank|1)==int(170+i));
                require(okage::event_variable(storage,bank|2)==int(levels[i]));
                require(okage::event_variable(storage,bank|7)==5);
                require(okage::event_variable(storage,bank|4)==0);
            }
            require(std::all_of(storage.begin()+0x7f4,storage.begin()+0x844,[](auto v){return v==0;}));
            require(std::all_of(storage.begin()+0x878,storage.begin()+0xd78,[](auto v){return v==0;}));
            require(std::all_of(backing.begin(),backing.begin()+16,[](auto v){return v==0xa5;}));
            require(std::all_of(backing.begin()+16+0xd78,backing.end(),[](auto v){return v==0xa5;}));
            const auto reset=backing;okage::initialize_game_variables(storage);require(backing==reset);
            for(std::size_t length:{0u,0x844u,0xd77u}) {
                rejects([&]{okage::initialize_game_variables(storage.first(length));});
                require(backing==reset);
            }
        }
        std::vector<std::uint8_t> variables(0x1000),state(0x800,0x55),script(64);
        variables[0x10]=0x81;variables[0x210]=0x80;variables[0x410]=0xff;
        put(variables,0x510,0x80000000);
        require(okage::event_variable(variables,0)==1 && okage::event_variable(variables,1)==0);
        require(okage::event_variable(variables,7)==1);
        require(okage::event_variable(variables,0x20000)==-128);
        require(okage::event_variable(variables,0x30000)==-1);
        require(okage::event_variable(variables,0x40000)==(-2147483647-1));
        rejects([&]{okage::event_variable(variables,0x50000);});
        rejects([&]{okage::event_variable(variables,0x800);});
        rejects([&]{okage::event_variable(variables,0x31000);});
        rejects([&]{okage::event_variable(variables,0x4ffff);});
        okage::set_event_variable(variables,2,0,1);require(variables[0x10]==0xa1);
        okage::set_event_variable(variables,2,0,0);require(variables[0x10]==0x81);
        okage::set_event_variable(variables,2047,1,-1);require(variables[0x20f]==1);
        okage::set_event_variable(variables,511,2,255);require(okage::event_variable(variables,0x201ff)==-1);
        okage::set_event_variable(variables,1,4,-123);require(okage::event_variable(variables,0x40001)==-123);
        rejects([&]{okage::set_event_variable(variables,65536,0,1);});
        rejects([&]{okage::set_event_variable(variables,0,5,1);});
        // Signed short records occupy adjacent 0x50-byte ranges. Exercise
        // both ends of every record and verify all unrelated bytes survive.
        for(unsigned bank=8;bank<=15;++bank)for(unsigned index:{0u,7u,39u}) {
            std::vector<std::uint8_t> shorts(0x845,0xa5);
            const auto offset=0x5c4+(bank-8)*0x50+index*2;
            for(const auto value:{0,32767,32768,65535,65536,-1,-32769,(-2147483647-1),2147483647}) {
                const auto previous=shorts;
                okage::set_event_variable(shorts,index,bank,value);
                const auto low=std::uint32_t(value)&0xffff;
                require(okage::event_variable(shorts,(bank<<16)|index)==
                        (low<32768?int(low):int(low)-65536));
                require(shorts[offset]==(low&255)&&shorts[offset+1]==(low>>8));
                for(std::size_t i=0;i<shorts.size();++i)
                    if(i!=offset&&i!=offset+1)require(shorts[i]==previous[i]);
            }
            const auto previous=shorts;
            rejects([&]{okage::event_variable(shorts,(bank<<16)|40);});
            rejects([&]{okage::set_event_variable(shorts,40,bank,7);});
            require(shorts==previous);
            shorts.resize(offset+2);okage::set_event_variable(shorts,index,bank,-123);
            require(okage::event_variable(shorts,(bank<<16)|index)==-123);
            shorts.pop_back();const auto truncated=shorts;
            rejects([&]{okage::event_variable(shorts,(bank<<16)|index);});
            rejects([&]{okage::set_event_variable(shorts,index,bank,42);});
            require(shorts==truncated);
        }
        for(unsigned bank:{5u,6u,7u,16u,24u,65535u}) {
            const auto previous=variables;
            rejects([&]{okage::event_variable(variables,bank<<16);});
            rejects([&]{okage::set_event_variable(variables,0,bank,1);});
            require(variables==previous);
        }
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
        bad=script;put(bad,36,0x50000);
        rejects([&]{okage::run_event_setup(bad,variables,state);});require(state==previous);
        for(std::size_t n=0;n<script.size();++n)
            rejects([&]{okage::run_event_setup(okage::Bytes(script.data(),n),variables,state);});
        {
            const std::string table="unused!variant";
            std::vector<std::uint8_t> text_script(52+table.size()+1);
            put(text_script,0,0x544d58);put(text_script,4,20);put(text_script,12,table.size()+1);
            put(text_script,16,0xffffffff);put(text_script,32,0x100fff59);put(text_script,36,7);
            put(text_script,40,0x10112359);put(text_script,44,7);put(text_script,48,0);
            std::copy(table.begin(),table.end(),text_script.begin()+52);
            auto out=state;
            require(okage::run_event_setup(text_script,variables,out)==std::vector<std::uint32_t>({32,40,48}));
            auto text_at=[&](unsigned offset){return std::string(reinterpret_cast<const char*>(out.data()+offset));};
            require(text_at(0x2c0)=="variant.xpf"&&text_at(0x300)=="flddata/variant/");
            const auto before=out;
            auto invalid=text_script;invalid.back()='x';
            rejects([&]{okage::run_event_setup(invalid,variables,out);});require(out==before);
            invalid=text_script;put(invalid,44,table.size()+1);
            rejects([&]{okage::run_event_setup(invalid,variables,out);});require(out==before);
            invalid=text_script;put(invalid,40,0x11200059);
            rejects([&]{okage::run_event_setup(invalid,variables,out);});require(out==before);
            invalid=text_script;invalid.resize(52+65);std::fill(invalid.begin()+52,invalid.end(),'a');invalid.back()=0;put(invalid,12,65);put(invalid,36,0);
            rejects([&]{okage::run_event_setup(invalid,variables,out);});require(out==before);
        }
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
