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
    okage::EffectDefinition cylinder;cylinder.id=26;cylinder.duration=160;cylinder.flags=0x8000;
    cylinder.elements.resize(2);
    for(unsigned i=0;i<2;++i) {
        auto& part=cylinder.elements[i];part.id=i+1;part.shape=24;part.channels=27;
        part.flags=i?0x200:0x40;part.primitive_flags=i?0x201:0x41;
        part.program=i?0x11000000:0x14000000;part.change_time=1.1f;
        part.values[0]={1,1,1,.2f};part.values[3]={1,1,1,1};
        part.values[4]=i?okage::Float4{0,1,20,0}:okage::Float4{0,1,0,1};
    }
    unsigned cylinder_rng=0;auto no_rng=[&]{++cylinder_rng;return 1u;};
    rejects([&]{okage::MapEffect invalid(cylinder,{},{},{},sizes,no_rng);});
    okage::MapEffect aura(cylinder,{},{},{1,1,1,1},sizes,no_rng,okage::EffectStart::FirstFrame);
    aura.tick(no_rng);const auto first_uv=aura.elements[0].cylinder_t;
    check(first_uv==aura.elements[1].cylinder_t&&!(aura.elements[1].program&0x3f000000));
    const auto snapshot=aura;aura.tick(no_rng);
    check(aura.elements[0].cylinder_t!=first_uv&&aura.elements[0].cylinder_t==aura.elements[1].cylinder_t);
    check(snapshot.elements[1].cylinder_t==first_uv); // Submission owns its geometry.
    for(unsigned i=0;i<159;++i)aura.tick(no_rng);
    check(aura.elements[0].cylinder_t==first_uv&&aura.elements[1].cylinder_t==first_uv&&cylinder_rng==0);
    const auto cylinders=okage::map_effect_draws(aura,identity,{},450,sine);
    check(cylinders.size()==2&&!cylinders[0].additive&&cylinders[1].additive&&cylinders[0].vertices.size()==96);
    auto bad_cylinder=aura;bad_cylinder.elements[0].interval[3]=2;
    rejects([&]{bad_cylinder.tick(no_rng);});
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
    okage::EffectDefinition torch;torch.id=91;torch.duration=9900;torch.flags=0x8000;
    torch.elements.resize(4);
    for(unsigned i=0;i<4;++i){auto& e=torch.elements[i];e.shape=i==3?40:28;
        e.flags=i==3?2:8;e.primitive_flags=1;
        e.change_time=1.1f;e.program=i==3?49u<<12:0;}
    okage::MapEffect flame(torch,{3,-4,5,1},{},{1,1,1,1},sizes,random);
    const auto phase=flame.phase;
    flame.tick(random,false);check(flame.phase==phase);
    rejects([&]{flame.tick(random);});
    const okage::Float4 invalid_eye{__builtin_bit_cast(float, (0x7fc00000u)),0,0,1};
    rejects([&]{flame.tick(random,true,&invalid_eye);});
    const okage::Float4 eye{3,-4,5,1};flame.tick(random,true,&eye);
    check(flame.phase==phase+1&&flame.elements[3].rendered[0][3]==0);
    const auto flames=okage::map_effect_draws(flame,identity,{},450,sine);
    check(flames.size()==4&&flames[0].vertices.size()==6&&flames[3].vertices.size()==48);
    check(flames[3].texture==0xffffffffu&&flames[3].additive);
    // Ordinary torch geometry cannot be submitted as a deferred job.
    flame.flags|=0x400;
    rejects([&]{okage::map_effect_draws(flame,identity,{},450,sine);});
    rejects([&]{okage::map_effect_draws(flame,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);});
    torch.id=16;torch.flags=0x8403;torch.elements.resize(3);
    for(auto& e:torch.elements)e.primitive_flags=8;
    okage::MapEffect deferred(torch,{},{},{1,1,1,1},sizes,random);
    rejects([&]{okage::map_effect_draws(deferred,identity,{},450,sine);});
    const auto deferred_draw=[&]{return okage::map_effect_draws(deferred,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);};
    const auto deferred_flames=deferred_draw();
    check(deferred_flames.size()==3);
    for(const auto& draw:deferred_flames)check(draw.vertices.size()==6&&draw.additive&&draw.texture==4);
    deferred.elements[0].primitive_flags=9;rejects(deferred_draw);
    deferred.elements[0].primitive_flags=8;
    deferred.elements[0].shape=40;deferred.elements[0].texture=0xffffffffu;
    rejects(deferred_draw);
    okage::EffectDefinition overlay;overlay.id=115;overlay.duration=500;overlay.flags=0x400;
    overlay.elements.resize(5);
    for(unsigned i=0;i<5;++i){auto& e=overlay.elements[i];e.shape=i?31:32;
        e.flags=i?0x4005:5;e.primitive_flags=0x40;}
    okage::MapEffect flash(overlay,{3,4,5,1},{.1f,.2f,.3f,0},{2,3,4,1},sizes,random,okage::EffectStart::FirstFrame);
    const auto flash_draw=[&]{return okage::map_effect_draws(flash,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);};
    auto first=flash_draw();check(first.size()==5);
    for(const auto& draw:first)check(!draw.additive&&!draw.depth_test&&draw.vertices.size()==6);
    flash.position={-1,7,90,1};flash.rotation={.2f,.7f,-.4f,0};flash.scale={9,1,3,1};
    auto shifted=identity;shifted[3]={18,-37,9,1};
    flash.elements[0].render_flags=1;
    const auto changed=okage::map_effect_draws(flash,shifted,{.3f,.7f,.2f,1},450,sine,nullptr,okage::EffectDrawPass::Deferred);
    for(unsigned i=0;i<5;++i)check(first[i].matrix==changed[i].matrix&&first[i].color==changed[i].color);
    flash.elements[0].render_flags=2;rejects(flash_draw);
    okage::EffectDefinition base=dust;base.id=19;base.flags=0x8000;base.duration=300;
    base.parameters={.03f,-.05f,0,.1f};
    okage::EffectDefinition reference;reference.id=98;reference.flags=0x800;
    reference.duration=999;reference.parameters={9,9,9,9};reference.elements.resize(1);
    reference.elements[0].id=19;reference.elements[0].channels=31;
    for(unsigned i=0;i<5;++i)reference.elements[0].values[i]={float(i+1),2,3,4};
    const auto resolved=okage::resolve_effect_references({reference,base});
    const auto& copy=resolved.front();
    check(copy.id==98&&copy.runtime_id==19&&copy.duration==300&&copy.flags==0x8000&&copy.elements.size()==9);
    check(copy.color==reference.elements[0].values[0]&&copy.rotation==reference.elements[0].values[1]);
    check(copy.position==reference.elements[0].values[2]&&copy.scale==reference.elements[0].values[3]);
    check(copy.parameters==reference.elements[0].values[4]&&resolved[1].parameters==base.parameters);
    okage::MapEffect inherited(copy,{7,8,9,1},{},{1,1,1,1},sizes,random);
    check(inherited.id==19&&inherited.duration==300&&inherited.color==copy.color);
    check(inherited.position==okage::Float4({7,8,9,1})&&inherited.rotation==okage::Float4{});
    rejects([&]{okage::MapEffect unresolved(reference,{},{},{},sizes,random);});
    rejects([&]{okage::resolve_effect_references({reference});});
    auto alias=reference;alias.id=97;alias.elements[0].id=98;
    check(okage::resolve_effect_references({reference,alias,base})[1].runtime_id==19);
    rejects([&]{okage::resolve_effect_references({alias,reference,base});});
    reference.elements[0].id=98;rejects([&]{okage::resolve_effect_references({reference,base});});
    reference.elements[0].id=19;reference.elements[0].channels=32;
    rejects([&]{okage::resolve_effect_references({reference,base});});
    reference.elements.clear();rejects([&]{okage::resolve_effect_references({reference,base});});
    std::cout<<"EFD extent, ID, pointer, finite-value and unsupported-program checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
