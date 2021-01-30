#include "okage/effect.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
using Data=std::vector<std::uint8_t>;
void check(bool v){if(!v)throw std::runtime_error("Effect test failed");}
void put(Data& b,std::size_t p,std::uint32_t v){for(unsigned j=0;j<4;++j)b.at(p+j)=v>>(j*8);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Invalid effect accepted");}
int main(){try{
    Data b(16+48+2*116);put(b,0,0x20444645);put(b,4,1);put(b,16,30);put(b,20,250);
    put(b,24,0x8400);put(b,32,2);put(b,36,64);
    for(unsigned i=0;i<2;++i){const auto p=64+i*116;
        put(b,p,i?0x241:1);put(b,p+4,i+1);put(b,p+8,34);put(b,p+12,i?0x140:0x40);
        put(b,p+16,i?29:25);put(b,p+20,i?0x1961e000:0x600000);put(b,p+24,14+i);
        put(b,p+28,__builtin_bit_cast(std::uint32_t, (1.1f)));put(b,p+104,__builtin_bit_cast(std::uint32_t, (1.0f)));
    }
    const auto bank=okage::read_efd(b);check(bank.size()==1&&bank[0].elements.size()==2);
    for(std::size_t n=0;n<b.size();++n)rejects([&]{okage::read_efd(okage::Bytes(b).first(n));});
    for(auto [p,v]:{std::pair{0u,0u},{4u,129u},{16u,128u},{32u,0xffffffffu},{36u,68u},{92u,0x7f800000u}}){
        auto bad=b;put(bad,p,v);rejects([&]{okage::read_efd(bad);});
    }
    auto tail=b;tail.push_back(0);rejects([&]{okage::read_efd(tail);});
    auto duplicated=b;duplicated.insert(duplicated.end(),b.begin()+16,b.end());put(duplicated,4,2);
    rejects([&]{okage::read_efd(duplicated);});
    for(int parameter:{-1,32})rejects([&]{okage::BubbleEffect e(bank[0],parameter);});
    auto unsupported=bank[0];unsupported.elements[1].program^=1;
    rejects([&]{okage::BubbleEffect e(unsupported,0);});
    okage::BubbleEffect e(bank[0],0);const auto rounding=std::fegetround();
    e.tick({0,0,0,0});check(std::fegetround()==rounding);
    rejects([&]{e.tick({__builtin_bit_cast(float, (0x7fc00000u)),0,0,0});});
    e.request_removal();rejects([&]{e.tick({0,0,0,0});});
    std::cout<<"EFD extent, ID, pointer, finite-value and unsupported-program checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
