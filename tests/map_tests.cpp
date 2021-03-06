#include "okage/map.hpp"
#include <bit>
#include <cfenv>
#include <cstring>
#include <iostream>
#include <stdexcept>
using Data=std::vector<std::uint8_t>;
void require(bool v) { if(!v) throw std::runtime_error("map test failed"); }
void put(Data& b,std::size_t at,std::uint32_t v) { for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i); }
template<class F> void rejects(F f) {try {f();} catch(const std::runtime_error&) {return;} throw std::runtime_error("Malformed map accepted");}
int main() {try {
    Data b(64+80+148,0);put(b,8,64);put(b,12,1);put(b,40,144);put(b,44,1);
    std::memcpy(b.data()+144,"room.xmd",9);put(b,144+28,0x80);
    auto f=[&](unsigned word,float v){put(b,64+word*4,__builtin_bit_cast(std::uint32_t, (v)));};
    f(4,3);f(5,2);f(6,-4);f(8,90);f(9,180);f(10,-90);f(12,1);f(13,1);f(14,1);
    auto m=okage::read_xmp(b);require(m.definitions[0].name=="room.xmd");
    const int before=std::fegetround();auto t=okage::map_transform(m.placements[0],m.definitions[0]);
    require(std::fegetround()==before && t.position==okage::Float4{3,-2,4,1});
    require(t.rotation[0]>0.24999f && t.rotation[0]<=0.25f && t.flags==0x100080);
    for (std::size_t i=0;i<b.size();++i) rejects([&]{okage::read_xmp(okage::Bytes(b).first(i));});
    for (const auto [at,value]:{std::pair{44U,641U},std::pair{8U,0xfffffff0U},std::pair{40U,64U},
        std::pair{72U,1U},std::pair{80U,0x7fc00000U}}) {
        auto bad=b;put(bad,at,value);rejects([&]{okage::read_xmp(bad);});
    }
    auto bad=b;std::memset(bad.data()+144,'a',28);rejects([&]{okage::read_xmp(bad);});
    bad=b;std::memcpy(bad.data()+144,"../x.xmd",9);rejects([&]{okage::read_xmp(bad);});
    // Special handler bits are retained, not interpreted as ordinary geometry.
    auto p=m.placements[0];p.words[3]=0x5b100000;
    require(p.handler()==1 && p.handler_argument()==0x10005b);
    p.words[12]=__builtin_bit_cast(std::uint32_t, (2.0f));p.words[14]=0;
    require(okage::map_transform(p,m.definitions[0]).flags==0x5b100080);
    auto ordinary=m.placements[0];ordinary.words[12]=__builtin_bit_cast(std::uint32_t, (2.f));
    auto effect=m.placements[0];effect.words[3]=0x100000;
    m.placements={m.placements[0],ordinary,effect,ordinary,m.placements[0]};
    require(okage::map_instance_ids(m)==std::vector<int>({2047,0,-1,1,2046}));
    m.placements[2].words[3]=0x200000;rejects([&]{okage::map_instance_ids(m);});
    m.placements.assign(2049,ordinary);rejects([&]{okage::map_instance_ids(m);});
    std::cout<<"Map bounds, references, handler metadata and coordinate tests passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
