#include "okage/navigation_mask.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <vector>
using namespace okage;
static void require(bool value,std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("Navigation mask test line "+std::to_string(at.line()));
}
template<class F> static void rejects(F f) {
    bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}require(caught);
}
static std::vector<std::uint8_t> copy(Bytes b){return {b.begin(),b.end()};}
static void reveal() {
    NavigationMask mask(7,32,32);
    require(mask.texture_slot()==7&&mask.bytes().size()==1024);
    require(std::all_of(mask.bytes().begin(),mask.bytes().end(),[](auto b){return b==0;}));
    mask.reveal_cell(10,20,0x81);
    unsigned changed=0;
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x) {
        const bool inside=x>=9&&x<=11&&y>=19&&y<=21;
        require(mask.bytes()[y*32+x]==(inside?0x81:0));changed+=inside;
    }
    require(changed==9);
    NavigationMask low(0,32,32);low.reveal_cell(std::numeric_limits<int>::min(),-200);
    require(low.bytes()[0]==255&&low.bytes()[2*32+2]==255&&low.bytes()[3]==0);
    NavigationMask high(0,32,32);high.reveal_cell(std::numeric_limits<int>::max(),30);
    require(high.bytes()[29*32+29]==255&&high.bytes()[1023]==255&&high.bytes()[29*32+28]==0);
    const auto before=copy(high.bytes());
    rejects([&]{high.reveal_cell(2,31);}); // Source incorrectly clamps X, not Y.
    rejects([&]{high.reveal_cell(2,std::numeric_limits<int>::max());});
    require(copy(high.bytes())==before);
    rejects([]{NavigationMask m(0,16,64);}); // Equal area is not equal stride.
    rejects([]{NavigationMask m(0xffffffffu,32,32);});
}
static void records() {
    NavigationMask mask(3,32,32);
    std::vector<std::uint8_t> vars(0xd88,0xa5);
    std::fill(vars.begin()+0x878,vars.begin()+0x8f8,0);
    vars[0x878]=0x81;vars[0x879]=0x42;vars[0x8f7]=0x01;
    const auto original=vars;
    mask.restore_record(vars,0);
    for(unsigned i=0;i<1024;++i)
        require(mask.bytes()[i]==((i==0||i==7||i==9||i==14||i==1023)?255:0));
    mask.pack_record(vars,9);
    require(std::equal(vars.begin(),vars.begin()+0xcf8,original.begin()));
    require(std::equal(vars.begin()+0xcf8,vars.begin()+0xd78,original.begin()+0x878));
    require(std::equal(vars.begin()+0xd78,vars.end(),original.begin()+0xd78));
    const auto before=vars,mask_before=copy(mask.bytes());
    rejects([&]{mask.pack_record(vars,-1);});rejects([&]{mask.pack_record(vars,10);});
    rejects([&]{mask.pack_record(std::span(vars).first(0xd77),9);});
    rejects([&]{mask.restore_record(Bytes(vars).first(0x8f7),0);});
    require(vars==before&&copy(mask.bytes())==mask_before);
    NavigationMask exact(0,32,32);exact.reveal_cell(4,4,1);exact.reveal_cell(10,10,0xfe);
    exact.pack_record(vars,0);
    require(std::all_of(vars.begin()+0x878,vars.begin()+0x8f8,[](auto b){return b==0;}));
    exact.reveal_cell(4,4,0xff);exact.pack_record(vars,0);
    // Rows3,4,5 and columns3,4,5: MSB-first bits0x1c in each row's first byte.
    for(unsigned i=0;i<128;++i)require(vars[0x878+i]==((i==12||i==16||i==20)?0x1c:0));
    NavigationMask restored(99,32,32);restored.restore_record(vars,0);
    require(restored.bytes()[10*32+10]==0);
    require(restored.bytes()[4*32+4]==255);
}
int main(){try{reveal();records();std::cout<<"Navigation mask tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
