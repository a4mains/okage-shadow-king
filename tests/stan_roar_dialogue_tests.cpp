#include "okage/dialogue.hpp"
#include "okage/enemy.hpp"
#include "okage/event.hpp"
#include "okage/game_state.hpp"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
void require(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f,std::string_view expected) {
    try{f();}catch(const std::runtime_error& e){
        if(std::string_view(e.what()).find(expected)==std::string_view::npos)
            throw std::runtime_error(std::string("Unexpected rejection: ")+e.what());
        return;
    }
    throw std::runtime_error("Expected rejection: "+std::string(expected));
}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Missing owned input "+path.string());
    return {std::istreambuf_iterator<char>(file),{}};
}
constexpr std::uint16_t up=0x1000,down=0x4000,confirm=0x40;
constexpr std::array<std::string_view,3> tags{"##########$$","########$$$$","######$$$$$$"};

std::array<char,256> bytes(std::string_view text) {
    std::array<char,256> result{};std::copy(text.begin(),text.end(),result.begin());return result;
}
void unit_tests() {
    // 0x19bd98 count and 0x120d68 segments; past the terminator the source
    // continues after the NUL (zero-filled object => empty segment).
    require(okage::replacement_segments("0\\nA\\nB")==3 && okage::replacement_segments("")==1,"segment count");
    require(okage::replacement_segments("2\\a b\\nc")==2,"backslash without n is not a separator");
    auto b=bytes("2\\a river.\\nsucks!");
    require(okage::replacement_segment(b,0)=="2\\a river." && okage::replacement_segment(b,1)=="sucks!","segments");
    require(okage::replacement_segment(b,2).empty() && okage::replacement_segment(b,3).empty(),"zero bytes after NUL");
    // Stale bytes from an earlier, longer list are visible past the NUL.
    auto stale=bytes("0\\nAA\\nBBBBBB\\nCCC");
    std::copy_n("1\\nX",5,stale.begin()); // "1\nX" + NUL over "0\nAA".
    require(okage::replacement_segment(stale,1)=="X","current segment");
    require(okage::replacement_segment(stale,2).empty(),"stale separator directly after NUL");
    require(okage::replacement_segment(stale,3)=="BBBBBB" && okage::replacement_segment(stale,4)=="CCC","stale segments");
    std::array<char,256> full;full.fill('A');
    rejects([&]{(void)okage::replacement_segment(full,1);},"beyond the text-object replacement buffer");
    std::array<char,256> empty{};
    require(okage::replacement_segment(empty,0).empty(),"empty index0");
    // \z writes argument+NUL; \l clears only byte0.
    okage::TextState state;std::size_t cursor=0;
    std::string z="\\z[0\\nLong entry]";okage::consume_text_control(z,cursor,state);
    cursor=0;std::string l="\\l";okage::consume_text_control(l,cursor,state);
    require(state.replacement.empty() && state.replacement_bytes[0]==0 && state.replacement_bytes[3]=='L',"\\l clears byte0 only");
    cursor=0;std::string z2="\\z[1]";okage::consume_text_control(z2,cursor,state);
    // "1"+NUL overwrote "0\\"; the stale 'n' of the old separator remains.
    require(okage::replacement_segment(state.replacement_bytes,1)=="nLong entry","stale list after shorter \\z");
    // \r stores nothing and renders the rest of the fragment.
    okage::TextState r;cursor=0;std::string tagged="\\r[##########$$]word";
    require(okage::consume_text_control(tagged,cursor,r)==1 && cursor==16 && r.option.empty(),"\\r skip");
    cursor=0;std::string bare="\\rX";
    require(okage::consume_text_control(bare,cursor,r)==1 && cursor==2,"\\r without [ consumes two bytes");
    cursor=0;std::string open="\\r[abc";rejects([&]{okage::consume_text_control(open,cursor,r);},"Unterminated text argument");
    // 0x146288 guards: stale tables, slot aliasing, storage overflow.
    okage::MessageBank bank;
    bank.strings={"0","\\r[##########$$]0123456789"};bank.nodes={{0,0},{1,1}};bank.capacities={2,40};
    okage::QuestionChoice q;okage::DialogueController c;okage::ChoiceMenu m;okage::DialogueText t({""});
    auto wheel=[&](std::string_view list){
        t.state.replacement=std::string(list);t.state.replacement_bytes=bytes(list);
    };
    wheel("0\\nA\\nB");q.update(t,bank,c,m);require(!q.active,"three-line CC list is not a QC wheel");
    wheel("0\\nA\\nB\\nC");
    rejects([&]{q.update(t,bank,c,m);},"stale 0x2dc6f0");
    q.generated=true;q.words={};q.answers={0,1,2};
    wheel("3\\nA\\nB\\nC");rejects([&]{q.update(t,bank,c,m);},"three recovered table rows");
    wheel("0\\nA\\nB\\nC");q.update(t,bank,c,m); // result0 binds "A" (fits 29 bytes).
    require(bank.strings[1]=="\\r[##########$$]\\c[2018529360]A\\c[0]" && q.chosen[0]==0 && !(c.layer_flags&8),"synthetic binding");
    wheel("0\\nLonger word\\nB\\nC");rejects([&]{q.update(t,bank,c,m);},"exceeds its PME string storage");
    q.words[0]=3;wheel("0\\nA\\nB\\nC");q.update(t,bank,c,m);
    require(q.lines[0].empty() && bank.strings[1]=="\\r[##########$$]\\c[2018529360]\\c[0]","word past NUL reads zero bytes");
}

