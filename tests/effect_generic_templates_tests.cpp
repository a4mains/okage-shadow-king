#include "effect_template_harness.hpp"
#include "legacy/effect_7d876aa.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <map>
#include <set>
#include <source_location>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace {
using namespace effect_harness;
void check(bool v,const std::string& what="",const std::source_location where=std::source_location::current()){
    if(!v)throw std::runtime_error("Generic effect test failed at line "+std::to_string(where.line())+(what.empty()?"":": "+what));
}
bool same(float a,float b){return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);}
bool same(const okage::Float4& a,const okage::Float4& b){
    for(unsigned i=0;i<4;++i)if(!same(a[i],b[i]))return false;
    return true;
}
template<std::size_t N>bool same(const std::array<okage::Float4,N>& a,const std::array<okage::Float4,N>& b){
    for(std::size_t i=0;i<N;++i)if(!same(a[i],b[i]))return false;
    return true;
}
bool same(const okage::MapEffectVertex& a,const okage::MapEffectVertex& b){
    return same(a.position,b.position)&&same(a.color,b.color)&&same(a.uv,b.uv);
}
//Reviewed at 7d876aa (construction, update and draw whitelists).
const std::set<unsigned> reviewed={6,10,11,14,16,17,19,20,22,24,25,26,33,34,35,36,38,39,90,91,105,106,115};
const std::set<unsigned> legacy_random_phase={16,19,25,34,90,91,105,106};
bool legacy_quad(unsigned id,unsigned shape){
    return ((id==24||id==34||id==105||id==38||id==39||id==10)&&shape==31)||((id==22||id==36)&&shape==30)||shape==28;
}
void compare_draws(const std::vector<okage::MapEffectDraw>& a,const std::vector<okage::legacy::MapEffectDraw>& b,const std::string& at) {
    check(a.size()==b.size(),at+" draw count");
    for(std::size_t i=0;i<a.size();++i) {
        const auto& x=a[i];const auto& y=b[i];const auto in=at+" draw "+std::to_string(i);
        check(same(x.matrix,y.matrix)&&same(x.color,y.color)&&x.texture==y.texture&&x.uv==y.uv,in+" header");
        check(x.additive==y.additive&&x.depth_test==y.depth_test&&x.subtractive==y.subtractive&&x.clamp_mode==y.clamp_mode,in+" material");
        check(x.vertices.size()==y.vertices.size(),in+" vertex count");
        for(std::size_t v=0;v<x.vertices.size();++v)check(same(x.vertices[v],y.vertices[v]),in+" vertex "+std::to_string(v));
    }
}
void compare_state(const okage::MapEffect& n,const okage::legacy::MapEffect& l,const std::string& at) {
    check(n.id==l.id&&n.flags==l.flags&&n.duration==l.duration&&n.phase==l.phase&&same(n.inverse_duration,l.inverse_duration),at+" job header");
    check(same(n.near_clip,l.near_clip)&&n.clamp_mode==l.clamp_mode&&same(n.position,l.position)&&same(n.rotation,l.rotation)&&
          same(n.scale,l.scale)&&same(n.color,l.color)&&same(n.parameters,l.parameters),at+" job channels");
    check(n.elements.size()==l.elements.size(),at+" element count");
    for(std::size_t i=0;i<n.elements.size();++i) {
        const auto& x=n.elements[i];const auto& y=l.elements[i];const auto in=at+" element "+std::to_string(x.id);
        check(same(x.position,y.position)&&same(x.rotation,y.rotation)&&same(x.scale,y.scale)&&same(x.color,y.color)&&
              same(x.interval,y.interval),in+" channels");
        check(x.id==y.id&&x.shape==y.shape&&x.flags==y.flags&&x.primitive_flags==y.primitive_flags&&x.program==y.program&&
              x.next_program==y.next_program&&same(x.change_time,y.change_time),in+" programs");
        check(same(x.rendered,y.rendered),in+" rendered");
        check(x.texture==y.texture&&x.render_flags==y.render_flags&&x.uv==y.uv,in+" texture");
        check(x.ring_y==y.ring_y,in+" ring");
        if(y.cylinder_t.empty())check(x.cylinder_t.empty()||(x.shape==24&&x.cylinder_t==std::vector<std::uint16_t>(17,0)),in+" cylinder");
        else check(x.cylinder_t==y.cylinder_t,in+" cylinder");
        check(x.cylinder_source_id==y.cylinder_source_id&&x.strip_source_id==y.strip_source_id,in+" links");
        check(x.quad_positions==y.quad_positions,in+" quad positions");
        if(x.shape==28)check(n.flame_positions(x)==l.flame_positions(y),in+" flame positions");
        if(legacy_quad(n.id,x.shape))check(x.quad_uv==y.quad_uv,in+" quad UVs");
        check(x.strip_positions==y.strip_positions&&x.strip_tags==y.strip_tags&&x.strip_uv==y.strip_uv,in+" strip");
        check(x.number_uv==y.number_uv,in+" numeric UVs");
        check(bool(x.water)==bool(y.water),in+" water");
        if(x.water) {
            const auto& a=*x.water;const auto& b=*y.water;
            check(a.heights==b.heights&&a.coordinates==b.coordinates&&a.packed_y==b.packed_y&&a.packed_uv==b.packed_uv&&
                  a.selected_packet==b.selected_packet&&a.active==b.active&&a.rate==b.rate&&a.rate_counter==b.rate_counter&&
                  a.period==b.period&&a.period_counter==b.period_counter&&a.initialized==b.initialized,in+" water state");
            for(unsigned p=0;p<2;++p)check(a.packets[p].y==b.packets[p].y&&a.packets[p].uv==b.packets[p].uv,in+" water packet");
        }
    }
}
unsigned compare_template(const okage::EffectDefinition& d,const okage::EffectTextureSizes& sizes,
                          const std::shared_ptr<const okage::EffectTemplatePreparation>& prepared,
                          okage::EffectStart start,unsigned updates) {
    const auto sine=okage::make_sine_table();const auto in=make_inputs();
    Random rn,rl;
    const okage::Float4 place{3,-2,7,1},turn{.1f,.2f,0,0},size{1.5f,1,2,1};
    okage::MapEffect n(d,place,turn,size,sizes,std::ref(rn),start);
    okage::legacy::MapEffect l(d,place,turn,size,sizes,std::ref(rl),start);
    const auto at=[&](unsigned tick){return "template "+std::to_string(d.id)+(start==okage::EffectStart::RandomPhase?" random":" first")+
                                         " update "+std::to_string(tick);};
    if(std::any_of(d.elements.begin(),d.elements.end(),[](const auto& e){return e.shape==55;})) {
        const std::shared_ptr<const std::vector<okage::MapEffectVertex>> geometry(prepared->geometry[55],&prepared->geometry[55]->vertices);
        n.shape55_geometry=geometry;l.shape55_geometry=geometry;
    }
    if(n.flame_geometry) {
        n.bind_flame_geometry(std::make_shared<okage::EffectFlameGeometry>(*prepared->flame));
        l.bind_flame_geometry(std::make_shared<okage::EffectFlameGeometry>(*prepared->flame));
    }
    check(bool(n.flame_geometry)==bool(l.flame_geometry),at(0)+" flame factory");
    if(d.id==35){n.set_numeric_parameter(-573);l.set_numeric_parameter(-573);}
    compare_state(n,l,at(0));
    const auto pass=(n.flags&0x400)?okage::EffectDrawPass::Deferred:okage::EffectDrawPass::Ordinary;
    for(unsigned tick=1;tick<=updates;++tick) {
        if(d.id==106&&tick==4){n.rebind_water_texture(0,2,64,32);l.rebind_water_texture(0,2,64,32);}
        n.tick(std::ref(rn),true,&in.eye,&in.projection);
        l.tick(std::ref(rl),true,&in.eye,&in.projection);
        check(rn.calls==rl.calls&&rn.state==rl.state,at(tick)+" RNG");
        compare_state(n,l,at(tick));
        compare_draws(okage::map_effect_draws(n,in.view,in.camera,450,sine,&in.tint,pass),
                      okage::legacy::map_effect_draws(l,in.view,in.camera,450,sine,&in.tint,pass),at(tick));
        //Immutable submission snapshots must agree as well.
        compare_state(n.snapshot(),l.snapshot(),at(tick)+" snapshot");
        if(n.flags&0x100)break;
    }
    return rn.calls;
}
//0x10ba38 ordinary pass in file order, with 0x10b318 element-ID ranges.
std::map<unsigned,std::pair<std::uint32_t,std::int32_t>> first_requesters(const std::vector<okage::EffectDefinition>& raw) {
    std::map<unsigned,std::pair<std::uint32_t,std::int32_t>> first;
    for(const auto& d:raw) {
        if(d.flags&0x800)continue;
        for(const auto& e:d.elements) {
            if(e.flags&0x10000) {
                for(const auto& source:d.elements)
                    if(source.id>=e.shape&&source.id<=e.primitive_flags&&!(source.flags&0x10000))
                        first.try_emplace(source.shape,d.id,source.texture);
            } else first.try_emplace(e.shape,d.id,e.texture);
        }
    }
    return first;
}
}
int main(int argc,char** argv){try{
    //The shape table covers exactly the recovered factories.
    for(unsigned shape:{1u,2u,4u,8u,18u,23u,27u,34u,36u,44u,46u,53u,57u,60u,61u,63u,64u,66u})
        try{okage::effect_shape_texture(shape);check(false,"unrecovered shape "+std::to_string(shape)+" accepted");}
        catch(const std::runtime_error& error){check(std::string(error.what())=="Unrecovered map effect shape "+std::to_string(shape));}
    check(okage::effect_shape_texture(30)==6&&okage::effect_shape_texture(31)==17&&okage::effect_shape_texture(55)==0xffffffffu);
    if(argc<2) {std::cout<<"No owned data directory; legacy comparison skipped\n";return 0;}
    const auto owned=load_owned(argv[1]);
    if(!owned){std::cout<<"Owned TES.EFD/ENV_TEX not present; legacy comparison skipped\n";return 0;}
    //Parameter-textured factories bind the first requester's EFD texture.
    const auto first=first_requesters(owned->raw);
    for(unsigned shape:{24u,28u,29u,33u,35u,56u,59u,62u,65u}) {
        const auto found=first.find(shape);
        check(found!=first.end()&&std::int64_t(okage::effect_shape_texture(shape))==found->second.second,
              "shape "+std::to_string(shape)+" first-requester texture");
    }
    Random prep_random;
    const auto prepared=std::make_shared<const okage::EffectTemplatePreparation>(owned->raw,owned->sizes,okage::make_sine_table(),std::ref(prep_random));
    unsigned compared=0,draws=0;
    for(const auto& d:owned->resolved) {
        if(!reviewed.contains(d.runtime_id.value_or(d.id)))continue;
        //Bounded sample of looping jobs; finite jobs run past removal.
        const unsigned updates=update_count(d,400);
        draws+=compare_template(d,owned->sizes,prepared,okage::EffectStart::FirstFrame,updates);++compared;
        if(legacy_random_phase.contains(d.runtime_id.value_or(d.id))) {
            draws+=compare_template(d,owned->sizes,prepared,okage::EffectStart::RandomPhase,updates);++compared;
        }
    }
    check(compared>=28,"reviewed template coverage");
    std::cout<<"Legacy 7d876aa comparison passed: "<<compared<<" template/start runs, "<<draws<<" RNG draws, byte-identical state and draws\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
