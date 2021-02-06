#include "okage/collision.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
using Data=std::vector<std::uint8_t>;
void check(bool b){if(!b)throw std::runtime_error("Collision test failed");}
void put(Data& b,unsigned at,std::uint32_t v,unsigned size=4){for(unsigned i=0;i<size;++i)b.at(at+i)=v>>(i*8);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Invalid XCL accepted");}
int main(){try {
    Data b(164);
    for(unsigned i=0;i<3;++i){put(b,i*4,8);put(b,12+i*4,3);put(b,48+i*4,1);}
    put(b,60,64);put(b,64,68);put(b,68,72);put(b,72,76);
    put(b,76,3,2);put(b,78,1,2);put(b,80,96);put(b,84,144);
    for(unsigned i=0;i<3;++i)put(b,96+i*16+12,__builtin_bit_cast(std::uint32_t, (1.f)));
    put(b,144+4,0xc000,2);put(b,144+10,0,2);put(b,144+12,1,2);put(b,144+14,2,2);
    // Absolute slot, contiguous run, byte skip, contiguous run, unused tag.
    b.insert(b.end(),{0xc4,60,0,0,0,2,0,0,0,164,0,0,0});
    const auto m=okage::read_xcl(b);check(m.chunks.size()==1&&m.chunks[0].vertices.size()==3&&m.pointer_locations.size()==6);
    const auto rounding=std::fegetround();check(m.query({1,1,1,0},{2,2,2,0})==std::vector<std::uint32_t>{0});
    check(std::fegetround()==rounding);check(m.query({-1,-1,-1,0},{-1,-1,-1,0}).empty());
    check(m.query({2,2,2,0},{1,1,1,0},1)==std::vector<std::uint32_t>{0});
    auto wide=b;wide.resize(164);
    // Same relocation sequence, using the two-byte skip encoding.
    wide.insert(wide.end(),{0xc8,60,0,0,0,2,0,0,0,0,164,0,0,0});
    check(okage::read_xcl(wide).pointer_locations==m.pointer_locations);
    auto grid=m;grid.header[12]=grid.header[13]=grid.header[14]=2;
    grid.cells={0,1,2,3,4,-1,0,5};
    check(grid.query({0,0,0,0},{8,8,8,0},0)==std::vector<std::uint32_t>({0,1,2,3,4,0,5}));
    check(grid.query({0,0,0,0},{8,8,8,0},1)==std::vector<std::uint32_t>({1,0,3,2,4,5,0}));
    check(grid.query({8,8,8,0},{0,0,0,0},2)==grid.query({0,0,0,0},{8,8,8,0},0));
    grid.header[12]=grid.header[14]=1;grid.header[13]=65;grid.cells.assign(65,0);
    rejects([&]{grid.query({0,0,0,0},{0,512,0,0});});
    rejects([&]{m.query({__builtin_bit_cast(float, (0x7fc00000u)),0,0,0},{},0);});
    check(std::fegetround()==rounding);
    rejects([&]{m.query({}, {},3);});
    for(unsigned n=0;n<69;++n)rejects([&]{okage::read_xcl(okage::Bytes(b).first(n));});
    for(auto [at,value]:{std::pair{12u,31u},{48u,0xffffffffu},{80u,164u},{96u,0x7f800000u}}) {
        auto bad=b;put(bad,at,value);rejects([&]{okage::read_xcl(bad);});
    }
    auto bad=b;put(bad,158,3,2);rejects([&]{okage::read_xcl(bad);});
    bad=b;put(bad,165,61);rejects([&]{okage::read_xcl(bad);});
    bad=b;put(bad,bad.size()-4,0xffffffff);rejects([&]{okage::read_xcl(bad);});
    bad=b;bad[164]=0;rejects([&]{okage::read_xcl(bad);});
    std::cout<<"XCL grid/geometry, relocation bounds, finite queries and malformed input checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
