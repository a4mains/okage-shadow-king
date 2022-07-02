#include "okage/battle_ui.hpp"
#include "okage/event.hpp"
#include "okage/ui_window.hpp"
#include "okage/battle_interface.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t w) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=w>>(8*i);
}
void digest(std::vector<std::uint8_t>& b,unsigned at,std::string_view s) {
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<s.size()/2;++i)b.at(at+i)=digits.find(s[i*2])*16+digits.find(s[i*2+1]);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(312);
    const std::string_view magic="OKBTUI1";std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1);put(b,12,2);put(b,16,28);put(b,20,112);
    digest(b,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    digest(b,56,"5632c521fdd835a20c81960f9758447216733e2de4afc2d0e32b05ace9e8805b");
    for(unsigned i=0;i<2;++i) {
        const unsigned at=88+112*i;
        put(b,at,i+1);put(b,at+4,40);put(b,at+8,6);put(b,at+12,2);
        put(b,at+16,0x80000000);put(b,at+64,0x80808080);
        put(b,at+80,1);put(b,at+84,1);put(b,at+88,i+1);put(b,at+92,i+1);
        put(b,at+96,i+2);put(b,at+100,i?13:29);put(b,at+104,i?8:17);
    }
    return b;
}
void rejects(const std::vector<std::uint8_t>& b) {
    try {okage::BattleUiData data(b);}
    catch(const std::runtime_error&) {return;}
    throw std::runtime_error("invalid interface accepted");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==4&&std::string_view(argv[1])=="--window-diagnostic") {
            std::ifstream in(argv[2],std::ios::binary);require(bool(in),"missing owned interface");
            std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),{});
            const okage::BattleUiData data(b);
            const std::filesystem::path path(argv[3]);
            require(!std::filesystem::exists(path)&&!std::filesystem::is_symlink(path),"refusing existing window report");
            std::ofstream out(path);require(bool(out),"cannot create window report");
            out<<"{\"schema\":\"okage-ui-window-diagnostic-v1\",\"scope\":\"198e38 and window configuration only; empty incoming streams and atlas, slots0/1; no layer constructors\",\"windows\":[";
            for(unsigned i=0;i<2;++i) {
                const auto& d=data.layers()[i];
                auto stream=std::make_shared<okage::UiWindowStream>(okage::UiWindowStream{d.stream(),{}});
                okage::UiWindow window(d.glyph_capacity(),d.sprite_capacity(),i,stream);
                const std::array<okage::UiWindow::AtlasTuple,1> atlas{};
                require(!window.configure(d.words,atlas,std::make_shared<int>(1)),"empty atlas unexpectedly dirty");
                if(i)out<<',';out<<"{\"slot\":"<<i<<",\"words\":[";
                for(unsigned at=0;at<okage::UiWindow::scalar_bytes;at+=4) {
                    if(at)out<<',';const auto value=window.known_word(at);
                    if(value)out<<*value;else out<<"null";
                }
                out<<"]}";
            }
            out<<"]}\n";require(bool(out),"cannot write window report");
            std::cout<<"owned interface window diagnostic passed\n";return 0;
        }
        if(argc==3&&std::string_view(argv[1])=="--diagnostic") {
            std::ifstream in(argv[2],std::ios::binary);require(bool(in),"missing owned interface");
            std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),{});
            const okage::BattleUiData data(b);
            require(data.layers()[0].glyph_capacity()==29&&data.layers()[1].sprite_capacity()==8,"owned capacities changed");
            std::cout<<"owned battle interface parsed\n";return 0;
        }
        require(argc==1,"usage: battle-ui-tests [--diagnostic owned-battle-ui.bin | --window-diagnostic owned-battle-ui.bin fresh.json]");
        auto b=fixture();const okage::BattleUiData data(b);b.clear();
        require(data.layers()[0].words[4]==0x80000000&&data.layers()[1].words[16]==0x80808080,"owned literal bits changed");
        require(data.layers()[0].stream()==1&&data.layers()[1].type()==3,"host bindings changed");
        okage::BattleUiShared shared;std::vector<unsigned> constructors;
        auto texture=std::make_shared<int>(1);
        {
            okage::BattleInterface ui(shared,data,texture,0x98000000,[&](unsigned type){
                constructors.push_back(type);
                require(shared.current_stream==type-1&&shared.stream_cursor==4,"source constructor stream order");
                require(shared.registry[type-2]&&shared.registry[type-2]->word(0x1960)==0,"window scans before stream replacement");
            });
            require(constructors==std::vector<unsigned>{2,3},"authored constructor order");
            require(ui.handles()==std::array<int,2>{0,1},"registry handles");
            const auto& w=ui.words();
            require(w[0x1c/4]==0x18000000&&w[0xe40/4]==2&&w[0xe44/4]==3&&w[0xe94/4]==1,"manager scalar fields");
            for(unsigned at:{0x88u,0x8cu,0x168u,0x16cu,0xed8u})require(w[at/4]==0xffffffff,"manager postwrites overlap layers");
            require(w[0x114/4]==0&&w[0x1f4/4]==0,"layer manager link remains null before callbacks");
        }
        require(!shared.registry[0]&&!shared.registry[1],"registry releases owned windows");
        {
            shared.atlas={okage::UiWindow::AtlasTuple{1,2,3,4},okage::UiWindow::AtlasTuple{}};
            okage::BattleInterface ui(shared,data,texture,0,[](unsigned){});
            require(ui.window(0).word(0x1960)==2&&ui.window(1).word(0x1960)==2,"reentry scans persistent streams");
            require(shared.atlas_dirty&&ui.window(0).word(0xc)==1,"actual runtime atlas owner used");
        }
        for(unsigned n:{0u,7u,24u,87u,88u,311u,313u}) {b=fixture();b.resize(n);rejects(b);}
        for(unsigned at:{0u,8u,12u,16u,20u,24u,56u,88u,88u+84u,88u+88u,88u+92u,88u+96u}) {
            b=fixture();put(b,at,0xdeadbeef);rejects(b);
        }
        for(unsigned i=0;i<2;++i)for(auto [offset,value]:{
            std::pair{4u,39u}, {8u,0u}, {12u,0u}, {80u,2u}, {100u,0u},
            {100u,33u}, {104u,0xffffffffu}, {108u,1u}, {16u,0x7f800000u}, {60u,0x7fc00000u}}) {
            b=fixture();put(b,88+i*112+offset,value);rejects(b);
        }
        std::vector<std::uint8_t> vars(0x1000);okage::initialize_game_variables(vars);
        auto character=[&](unsigned slot,unsigned member,unsigned id,unsigned hp,unsigned charge) {
            put(vars,0x570+slot*4,member);const unsigned at=0x5c4+slot*0x50;
            put(vars,at+0, id<<16);put(vars,at+8,hp);
            vars[at+0x26]=charge;vars[at+0x27]=0;
        };
        character(0,3,10,99,100);character(1,1,11,50,100);
        character(2,3,12,0,100);character(3,3,13,80,99);
        character(4,3,14,1,100);character(6,3,16,100,100);
        okage::UiPartySelection party;party.refresh(vars,3);
        require(party.counts()==std::array<std::uint32_t,8>{5,5,4,4,2,2,2,2},"party row counts");
        require(party.at(0,1)==11&&party.at(1,1)==1,"owned roster rows");
        require(party.at(2,1)==12&&party.at(3,1)==2,"active party rows");
        require(party.at(4,1)==14&&party.at(5,1)==4&&party.at(6,1)==3&&party.at(7,1)==4,"ready party ordinal differs from slot");
        require(party.at(2,4)==-1&&party.at(6,2)==-1,"unused selection sentinel");
        const auto unchanged=vars;party.refresh(vars,3);require(vars==unchanged,"non-demo refresh mutated variables");
        okage::set_event_variable(vars,5,4,1);party.refresh(vars,3);
        require(party.counts()==std::array<std::uint32_t,8>{1,1,1,1,1,1,1,1},"restricted party selection");
        okage::set_event_variable(vars,5,4,0);party.refresh(vars,4);
        require(party.at(5,1)==3&&party.at(6,1)==2&&party.counts()[5]==3,"demo charge refresh");
        require(vars[0x5c4+3*0x50+0x26]==100,"demo writes actual shared charge");
        character(1,0xffffffff,0xffff,0xffffffff,100);
        character(4,3,0xffff,0xffffffff,100);party.refresh(vars,3);
        require(party.counts()[0]==4&&party.at(0,3)==-1,"signed membership and ID");
        require(party.at(5,2)==4,"negative readiness word is nonzero");
        character(3,3,13,80,9);character(6,3,16,100,7);
        okage::set_event_variable(vars,5,4,1);party.refresh(vars,4);
        require(vars[0x5c4+3*0x50+0x26]==9&&vars[0x5c4+6*0x50+0x26]==7,"demo must preserve skipped slots");
        std::cout<<"battle UI data/party tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
