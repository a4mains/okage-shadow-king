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
    okage::EffectDefinition map;map.id=25;map.duration=400;map.flags=0x9000;map.elements.resize(8);
    for(unsigned i=0;i<8;++i){map.elements[i].shape=i<3?24:29;map.elements[i].primitive_flags=i<3?1:0x101;}
    okage::EffectTextureSizes sizes;for(auto& size:sizes)size={64,64};sizes[5]={32,24};sizes[13]={32,32};
    okage::MapEffect job(map,{0,0,420,1},{0,0,0,0},{1,1,1,1},sizes,[]{return 1;});
    const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    const auto sine=okage::make_sine_table();
    const auto draw=[&]{return okage::map_effect_draws(job,identity,{0,0,0,0},450,sine);};
    auto draws=draw();check(draws.size()==8&&draws[0].vertices.size()==96&&draws[3].vertices.size()==6);
    check(draws[0].color[3]==.75f);check(std::fegetround()==rounding);
    // Texture replacement uses fixed5-bit ratios, not ideal float scaling.
    job.elements[3].texture=13;job.elements[3].uv={0,0,31,31};
    draws=draw();check(draws[3].vertices[2].uv[1]==1050.f/32768);
    job.flags|=0x100;check(draw().empty());job.flags&=~0x100u;
    job.flags|=0x400;rejects(draw);job.flags&=~0x400u;
    job.elements[0].flags=2;rejects(draw);job.elements[0].flags=0;
    job.elements[0].primitive_flags=0;rejects(draw);job.elements[0].primitive_flags=1;
    job.elements[0].shape=31;rejects(draw);
    okage::EffectDefinition dust;dust.id=17;dust.duration=30;dust.elements.resize(9);
    for(unsigned i=0;i<9;++i){auto& element=dust.elements[i];element.id=i+1;element.shape=31;
        element.flags=i?0x400601:0x400401;element.primitive_flags=0x41;
        element.program=i?0x1104c114:0x4c114;element.change_time=1.1f;
    }
    unsigned random_calls=0;auto random=[&]{++random_calls;return 7u;};
    rejects([&]{okage::MapEffect invalid(dust,{},{},{},sizes,random);});
    okage::MapEffect puff(dust,{1,2,3,1},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
    check(puff.phase==1&&random_calls==0);
    rejects([&]{okage::map_effect_draws(puff,identity,{},450,sine);});
    const auto tint=okage::effect_light_tint({1,1,1,1},{.5,.5,.5,1},{1,1,1,0});
    check(tint==okage::Float4({1,1,1,1}));
    rejects([&]{okage::effect_light_tint({},{},{});});
    const auto dust_draws=okage::map_effect_draws(puff,identity,{},450,sine,&tint);
    check(dust_draws.size()==9&&!dust_draws[0].additive&&dust_draws[0].vertices.size()==6);
    puff.tick(random);check(random_calls==36&&puff.phase==2&&!(puff.flags&0x100));
    for(unsigned i=0;i<29;++i)puff.tick(random);
    check(random_calls==36&&puff.phase==31&&!(puff.flags&0x100));
    puff.tick(random);check(puff.phase==31&&(puff.flags&0x100));
    check(okage::map_effect_draws(puff,identity,{},450,sine,&tint).empty());
    rejects([&]{puff.tick(random);});
    std::cout<<"EFD extent, ID, pointer, finite-value and unsupported-program checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