struct Harness {
    okage::MessageBank& bank;
    okage::DialogueText text;
    okage::TextClock clock;
    okage::DialogueController controller;
    okage::DialogueOpacity opacity;
    okage::DialogueClosure closure;
    okage::DialogueOpening opening;
    okage::DialoguePadRepeat repeat;
    okage::ChoiceMenu menu;
    okage::QuestionChoice& question;
    std::array<std::uint8_t,4096> variables{};
    std::uint16_t previous=0;
    unsigned frames=0,draws=0,qc=0,answers=0;
    std::vector<int> accepted;
    std::vector<std::string> pages;
    Harness(okage::MessageBank& b,okage::QuestionChoice& q,std::int32_t id,
            std::span<const std::int32_t> sysval,okage::GameRandom& random):bank(b),text(b,id),question(q) {
        text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        opacity.set(1,8);
        opening.initialize(text,clock,controller,opacity,closure);
        question.initialize(id,sysval,[&]{++draws;return random.next();},controller,menu);
        controller.get_variable=[this](unsigned index,unsigned bank){return okage::event_variable(variables,(bank<<16)|index);};
        controller.set_variable=[this](unsigned index,unsigned bank,std::int32_t value){okage::set_event_variable(variables,index,bank,value);};
    }
    bool paused() const {return std::bit_cast<std::int32_t>(text.state.delay)<0 || (text.state.flags&0x910);}
    void tick(std::uint16_t held) {
        if(++frames>40000)throw std::runtime_error("Roar dialogue did not terminate (stage "+std::to_string(controller.stage)+
            ", fragment "+std::to_string(text.position().fragment)+", option '"+text.state.option+"', flags "+
            std::to_string(text.state.flags)+", delay "+std::to_string(text.state.delay)+", layer "+std::to_string(controller.layer_flags)+", window "+text.state.window+", period "+std::to_string(text.state.period)+", page '"+text.page+"')");
        okage::DialogueButtons input{held,std::uint16_t(held&~previous),0};previous=held;
        input.repeat=repeat.update(input.held,input.pressed);
        opacity.tick();closure.decoration.tick();
        const auto before=text.page;
        if(controller.stage==0)opening.bootstrap(text,clock);
        else text.frame(clock,opacity.visible());
        if(text.page.size()<before.size())pages.push_back(before);
        const bool opening_tick=controller.stage<6 && opening.update(text,clock,controller,opacity,closure);
        if(controller.stage>=6)closure.update(controller.stage);
        else if(!opening_tick && controller.stage==5) {
            const auto option=text.state.option;
            const auto effects=controller.update(text,input);
            if(option=="QC" && controller.question_flash==10){++qc;accepted.push_back(controller.question_flash_row);}
            if(option=="ANSWERCMP" && text.state.option!=option)++answers;
            if(effects.accepted_choice>=0)menu.accepted=unsigned(effects.accepted_choice);
            if(effects.reset_text_clock){clock.revealed=0;clock.scroll=0;clock.scroll_target=0;clock.scroll_step=clock.scroll_base_step;}
            if(effects.close)opacity.set(128,-4);
        }
        if(!closure.finished()) {
            menu.update(text.state.replacement,text.choice_cursor,controller.choice_armed,"Ari");
            question.update(text,bank,controller,menu);
        }
    }
    void press(std::uint16_t mask) {tick(mask);tick(0);}
    void run(const std::function<int(unsigned slot)>& choose) {
        while(!closure.finished()) {
            if(controller.stage==5 && paused() && text.state.option=="QC") {
                require(question.active && question.slot==int(qc),"QC widget slot order");
                const int row=choose(unsigned(question.slot));
                require(!controller.choice_armed && text.choice_cursor==0,"QC starts unarmed at row0");
                press(down);require(controller.choice_armed && text.choice_cursor==0,"first direction arms only");
                if(row==2){press(up);require(text.choice_cursor==2,"up wraps from row0 to row2");}
                if(row==1){press(down);require(text.choice_cursor==1,"down moves to row1");}
                require(question.highlight_row==row && question.scale[unsigned(row)][1]==1.2f,"armed highlight");
                press(confirm);
                require(question.chosen[unsigned(qc-1)]==row && question.highlight_row==row,"bound row after acceptance");
            } else if(controller.stage==5 && paused())press(confirm);
            else tick(0);
        }
        pages.push_back(text.page);
    }
};
std::string bound(const okage::MessageBank& bank,unsigned slot) {
    const auto index=bank.tagged_string(tags[slot]);require(bool(index),"tag present");
    return bank.strings[*index];
}
}

