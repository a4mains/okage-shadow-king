#include "okage/name_entry.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <source_location>
#include <stdexcept>
using namespace okage;
namespace {
void require(bool value,std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("Name entry assertion line "+std::to_string(at.line()));
}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned value) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(i*8);
}
NameEntryData data() {
    std::vector<std::uint8_t> b(265);
    const std::string magic="OKNMED2";std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,2);put(b,12,1);put(b,16,28);put(b,20,112);
    const auto hash=[&](unsigned at,std::string_view text) {
        for(unsigned i=0;i<32;++i)b[at+i]=std::stoul(std::string(text.substr(i*2,2)),nullptr,16);
    };
    hash(24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    hash(56,"b8356ed62e594a4c2b1e9dc550df48200987ddd861fd8862057fa1cb3820be74");
    for(unsigned i:{0u,21u,22u,23u})put(b,88+i*4,1);
    put(b,88+24*4,9);put(b,88+25*4,32);put(b,88+26*4,16);
    b[248]='A';b[249]='r';b[250]='i';put(b,252,2);b[256]=' ';
    put(b,258,3);b[262]=0x81;b[263]=0x40;
    return NameEntryData(b);
}
std::shared_ptr<RuwResource> resource() {
    std::vector<std::uint8_t> b(96);put(b,12,1);put(b,16,18);
    return std::make_shared<RuwResource>(b);
}
struct Fixture {
    BattleUiShared shared;NameEntryShared name;NameEntryData definition=data();
    std::shared_ptr<RuwResource> ruw=resource();BattleUiPad pad;
    std::vector<std::uint8_t> variables=std::vector<std::uint8_t>(0xda0);
    std::vector<unsigned> sounds;
    NameEntryInterface manager{shared,name,definition,ruw,std::make_shared<int>(0),0};
    Fixture(){std::copy(definition.default_name.begin(),definition.default_name.end(),variables.begin()+0x844);name.selected_character={'A',0};}
    int word(unsigned at)const{return std::bit_cast<std::int32_t>(manager.words()[at/4]);}
    void tick(unsigned pressed=0,unsigned repeat=0) {
        shared.registry[manager.handle()]->advance_clock();
        manager.first_update(pad,{std::uint16_t(pressed|repeat),std::uint16_t(pressed),0,std::uint16_t(repeat)},variables,
            [&](unsigned id){sounds.push_back(id);});
    }
    std::string player()const{return reinterpret_cast<const char*>(variables.data()+0x844);}
};
void edit_and_capacity() {
    Fixture f;require(f.word(0xe4c)==0);f.tick();require(f.word(0xe4c)==1&&f.word(0xa8)==3);
    f.tick(0x40);require(f.player()=="AriA"&&f.word(0xa8)==4&&f.sounds.back()==3);
    f.tick(0x20);require(f.player()=="Ari"&&f.word(0xa8)==3&&f.sounds.back()==4);
    f.name.selected_character={' ',0};f.tick(0x40);require(f.player()=="Ari"&&f.word(0xa8)==3);
    f.name.selected_character={'B',0};for(unsigned i=0;i<4;++i)f.tick(0x40);
    require(f.player()=="AriBBBB"&&f.word(0xa8)==7&&f.word(0xcc)==-3);
    f.tick(0x40);require(f.word(0xe4c)==-1&&f.word(0xd8)==1&&f.player()=="AriBBBB");
    f.tick(0x40);require(f.word(0xe4c)==1); //Default No returns to editing.
}
void erase_empty_and_restore_default() {
    Fixture f;f.tick();for(unsigned i=0;i<3;++i)f.tick(0x20);
    require(f.word(0xa8)==0&&f.player()==" ");
    const auto sounds=f.sounds.size();f.tick(0x20);require(f.word(0xa8)==0&&f.sounds.size()==sounds);
    f.tick(0x800);require(f.player()=="Ari"&&f.word(0xa8)==3&&f.word(0xcc)==-3&&f.word(0xdc)==2&&f.word(0xe0)==-1);
}
void navigation_and_retirement() {
    Fixture f;f.tick();f.tick(0,0x1000);require(f.word(0xcc)==-1&&f.word(0xe0)==-1);
    f.tick(0,0x8000);require(f.word(0xcc)==-3);
    f.tick(0,0x4000);require(f.word(0xcc)==0&&f.word(0xe0)==0);
    f.tick(0,0x8000);require(f.word(0xcc)==12);
    f.tick(0,0x2000);require(f.word(0xcc)==0);
    f.tick(0x800);f.tick(0x40);require(f.word(0xe4c)==-1);
    f.tick(0x20);require(f.word(0xe4c)==1); //Cancel preserves name and reopens grid.
    f.tick(0x800);f.tick(0x40);f.tick(0,0x1000);require(f.word(0xd8)==0);
    f.tick(0x40);require(f.word(0xe4c)==-2&&!f.manager.dead());
    for(unsigned i=0;i<31;++i)f.tick();require(f.word(0xe4c)==-2);
    f.tick();require(f.word(0xe4c)==-3&&!f.manager.dead());
    f.tick();require(f.manager.dead()&&f.word(0xe8c)==1);
    f.manager.destroy_windows();require(!f.shared.registry[0]&&f.manager.dead());
}
}
int main(){try{edit_and_capacity();erase_empty_and_restore_default();navigation_and_retirement();
    std::cout<<"Name entry input and retirement tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
