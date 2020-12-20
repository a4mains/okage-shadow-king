#include "okage/text.hpp"
#include "okage/event.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <limits>
#include <iostream>
#include <stdexcept>

void require(bool value) {if(!value)throw std::runtime_error("Text assertion failed");}
template<class F> void rejects(F f) {bool failed=false;try{f();}catch(const std::runtime_error&){failed=true;}require(failed);}
void word(std::vector<std::uint8_t>& b,std::uint32_t n) {for(unsigned i=0;i<4;++i)b.push_back(n>>(8*i));}
int main() {
    try {
        const std::string header="KOME_RXUI_DATA  ";
        std::vector<std::uint8_t> b(header.begin(),header.end());
        const std::array<std::string,4> strings={"system\r\n","42\r\n","\\m[Voice]\\v\r\n","Hello\\v\\e"};
        word(b,strings.size());
        for(unsigned i=0;i<strings.size();++i){word(b,i);word(b,strings[i].size()+1);}
        word(b,4);for(auto n:{0u,0u,0u,1u,1u,2u,2u,3u})word(b,n);
        for(const auto& s:strings){b.insert(b.end(),s.begin(),s.end());b.push_back(0);}
        const auto bank=okage::read_pme(b);
        require(bank.message(42)==std::vector<std::string>({strings[2],strings[3]}));
        require(bank.message(-1)==bank.message(42) && bank.message(999)==bank.message(42));
        auto branched=bank;
        branched.nodes={{0,0},{0,1},{1,2},{2,3},{2,0},{3,0},{1,3}};
        require(branched.message_indices(42)==std::vector<std::uint32_t>({2,3,3}));
        for(std::size_t n=0;n<b.size();++n)rejects([&]{okage::read_pme(okage::Bytes(b.data(),n));});
        auto bad=b;bad[16]=0xff;rejects([&]{okage::read_pme(bad);});
        bad=b;bad.push_back(0);rejects([&]{okage::read_pme(bad);});
        bad=b;bad.back()='x';rejects([&]{okage::read_pme(bad);});
        okage::TextState state;std::size_t cursor=0;
        require(okage::consume_text_control("\\e",cursor,state)==1 && cursor==1 && state.progress==1);
        require(okage::consume_text_control("\\e",cursor,state)==0 && cursor==1);
        for(auto source:{"\\", "\\g[2]", "\\m[broken", "\\c"}) {
            cursor=0;rejects([&]{okage::consume_text_control(source,cursor,state);});
            require(cursor==0 && state.progress==1);
        }
        cursor=0;require(okage::consume_text_control("\\c[4294967295]",cursor,state)==1);
        require(state.color==127 && state.previous_color==0x80808080);
        cursor=0;okage::consume_text_control("\\c[0]",cursor,state);require(state.color==0x80808080);
        okage::DialogueText text({"\\m[Voice]\\y[WINTYPE(0)]\\v\r\n",
            "First\\v\r\n", "\\c[100]\\e\\c[0]\\lSecond\\v\\c[100]\\e\\c[0]"});
        std::array<std::uint32_t,2> layers{0x40,0x40};
        okage::DialogueWaitState wait{5,1};
        auto poll=[&]{return okage::poll_dialogue_wait(wait,2,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});};
        auto page=[&]{for(unsigned i=0;i<1000;++i){auto s=text.step();if(s!=okage::TextStep::glyph)return s;}throw std::runtime_error("Text loop");};
        require(page()==okage::TextStep::pause && text.state.speaker=="Voice" && text.page.empty());
        text.resume();require(page()==okage::TextStep::pause && text.page=="First");
        require(poll()==1 && wait.observed==0);
        text.resume();require(page()==okage::TextStep::pause && text.page=="Second" && text.state.progress==1);
        require(poll()==1 && wait.observed==1 && wait.countdown==0);
        text.resume();require(page()==okage::TextStep::end && text.state.progress==2);
        require(poll()==1 && wait.countdown==2);require(poll()==-1);require(poll()==0);
        require(text.step()==okage::TextStep::end);
        okage::DialogueText timed({"A B\\v\\e"});
        timed.state.flags=0x8001;timed.state.period=1;
        okage::TextClock clock;
        timed.frame(clock);require(!timed.position().loaded && timed.state.phase==1);
        timed.frame(clock);require(timed.position().loaded && timed.page.empty());
        timed.frame(clock);timed.frame(clock);require(timed.page=="A" && timed.position().cursor==1);
        timed.frame(clock);timed.frame(clock);require(timed.page=="A B" && timed.position().cursor==3);
        timed.frame(clock);timed.frame(clock);require((timed.state.flags&0x910)==0x910);
        const auto paused=timed.position();timed.frame(clock);require(timed.position().cursor==paused.cursor);
        timed.confirm(1);require(!(timed.state.flags&0x910) && timed.state.phase==0 && timed.state.period==1);
        timed.frame(clock);timed.frame(clock);require(timed.state.progress==1 && timed.position().fragment==1);
        timed.frame(clock);require((timed.state.flags&0x22)==0x22);
        okage::DialogueText quick({"ABCDEFGHIJ"});quick.state.flags=0x8001;quick.state.period=1;
        quick.confirm(1);require(quick.state.period==0xfffffffc);
        quick.frame(clock);require(quick.position().loaded && quick.page.empty());
        quick.frame(clock);require(quick.page=="ABCD");
        quick.state.delay=2;quick.confirm(1);require(quick.state.delay==2);
        quick.frame(clock);require(quick.page=="ABCD" && quick.state.delay==1);
        rejects([&]{quick.seek({2,0,false});});
        rejects([&]{quick.seek({0,1,false});});
        const auto rounding=std::fegetround();
        clock.scroll_target=1;clock.scroll=0.1f;clock.scroll_step=0.2f;clock.scroll_base_step=1;
        okage::tick_text_clock(quick.state,clock);require(std::fegetround()==rounding);
        clock.scroll=std::numeric_limits<float>::infinity();
        rejects([&]{okage::tick_text_clock(quick.state,clock);});require(std::fegetround()==rounding);
        std::cout<<"Text import, bounds, control and event integration tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
