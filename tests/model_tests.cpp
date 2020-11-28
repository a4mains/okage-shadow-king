#include "okage/model.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>

using Data=std::vector<std::uint8_t>;
void require(bool value) { if (!value) throw std::runtime_error("model test assertion failed"); }
void put(Data& b, std::size_t at, std::uint32_t v) {
    for (unsigned i=0;i<4;++i) b.at(at+i)=std::uint8_t(v>>(8*i));
}
void word(Data& b, std::uint32_t v) { auto at=b.size(); b.resize(at+4); put(b,at,v); }
template<class F> void rejects(F f) {
    try { f(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("accepted malformed model");
}
int main() {
    try {
        Data b(16+20+52+8+4+16,0);
        put(b,0,12); put(b,12,1); put(b,16,0x15);
        put(b,24,20); put(b,28,72); put(b,32,80);
        put(b,36,1); put(b,40,0x3f800000); put(b,44,0x3f800000); put(b,48,0x3f800000);
        put(b,88,1); put(b,92,0xffff0000); put(b,96,1);
        word(b,0x04000008); word(b,0);
        const auto positions=b.size();
        word(b,0x0500003c); word(b,1); word(b,3<<16);
        for (const auto v : {okage::Float4{0,0,0,1},okage::Float4{1,0,0,1},okage::Float4{0,1,0,1}})
            for (const auto x:v) word(b,__builtin_bit_cast(std::uint32_t, (x)));
        const auto primitive=b.size();
        word(b,0x0b000020); word(b,0);
        for (unsigned i=0;i<3;++i) { word(b,0xffff0000|i); word(b,0xffffffff); }
        word(b,0x01000004); put(b,20,b.size()-16);
        auto parsed=okage::read_xmd(b);
        const auto& block=parsed.models.at(0).blocks.at(0);
        require(block.polygons.size()==1 && block.polygons[0].corners[2][0]==2);
        require(block.polygons[0].corners[0][1]==-1);
        require(block.attribute_extents[0]==3 && block.attributes[0].values[2][1]==1);
        require(block.draw_offset==primitive && block.end==b.size());
        for (std::size_t n=0;n<b.size();++n)
            rejects([&]{okage::read_xmd(okage::Bytes(b).first(n));});
        auto bad=b; put(bad,positions,0x05000000);
        rejects([&]{okage::read_xmd(bad);});
        bad=b; put(bad,primitive+8,0xffff0003);
        rejects([&]{okage::read_xmd(bad);});
        bad=b; put(bad,92,0x00000000); // Parent points back to itself.
        rejects([&]{okage::read_xmd(bad);});
        bad=b; put(bad,28,20); // Aliased bone/hierarchy sections.
        rejects([&]{okage::read_xmd(bad);});
        bad=b; put(bad,positions,0x05000040); // Declared payload runs into next command.
        rejects([&]{okage::read_xmd(bad);});
        bad=b; put(bad,positions,0x1000003c); // Unknown command is never dropped.
        rejects([&]{okage::read_xmd(bad);});
        bad=b; put(bad,16,0x16); // Unverified shared-section mode.
        rejects([&]{okage::read_xmd(bad);});
        // Preserve repeated contributions to a weighted vertex; don't overwrite.
        bad=b;
        Data second(b.begin()+positions,b.begin()+primitive);
        bad.insert(bad.begin()+primitive,second.begin(),second.end());
        put(bad,20,bad.size()-16);
        require(okage::read_xmd(bad).models[0].blocks[0].attributes.size()==2);
        std::cout << "Native model boundary and topology tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
