#include "okage/effect.hpp"
#include "okage/assets.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
using Data=std::vector<std::uint8_t>;
void check(bool v){if(!v)throw std::runtime_error("Effect test failed");}
void put(Data& b,std::size_t p,std::uint32_t v){for(unsigned j=0;j<4;++j)b.at(p+j)=v>>(j*8);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Invalid effect accepted");}
int main(int argc,char** argv){try{
    Data b(16+48+2*116);put(b,0,0x20444645);put(b,4,1);put(b,16,30);put(b,20,250);
    put(b,24,0x8400);put(b,32,2);put(b,36,64);
    for(unsigned i=0;i<2;++i){const auto p=64+i*116;
        put(b,p,i?0x241:1);put(b,p+4,i+1);put(b,p+8,34);put(b,p+12,i?0x140:0x40);
        put(b,p+16,i?29:25);put(b,p+20,i?0x1961e000:0x600000);put(b,p+24,14+i);
        put(b,p+28,std::bit_cast<std::uint32_t>(1.1f));put(b,p+104,std::bit_cast<std::uint32_t>(1.0f));
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
    rejects([&]{e.tick({std::bit_cast<float>(0x7fc00000u),0,0,0});});
    e.request_removal();rejects([&]{e.tick({0,0,0,0});});
    okage::EffectDefinition map;map.id=25;map.duration=400;map.flags=0x9000;map.elements.resize(8);
    for(unsigned i=0;i<8;++i){map.elements[i].shape=i<3?24:29;map.elements[i].primitive_flags=i<3?1:0x101;}
    okage::EffectTextureSizes sizes;for(auto& size:sizes)size={64,64};sizes[5]={32,24};sizes[13]={32,32};
    okage::MapEffect job(map,{0,0,420,1},{0,0,0,0},{1,1,1,1},sizes,[]{return 1;});
    {
        okage::RetiringSceneEffects effects;
        effects.map[4]=std::make_unique<okage::MapEffect>(job);effects.map[4]->flags=0x4020;
        effects.map[80]=std::make_unique<okage::MapEffect>(job);effects.map[80]->flags=0x220;
        effects.mark();check(effects.flags()[4]==0x4120&&effects.flags()[80]==0x320);
        effects.tick();check(effects.flags()[4]==0x4320&&!effects.map[80]);
        effects.tick();check(!effects.map[4]);
    }
    const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    const auto sine=okage::make_sine_table();
    okage::EffectDefinition water;water.id=106;water.duration=600;water.flags=0x8400;
    water.parameters={32,32,3,1};water.elements.resize(1);
    auto& water_part=water.elements[0];water_part.id=1;water_part.shape=65;
    water_part.primitive_flags=0x41;water_part.program=0x20000000;water_part.change_time=1.1f;
    unsigned water_rng=0;const auto water_random=[&]{++water_rng;return 1u;};
    okage::MapEffect pool(water,{},{},{1,1,1,1},sizes,water_random);
    const auto initial_pool=pool;
    pool.tick(water_random,false);
    check(pool.elements[0].water==initial_pool.elements[0].water&&pool.near_clip==3);
    for(unsigned i=0;i<51;++i)pool.tick(water_random);
    const auto& pool_surface=*pool.elements[0].water;
    check(water_rng==1&&pool.near_clip==10&&pool.clamp_mode==3);
    check(pool_surface.packed_y[30*32+16]==-15);
    check(!initial_pool.elements[0].water->initialized&&initial_pool.elements[0].water->packed_y[30*32+16]==0);
    const auto submitted_pool=pool;
    const auto submitted_uv=pool.elements[0].water->packed_uv;
    for(unsigned i=0;i<3;++i)pool.tick(water_random);
    check(submitted_pool.elements[0].water->packed_uv==submitted_uv);
    check(pool.elements[0].water!=submitted_pool.elements[0].water);
    const auto water_draw=okage::map_effect_draws(pool,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);
    check(water_draw.size()==1&&water_draw[0].vertices.size()==31*31*6&&water_draw[0].clamp_mode==3);
    check(water_draw[0].depth_test&&!water_draw[0].additive&&!water_draw[0].subtractive);
    auto unsupported_water=pool;unsupported_water.parameters[3]=2;
    rejects([&]{unsupported_water.tick(water_random);});
    unsigned shape_rng=0;
    const auto shape_random=[&]{++shape_rng;return shape_rng;};
    const auto noise=okage::make_effect_noise_grid(shape_random);
    check(shape_rng==384);unsigned noise_vertices=0;
    for(unsigned shape=10;shape<=16;++shape)noise_vertices+=okage::make_effect_noise_shape(shape,noise).size();
    check(noise_vertices==6*8*8*6&&shape_rng==384);
    check(okage::make_effect_noise_shape(17,noise).empty());
    rejects([&]{okage::make_effect_noise_grid({});});
    rejects([&]{okage::make_effect_noise_shape(9,noise);});
    auto bad_noise=noise;bad_noise[0]=7;
    rejects([&]{okage::make_effect_noise_shape(10,bad_noise);});
    shape_rng=0;
    const auto rounding_before=std::fegetround();
    const auto sphere=okage::make_shadow_effect_shape(3,sine,shape_random);
    check(sphere.size()==672&&shape_rng==0&&std::fegetround()==rounding_before);
    const auto edge=okage::make_shadow_effect_shape(55,sine,shape_random);
    check(edge.size()==240&&shape_rng==20&&std::fegetround()==rounding_before);
    rejects([&]{okage::make_shadow_effect_shape(55,sine,{});});
    rejects([&]{okage::make_shadow_effect_shape(56,sine,shape_random);});
    okage::EffectDefinition shadow;shadow.id=24;shadow.duration=80;shadow.elements.resize(8);
    for(unsigned i=0;i<8;++i){auto& e=shadow.elements[i];e.id=i+1;
        e.shape=i<3?25:i==3?3:i==4?55:31;e.primitive_flags=0x21;}
    okage::MapEffect transition(shadow,{},{},{1,1,1,1},sizes,shape_random,okage::EffectStart::FirstFrame);
    rejects([&]{okage::map_effect_draws(transition,identity,{},450,sine);});
    transition.shape55_geometry=std::make_shared<const std::vector<okage::MapEffectVertex>>(edge);
    const auto shadow_draws=okage::map_effect_draws(transition,identity,{},450,sine);
    check(shadow_draws.size()==8&&shape_rng==20); // Drawing never consumes template RNG.
    for(const auto& draw:shadow_draws)check(draw.subtractive&&draw.depth_test&&!draw.additive);
    unsigned prepared_rng=0;auto preparation_random=[&]{return ++prepared_rng;};
    okage::EffectDefinition initial;initial.id=120;
    for(unsigned shape:{55u,43u,56u,62u,45u}) {
        okage::EffectElement e;e.id=initial.elements.size()+1;e.shape=shape;
        e.texture=shape==56?30:shape==62?10:-1;initial.elements.push_back(e);
    }
    auto duplicate=initial;duplicate.id=24;
    auto prepared_reference=initial;prepared_reference.id=22;prepared_reference.flags=0x800;
    const okage::EffectTemplatePreparation prepared({initial,duplicate,prepared_reference},sizes,sine,preparation_random);
    check(prepared_rng==544&&prepared.cache_owners[55]==std::array<std::uint32_t,2>{24,1});
    check(prepared.geometry[43]->vertices.size()==24&&prepared.geometry[56]->vertices.size()==240&&
          prepared.geometry[62]->vertices.size()==60&&prepared.geometry[55]->position_tags.size()==240);
    check(std::fegetround()==rounding_before);
    rejects([&]{okage::make_procedural_effect_shape(62,sine,preparation_random,{});});
    rejects([&]{okage::make_procedural_effect_shape(43,sine,{},{});});
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
    const okage::Float4 invalid_eye{std::bit_cast<float>(0x7fc00000u),0,0,1};
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
    okage::EffectDefinition dark;dark.id=6;dark.duration=70;dark.flags=0x405;dark.elements.resize(4);
    for(unsigned i=0;i<4;++i){auto& e=dark.elements[i];e.id=i+1;e.shape=i%2?9:7;
        e.primitive_flags=0x20;e.program=0x80003;e.change_time=1.1f;}
    unsigned dark_rng=0;auto dark_random=[&]{++dark_rng;return 1u;};
    okage::MapEffect rings(dark,{1,2,3,1},{},{1,1,1,1},sizes,dark_random,okage::EffectStart::FirstFrame);
    const auto dark_draw=[&]{return okage::map_effect_draws(rings,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);};
    rejects([&]{okage::map_effect_draws(rings,identity,{},450,sine);});
    const auto ring_draws=dark_draw();check(ring_draws.size()==4);
    for(unsigned i=0;i<4;++i)check(ring_draws[i].subtractive&&!ring_draws[i].additive&&!ring_draws[i].depth_test&&
        ring_draws[i].texture==0xffffffffu&&ring_draws[i].vertices.size()==(i%2?240:120));
    rings.elements[0].primitive_flags=0x40;rejects(dark_draw);rings.elements[0].primitive_flags=0x20;
    for(unsigned i=0;i<70;++i)rings.tick(dark_random);
    check(rings.phase==71&&!(rings.flags&0x100)&&dark_rng==0);
    rings.tick(dark_random);check((rings.flags&0x100)&&dark_draw().empty()&&dark_rng==0);
    rejects([&]{rings.tick(dark_random);});
    okage::EffectDefinition strike;strike.id=38;strike.duration=35;strike.flags=0x400;
    strike.elements.resize(3);
    for(unsigned i=0;i<3;++i) {
        auto& part=strike.elements[i];part.id=i+1;part.shape=i==2?30:31;
        part.flags=0x401;part.primitive_flags=i==0?0x100:0;part.channels=25;
        part.program=((i==2?23u:6u)<<18)|(4u<<6)|(i==0?17u:i==1?20u:5u);
        part.change_time=1.1f;part.values[0]={.3f,.4f,.5f,.75f};
        part.values[3]={2,5,7,.25f};part.values[4]={0,1,1,0};
        part.texture=5;
    }
    auto strike_sizes=sizes;strike_sizes[17]={64,24};strike_sizes[1]={48,40};strike_sizes[6]={32,16};
    unsigned strike_rng=0;const auto strike_random=[&]{return (++strike_rng)*91u;};
    okage::MapEffect hit(strike,{9,-7.5f,11,1},{},{1,1,1,1},strike_sizes,strike_random,okage::EffectStart::FirstFrame);
    check(strike_rng==0&&hit.phase==1&&hit.elements[2].texture==6);
    hit.tick(strike_random,false);check(strike_rng==0&&hit.phase==1);
    const auto draw_hit=[&]{return okage::map_effect_draws(hit,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);};
    rejects([&]{okage::map_effect_draws(hit,identity,{},450,sine);});
    hit.tick(strike_random);check(strike_rng==3&&hit.phase==2);
    const auto hit_rounding=std::fegetround();check(std::fesetround(FE_TOWARDZERO)==0);
    for(unsigned i=0;i<3;++i) {
        volatile float degrees=float((i+1)*91+360);
        const float expected=degrees*.00278f;
        check(hit.elements[i].rendered[1][2]==expected);
    }
    check(std::fesetround(hit_rounding)==0);
    check(hit.elements[0].texture==1&&hit.elements[1].texture==17&&hit.elements[2].texture==6);
    check(hit.elements[0].quad_uv[2]==std::array<std::uint16_t,2>{1536,1325});
    check(hit.elements[1].quad_uv[2]==std::array<std::uint16_t,2>{2048,800});
    const auto initial_hit=hit;const auto draws_hit=draw_hit();check(draws_hit.size()==3&&strike_rng==3);
    for(unsigned i=0;i<3;++i) {
        check(draws_hit[i].additive&&!draws_hit[i].subtractive&&!draws_hit[i].depth_test);
        check(draws_hit[i].vertices.size()==6);
        //Original B,A,C,A,C,D triangle order and centeredshape30 height.
        const std::array<okage::Float4,6> expected={okage::Float4{.5f,-.5f,0,1},
            {-.5f,-.5f,0,1},{.5f,.5f,0,1},{-.5f,-.5f,0,1},{.5f,.5f,0,1},{-.5f,.5f,0,1}};
        for(unsigned j=0;j<6;++j)check(draws_hit[i].vertices[j].position==expected[j]);
    }
    check(draws_hit[0].vertices[2].uv==okage::Float4({1536/32768.f,1325/32768.f,0,0}));
    check(draws_hit[2].vertices[2].uv==okage::Float4({1024/32768.f,544/32768.f,0,0}));
    auto scale_hit=hit;scale_hit.duration=4;scale_hit.inverse_duration=.25f;scale_hit.phase=1;
    scale_hit.tick(strike_random);
    check(scale_hit.elements[2].rendered[2]==okage::Float4({1.5f,1.5f,1.5f,.25f}));
    check(strike_rng==3);
    for(unsigned i=1;i<35;++i)hit.tick(strike_random);
    check(hit.phase==36&&!(hit.flags&0x100)&&strike_rng==3);
    check(hit.elements[0].quad_uv==initial_hit.elements[0].quad_uv);
    for(unsigned i=0;i<3;++i)check(hit.elements[i].rendered[1]==initial_hit.elements[i].rendered[1]);
    hit.tick(strike_random);check((hit.flags&0x100)&&draw_hit().empty()&&strike_rng==3);
    auto bad_strike=initial_hit;bad_strike.elements[0].primitive_flags=0x40;
    rejects([&]{okage::map_effect_draws(bad_strike,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);});
    rejects([&]{okage::MapEffect bad(strike,{},{},{1,1,1,1},sizes,strike_random);});
    okage::EffectProjectionContext numeric_projection{identity,{},40};
    numeric_projection.screen[0]={1,1,1,0};
    numeric_projection.screen[1]={0,0,0,1};
    numeric_projection.screen[4]={2048,2048,0,0};
    numeric_projection.screen[5]={1,1,1,0};
    okage::EffectDefinition numbers;numbers.id=35;numbers.duration=90;numbers.flags=0x400;
    numbers.parameters={0,0,0,573};numbers.elements.resize(1);
    auto& number_part=numbers.elements[0];number_part.id=4;number_part.shape=33;
    number_part.flags=0x245;number_part.primitive_flags=0x40;number_part.channels=13;
    number_part.program=0x1a03201e;number_part.change_time=1.1f;number_part.texture=12;
    number_part.values[0]={.7f,.7f,.7f,.5f};number_part.values[2]={0,-1.5f,0,0};
    number_part.values[3]={.7f,.7f,1,1};
    auto number_sizes=sizes;number_sizes[12]={256,32};number_sizes[6]={32,16};
    unsigned numeric_rng=0;const auto numeric_random=[&]{++numeric_rng;return 50u;};
    const auto make_number=[&](int amount){
        okage::MapEffect result(numbers,{40,0,10,99},{.25f,.5f,0,1},{3,4,5,1},number_sizes,numeric_random,okage::EffectStart::FirstFrame);
        result.set_numeric_parameter(amount);return result;
    };
    auto digits=make_number(573);
    check(digits.parameters==okage::Float4({573,573,573,573})&&digits.scale==okage::Float4({3,4,5,1}));
    digits.tick(numeric_random,false);check(numeric_rng==0&&digits.phase==1);
    rejects([&]{digits.tick(numeric_random);});check(numeric_rng==0&&digits.phase==1);
    digits.tick(numeric_random,true,nullptr,&numeric_projection);
    check(numeric_rng==1&&digits.phase==2&&!(digits.elements[0].program&0x3f000000));
    check(digits.elements[0].rendered[0]==okage::Float4({1,0,1,0}));
    for(unsigned q=0;q<3;++q) {
        const unsigned cell=std::array<unsigned,3>{5,7,3}[q];
        const auto& uv=digits.elements[0].number_uv;
        check(uv[q*4]==std::array<std::uint16_t,2>{std::uint16_t(cell*512),0});
        check(uv[q*4+2]==std::array<std::uint16_t,2>{std::uint16_t(cell*512+512),528});
    }
    const auto draw_number=[&](const okage::MapEffect& job,const okage::Matrix4& view){
        return okage::map_effect_draws(job,view,{.4f,.3f,0,1},450,sine,nullptr,okage::EffectDrawPass::Deferred);
    };
    rejects([&]{okage::map_effect_draws(digits,identity,{},450,sine);});
    auto shifted_view=identity;shifted_view[3]={99,88,77,1};
    const auto numeric_draws=draw_number(digits,shifted_view);
    check(numeric_draws.size()==1&&numeric_draws[0].vertices.size()==18);
    check(numeric_draws[0].matrix==draw_number(digits,identity)[0].matrix);
    check(numeric_draws[0].matrix[3]==okage::Float4({1,0,1,1})); //No second job/view transform.
    check(!numeric_draws[0].depth_test&&!numeric_draws[0].additive&&!numeric_draws[0].subtractive);
    for(unsigned q=0;q<3;++q) {
        const auto& vertices=numeric_draws[0].vertices;
        check(vertices[q*6].position==okage::Float4({float(q)-.5f,-.5f,0,1}));
        check(vertices[q*6+1].position==okage::Float4({float(q)-1.5f,-.5f,0,1}));
        check(vertices[q*6+2].position==okage::Float4({float(q)-.5f,.5f,0,1}));
    }
    const auto first_number_uv=digits.elements[0].number_uv;
    digits.set_numeric_parameter(999);digits.position[0]=80;
    digits.tick(numeric_random,true,nullptr,&numeric_projection);
    check(numeric_rng==1&&digits.elements[0].number_uv==first_number_uv);
    check(digits.elements[0].rendered[0][0]==2);
    for(unsigned i=2;i<90;++i)digits.tick(numeric_random,true,nullptr,&numeric_projection);
    check(digits.phase==91&&!(digits.flags&0x100)&&numeric_rng==1);
    digits.tick(numeric_random,true,nullptr,&numeric_projection);
    check((digits.flags&0x100)&&draw_number(digits,identity).empty()&&numeric_rng==1);
    for(const auto& [amount,cells]:std::array<std::pair<int,std::array<unsigned,3>>,5>{{
            {0,{11,11,0}},{7,{11,11,7}},{10,{11,1,0}},{-573,{5,7,3}},{1234,{2,3,4}}}}) {
        auto example=make_number(amount);example.tick(numeric_random,true,nullptr,&numeric_projection);
        for(unsigned q=0;q<3;++q)check(example.elements[0].number_uv[q*4][0]==cells[q]*512);
        if(amount<0)check(example.elements[0].color==okage::Float4({.2f,.7f,.2f,.5f}));
    }
    auto conversion=make_number(16777217);check(conversion.parameters[3]==16777216.f);
    conversion.set_numeric_parameter(-16777217);check(conversion.parameters[3]==-16777216.f);
    check(std::fegetround()==rounding_before);
    //Source color30 equality preserves prior RGB; it only clears alpha.
    auto terminal_number=make_number(9);terminal_number.inverse_duration=.25f;terminal_number.duration=4;
    terminal_number.phase=2;terminal_number.tick(numeric_random,true,nullptr,&numeric_projection);
    const auto earlier_color=terminal_number.elements[0].rendered[3];
    check(earlier_color[0]<.7f&&earlier_color[2]>.7f&&earlier_color[3]<.5f);
    terminal_number.phase=4;terminal_number.tick(numeric_random,true,nullptr,&numeric_projection);
    for(unsigned i=0;i<3;++i)check(terminal_number.elements[0].rendered[3][i]==earlier_color[i]);
    check(terminal_number.elements[0].rendered[3][3]==0);
    auto clipped_number=make_number(1);clipped_number.position[2]=-2;
    clipped_number.tick(numeric_random,true,nullptr,&numeric_projection);
    check(clipped_number.elements[0].rendered[0]==okage::Float4({2088,100,-1,0}));
    auto projected_number=make_number(1);auto altered_projection=numeric_projection;
    altered_projection.view[3][0]=40;altered_projection.screen[1][3]=2;
    projected_number.tick(numeric_random,true,nullptr,&altered_projection);
    check(projected_number.elements[0].rendered[0][0]==1); //View X80 divided byW2.
    auto invalid_projection=numeric_projection;invalid_projection.focal=0;
    auto rejected_number=make_number(1);const auto before_invalid=numeric_rng;
    rejects([&]{rejected_number.tick(numeric_random,true,nullptr,&invalid_projection);});
    check(numeric_rng==before_invalid);
    auto miss=numbers;miss.id=36;miss.parameters={1,1,1,1};
    auto& miss_part=miss.elements[0];miss_part.id=1;miss_part.shape=30;miss_part.texture=5;
    miss_part.primitive_flags=0x140;miss_part.channels=29;miss_part.program=0x1b03201e;
    miss_part.values[0]={.5f,.5f,.5f,.5f};miss_part.values[3]={2.1f,.7f,1.2f,1};
    miss_part.values[4]={0,1,12,0};
    okage::MapEffect missed(miss,{0,0,10,1},{},{1,1,1,1},number_sizes,numeric_random,okage::EffectStart::FirstFrame);
    check(missed.elements[0].texture==6);const auto before_miss=numeric_rng;
    missed.tick(numeric_random,true,nullptr,&numeric_projection);
    check(numeric_rng==before_miss+1&&missed.elements[0].texture==12);
    check(missed.elements[0].quad_uv==std::array<std::array<std::uint16_t,2>,4>{{{6144,0},{6841,0},{6841,68},{6144,68}}});
    const auto miss_draw=draw_number(missed,identity);check(miss_draw.size()==1&&miss_draw[0].vertices.size()==6);
    check(miss_draw[0].vertices[2].uv==okage::Float4({6841/32768.f,68/32768.f,0,0}));
    auto tiny_sizes=number_sizes;tiny_sizes[12]={1,32};
    okage::MapEffect tiny_miss(miss,{0,0,10,1},{},{1,1,1,1},tiny_sizes,numeric_random,okage::EffectStart::FirstFrame);
    tiny_miss.tick(numeric_random,true,nullptr,&numeric_projection);
    check(tiny_miss.elements[0].quad_uv==std::array<std::array<std::uint16_t,2>,4>{{{0,0},{32,0},{32,1088},{0,1088}}});
    const auto miss_uv=missed.elements[0].quad_uv;
    for(unsigned i=1;i<90;++i)missed.tick(numeric_random,true,nullptr,&numeric_projection);
    check(missed.elements[0].quad_uv==miss_uv&&numeric_rng==before_miss+2);
    missed.tick(numeric_random,true,nullptr,&numeric_projection);check(missed.flags&0x100);
    if(argc!=1) {
        check(argc==3);
        const auto read=[](const std::filesystem::path& path){
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read owned effect fixture");
            return Data(std::istreambuf_iterator<char>(in),{});
        };
        const auto definitions=okage::resolve_effect_references(okage::read_efd(read(argv[1])));
        const auto definition=std::find_if(definitions.begin(),definitions.end(),[](const auto& d){return d.id==38;});
        check(definition!=definitions.end()&&definition->duration==35&&definition->flags==0x400);
        okage::EffectTextureSizes owned_sizes;
        for(unsigned i=0;i<owned_sizes.size();++i) {
            const auto image=okage::decode_xim(read(std::filesystem::path(argv[2])/okage::effect_texture_names[i]));
            owned_sizes[i]={image.width,image.height};
        }
        unsigned draws=0;const auto rng=[&]{return ++draws;};
        okage::MapEffect owned(*definition,{0,-7.5f,0,1},{},{1,1,1,1},owned_sizes,rng,okage::EffectStart::FirstFrame);
        for(unsigned i=0;i<35;++i) {
            owned.tick(rng);
            const auto packets=okage::map_effect_draws(owned,identity,{},450,sine,nullptr,okage::EffectDrawPass::Deferred);
            check(packets.size()==3&&draws==3);
            check(packets[0].texture==1&&packets[1].texture==17&&packets[2].texture==6);
            for(const auto& packet:packets)check(packet.vertices.size()==6&&packet.additive&&!packet.depth_test);
        }
        owned.tick(rng);check((owned.flags&0x100)&&draws==3);
        for(unsigned id:{35u,36u}) {
            const auto def=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.id==id;});
            check(def!=definitions.end()&&def->duration==90&&def->flags==0x400);
            const auto before=draws;
            okage::MapEffect owned_numeric(*def,{0,0,10,1},{},{1,1,1,1},owned_sizes,rng,okage::EffectStart::FirstFrame);
            if(id==35)owned_numeric.set_numeric_parameter(-573);
            for(unsigned i=0;i<90;++i) {
                owned_numeric.tick(rng,true,nullptr,&numeric_projection);
                const auto packets=draw_number(owned_numeric,identity);
                check(draws==before+1&&packets.size()==1&&packets[0].texture==12);
                check(packets[0].vertices.size()==(id==35?18:6)&&!packets[0].depth_test&&!packets[0].additive);
            }
            owned_numeric.tick(rng,true,nullptr,&numeric_projection);
            check((owned_numeric.flags&0x100)&&draws==before+1);
        }
        std::cout<<"Owned effects35/36/38 lifetime and geometry smoke passed\n";
    }
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
