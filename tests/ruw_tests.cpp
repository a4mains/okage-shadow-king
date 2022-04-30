#include "okage/ruw.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool b){if(!b)throw std::runtime_error("RUW test failed");}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected RUW rejection");}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t v) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(i*8);
}
}
int main(){try {
    std::vector<std::uint8_t> b(32+16+7*68);
    put(b,8,1);put(b,12,1);put(b,16,2);
    put(b,20,1);put(b,24,2);put(b,28,4);
    for(unsigned i=0;i<4;++i)put(b,32+i*4,100+i);
    for(unsigned i=0;i<7;++i)for(unsigned j=0;j<17;++j)put(b,48+i*68+j*4,1000+i*100+j);
    const std::array<unsigned,6> types={0,1,2,3,0,4},flags={1,1,0x41,0x40,0,1};
    for(unsigned i=0;i<6;++i){put(b,116+i*68+16,flags[i]);put(b,116+i*68+20,types[i]);}
    okage::RuwResource r(b);b.assign(b.size(),0); // All ownership is independent.
    require(r.styles().size()==1&&r.styles()[0][3]==103);
    require(r.templates()[0][0].words[16]==1016&&!r.templates()[0][0].runtime);
    const auto& first=r.widgets()[0][0];const auto& text=r.widgets()[0][1];
    require(!first.runtime&&first.words[4]==0x8001&&first.words[16]==0);
    require(text.runtime&&text.runtime->text&&text.runtime->words[1]==1&&text.words[4]==0x8001);
    require(std::string(text.runtime->text->data())=="\\f[1]abcdefg");
    require(text.runtime->words==std::array<std::uint32_t,10>{1207,1,1200,1201,1202,1203,1211,1212,1213,1214});
    require(r.widgets()[1][0].runtime&&!r.widgets()[1][0].runtime->text);
    require(r.widgets()[1][1].runtime&&!r.widgets()[1][1].runtime->text);
    require(!r.widgets()[1][2].runtime&&!r.widgets()[1][3].runtime);
    require(r.widgets()[1][3].words[4]==1&&r.widgets()[1][3].words[16]==1616);
    put(b,8,1);put(b,12,1);put(b,16,1);put(b,20,1);put(b,24,1);
    for(unsigned n:{0,16,24,40,100,179})rejects([&]{okage::RuwResource bad(okage::Bytes(b).first(n));});
    put(b,24,0xffff);rejects([&]{okage::RuwResource bad(b);});
    put(b,24,0x7fff);rejects([&]{okage::RuwResource bad(b);});
    put(b,12,0);rejects([&]{okage::RuwResource bad(b);});
    std::cout<<"RUW tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