int main(int argc,char** argv) {
    try {
        unit_tests();
        if(argc<2)throw std::runtime_error("usage: stan-roar-dialogue-tests <game-data-stage>");
        const fs::path stage=argv[1];
        const auto pme=stage/"FLDDATA/MES/F2.PME";
        if(!fs::exists(pme)) {std::cout<<"stan-roar-dialogue-tests: unit tests passed; owned stage absent, skipped\n";return 0;}
        const okage::GameDataTables tables(read(stage/"CMNDATA/XDB/BKDATA.XDB"),read(stage/"CMNDATA/XDB/ITEM.XDB"));
        const auto& sysval=tables.system_values();
        require(sysval.size()==222 && sysval.back()==999999999,"owned sysval extent");
        // Source strings used by 0x146288 (USA BKDATA sysmes).
        require(tables.system_message(0x72)=="\\f[2]\\s[0.95,1.0]%s" && tables.system_message(0xcc)=="\\c[%d]%s\\c[0]","sysmes formats");
        for(unsigned i=0;i<3;++i)require(tables.system_message(0xc9+i)==tags[i],"sysmes tags");
        const auto original=okage::read_pme(read(pme));

        // Golden 0x1468c8 tables from an independent asm transcription (seed1/2).
        {
            okage::QuestionChoice q;okage::DialogueController c;okage::ChoiceMenu m;m.accepted=2;c.question_flash=4;
            okage::GameRandom random{1};unsigned n=0;
            q.initialize(100013,sysval,[&]{++n;return random.next();},c,m);
            require(n==10 && random.state==0x0ff6d5dfu && q.generated && m.accepted==0 && c.question_flash==0,"100013 seed1 draw count");
            require(std::equal(q.words.begin(),q.words.begin()+9,std::array{0,5,7,7,0,1,2,9,8}.begin()),"100013 seed1 words");
            require(q.answers==std::array<std::int32_t,3>{0,1,0} && q.words[9]==-1 && q.words[47]==-1,"100013 seed1 answers/cleared rows");
            random.state=2;n=0;q.initialize(100013,sysval,[&]{++n;return random.next();},c,m);
            require(std::equal(q.words.begin(),q.words.begin()+9,std::array{4,0,8,4,0,7,1,2,0}.begin()) &&
                    q.answers==std::array<std::int32_t,3>{1,1,1} && n==10,"100013 seed2 tables");
            random.state=1;n=0;q.initialize(100001,sysval,[&]{++n;return random.next();},c,m);
            require(std::equal(q.words.begin(),q.words.begin()+9,std::array{5,6,8,5,1,4,4,6,3}.begin()),"100001 seed1 words");
            // A message without a sysval record draws nothing and leaves tables.
            const auto before=q.words;n=0;q.initialize(10001,sysval,[&]{++n;return random.next();},c,m);
            require(n==0 && !q.generated && q.words==before,"non-roar message has no QC draws");
            rejects([&]{q.initialize(100013,std::span<const std::int32_t>(sysval).first(8),[&]{return random.next();},c,m);},"unterminated");
        }

        unsigned successes=0,failures=0;
        auto run_bank=[&](const okage::MessageBank& original,std::string_view name,std::initializer_list<std::uint32_t> seeds) {
        for(std::int32_t id=100001;id<=100013;++id) {
            for(std::uint32_t seed:seeds) {
                for(const bool correct:{true,false}) {
                    auto bank=original;
                    require(bank.bind("############","Ari"),"player-name tag");
                    okage::QuestionChoice question;okage::GameRandom random{seed};
                    Harness h(bank,question,id,sysval,random);
                    require(h.draws==10 && question.generated,"QC table draws");
                    const auto answers=question.answers;
                    std::array<std::string,3> chosen_words;
                    h.run([&](unsigned slot) {
                        const int row=correct||slot!=2 ? answers[slot] : (answers[slot]+1)%3;
                        chosen_words[slot]=question.lines[unsigned(row)];
                        return row;
                    });
                    const auto label=std::string(name)+" message "+std::to_string(id)+" seed "+std::to_string(seed)+(correct?" correct":" wrong");
                    require(h.closure.finished() && h.controller.stage==9,label+": terminal state");
                    require(h.qc==3 && h.answers==1,label+": QC/ANSWERCMP dispatch");
                    require((question.chosen==answers)==correct && bool(h.controller.layer_flags&8)==correct,label+": comparison flag");
                    require(okage::event_variable(h.variables,0x4000d)==(correct?1:0),label+": roar result var(13,4)");
                    std::string all;for(const auto& page:h.pages)all+=page+"|";
                    require(all.find("\x82\x4f")==std::string::npos,label+": placeholder digits rendered");
                    std::size_t at=0;
                    for(unsigned slot=0;slot<3;++slot) {
                        require(bound(bank,slot)=="\\r["+std::string(tags[slot])+"]\\c[2018529360]"+chosen_words[slot]+"\\c[0]",label+": binding");
                        const auto found=all.find(chosen_words[slot],at);
                        require(found!=std::string::npos,label+": rendered substitution order");
                        at=found+chosen_words[slot].size();
                    }
                    if(id==100006)require(all.find("Ari")!=std::string::npos,label+": player-name binding renders");
                    (correct?successes:failures)++;
                }
            }
        }
        };
        run_bank(original,"F2.PME",{1u,2u,7u});
        require(successes==39 && failures==39,"all F2 roar runs");
        // The same 13 roar IDs exist in every staged field bank; one seed each.
        unsigned banks=0;
        for(const auto& entry:fs::directory_iterator(stage/"FLDDATA/MES")) {
            if(entry.path().extension()!=".PME" || entry.path().filename()=="F2.PME")continue;
            const auto other=okage::read_pme(read(entry.path()));
            const bool roar=std::any_of(other.nodes.begin(),other.nodes.end(),[&](const okage::MessageNode& node) {
                return node.depth==0 && other.strings.at(node.string_index).starts_with("100013");
            });
            if(!roar)continue;
            require(bool(other.tagged_string(tags[0])),"roar bank answer tags");
            run_bank(other,entry.path().filename().string(),{3u});++banks;
        }
        require(banks==9,"other staged roar banks");

        {
            auto bank=original;okage::QuestionChoice question;okage::GameRandom random{1};
            Harness h(bank,question,100013,sysval,random);
            h.run([&](unsigned slot){return slot==2?1:question.answers[slot];});
            require(question.words[7]==9 && question.lines[1].empty(),"overrun word9 displays empty");
            require(bound(bank,2)=="\\r[######$$$$$$]\\c[2018529360]\\c[0]","empty overrun binding");
            require(okage::event_variable(h.variables,0x4000d)==0 && h.closure.finished(),"overrun answer is wrong");
        }
        // Up from row0 skips row3: 11 segments, 0-1 -> 10 then +14 -> 2.
        {
            auto bank=original;okage::QuestionChoice question;okage::GameRandom random{2};
            Harness h(bank,question,100002,sysval,random);
            while(!(h.paused() && h.text.state.option=="QC")) {
                if(h.controller.stage==5 && h.paused())h.press(confirm);else h.tick(0);
            }
            h.press(up);require(h.controller.choice_armed && h.text.choice_cursor==0,"up arms");
            h.press(up);require(h.text.choice_cursor==2,"wrap to 2");
            h.press(up);require(h.text.choice_cursor==1,"up to 1");
            h.press(down);h.press(down);require(h.text.choice_cursor==0,"down wraps 2->0 (skips 3)");
            h.press(confirm);
            // Acceptance sets 10; 0x146288 decrements on that tick and the release tick.
            require(h.controller.question_flash==8 && h.question.highlight_row==0 && !h.controller.choice_armed,
                    "flash counts down in 0x146288");
            for(unsigned i=0;i<8 && h.question.active;++i)h.tick(0);
            require(h.controller.question_flash==0 || !h.question.active,"flash expires while the wheel remains");
        }
        std::cout<<"stan-roar-dialogue-tests passed ("<<successes+failures<<" owned message runs, "<<banks+1<<" banks)\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"stan-roar-dialogue-tests: "<<e.what()<<'\n';return 1;
    }
}
