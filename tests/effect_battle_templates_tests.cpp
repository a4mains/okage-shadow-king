#include "okage/effect.hpp"
#include "okage/assets.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace {
using Data=std::vector<std::uint8_t>;
void check(bool v,const std::source_location where=std::source_location::current()){
    if(!v)throw std::runtime_error("Battle effect test failed at line "+std::to_string(where.line()));
}
template<class F>void rejects(F f,const char* message=nullptr,const std::source_location where=std::source_location::current()){
    try{f();}catch(const std::runtime_error& error){
        if(!message||std::string_view(error.what()).starts_with(message))return;
        throw std::runtime_error("Unexpected rejection at line "+std::to_string(where.line())+": "+error.what());
    }
    throw std::runtime_error("Invalid battle effect accepted at line "+std::to_string(where.line()));
}
//effect.cpp evaluates effect arithmetic with round-toward-zero.
struct TowardZero {
    int old=std::fegetround();
    TowardZero(){check(std::fesetround(FE_TOWARDZERO)==0);}
    ~TowardZero(){std::fesetround(old);}
};
//Former per-template expressions (33716be map_effect_draws), kept here only
//to prove the0x101960 material rule reproduces them for reviewed templates.
bool legacy_additive(unsigned id,std::uint32_t p){
    return (p&255)==1||p==8||((id==14||id==22||id==38||id==39)&&(p&255)==0);
}
bool legacy_depth(unsigned id){
    return id!=22&&id!=35&&id!=36&&id!=115&&id!=6&&id!=14&&id!=38&&id!=39;
}
bool legacy_subtractive(unsigned id,std::uint32_t p){
    return id==6||id==24||((id==14||id==22)&&(p&255)==0x20);
}
const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};

okage::EffectDefinition strike_definition() {
    //Structure of TES.EFD template10: header flags0x400, duration40.
    okage::EffectDefinition d;d.id=10;d.duration=40;d.flags=0x400;
    struct Row {std::uint32_t id,shape,flags,primitive,channels,program;};
    const std::array<Row,6> rows={Row{1,40,0x2,0,0x1b,0x54000f},{10,31,0x501,0,0x1d,0x182114},
        {11,31,0x541,0x100,0x1d,0x182114},{20,7,0,0,0x1b,0x180003},
        {33,30,0x401,0,0x19,0x180118},{34,30,0x401,0,0x1b,0x180218}};
    for(const auto& row:rows) {
        okage::EffectElement e;e.id=row.id;e.shape=row.shape;e.flags=row.flags;
        e.primitive_flags=row.primitive;e.channels=row.channels;e.program=row.program;
        e.change_time=1.1f;e.texture=row.shape==31?17:row.shape==30?5:-1;
        e.values[0]={.5f,.4f,.3f,.8f};
        e.values[2]={.1f,.1f,.1f,0};
        e.values[3]={10,10,10,.1f};
        e.values[4]={0,1,1,1};
        if(row.id==1){e.values[1]={0,.25f,0,.5f};e.values[4]={0,.5f,27,0};}
        if(row.id==20){e.values[1]={.25f,.25f,0,0};e.values[4]={.4f,1,0,0};}
        if(row.id==34)e.values[1]={0,0,.5f,33};
        d.elements.push_back(e);
    }
    return d;
}
okage::EffectDefinition projectile_definition() {
    //Structure of TES.EFD template11: ordinary pass, duration80, ten shape55.
    okage::EffectDefinition d;d.id=11;d.duration=80;d.flags=0;
    for(unsigned i=0;i<10;++i) {
        okage::EffectElement e;e.id=i+1;e.shape=55;e.flags=0;e.primitive_flags=1;
        e.channels=0x9;e.program=0x2000c0;e.change_time=1.1f;e.texture=-1;
        e.values[0]={.9f,.125f,.125f,.5f};
        e.values[1]={.3f,.6f,.9f,0}; //Ignored: channel bit1 is clear.
        e.values[3]={.5f*float(i+1),10-.5f*float(i+1),.5f*float(i+1),.1f};
        d.elements.push_back(e);
    }
    return d;
}
std::vector<okage::MapEffectDraw> draw(const okage::MapEffect& e,okage::EffectDrawPass pass) {
    return okage::map_effect_draws(e,identity,{},450,okage::make_sine_table(),nullptr,pass);
}
void run_strike(const okage::EffectDefinition& definition,const okage::EffectTextureSizes& sizes,bool synthetic) {
    unsigned rng=0;const auto random=[&]{return rng++;};
    okage::MapEffect job(definition,{1,2,3,1},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
    check(rng==0&&job.phase==1&&job.flags==0x400&&job.elements.size()==6);
    const std::array<std::uint32_t,6> initial_textures={0xffffffffu,17,17,0xffffffffu,6,6};
    for(unsigned i=0;i<6;++i)check(job.elements[i].texture==initial_textures[i]);
    const auto w=std::uint16_t(sizes[17][0]*32),h=std::uint16_t((sizes[17][1]+1)*32);
    check(job.elements[2].quad_uv[2]==std::array<std::uint16_t,2>{w,h});
    job.tick(random);
    check(rng==9&&job.phase==2);
    //Mode1 (primitive0x100): 0x103938(interval Z=1) then the0x1055d8 rescale
    //of the element's private (flag0x40) UV packet.
    check(job.elements[1].texture==17&&job.elements[2].texture==1);
    const auto rescale=[&](unsigned value,unsigned axis){
        return std::uint16_t((value*((sizes[1][axis]<<5)/sizes[17][axis]))>>5);};
    check(job.elements[2].quad_uv[2]==std::array<std::uint16_t,2>{rescale(w,0),rescale(h,1)});
    {
        TowardZero rounding;
        //Rotation8 adds element34's authored XYZ to element33's rendered angle.
        const auto& linked=job.elements[4].rendered[1];const auto& offset=definition.elements[5].values[1];
        check(job.elements[5].rendered[1]==okage::Float4({linked[0]+offset[0],linked[1]+offset[1],linked[2]+offset[2],offset[3]}));
        //Scale21 at the element's local time.
        volatile float one=1.0f,count=40.0f;volatile float inverse=one/count;volatile float time=one*inverse;
        const auto& interval=definition.elements[0].values[4];
        volatile float t=(time-interval[0])/(interval[1]-interval[0]);
        volatile float cycle=t*30.f;volatile float fraction=cycle-float(int(cycle));
        volatile float size=(1.f-fraction)*.15f+(1.f-t);
        const auto& scale=definition.elements[0].values[3];
        check(job.elements[0].rendered[2]==okage::Float4({size*scale[0],size*scale[1],size*scale[2],scale[3]}));
    }
    const auto packets=draw(job,okage::EffectDrawPass::Deferred);
    check(packets.size()==6);
    const std::array<std::size_t,6> vertex_counts={48,6,6,120,6,6};
    for(unsigned i=0;i<6;++i) {
        //All primitive low bytes are0: ALPHA0x48, TEST0x30000, ZBUF ZMSK1.
        check(packets[i].additive&&!packets[i].subtractive&&!packets[i].depth_test);
        check(packets[i].vertices.size()==vertex_counts[i]&&packets[i].texture==job.elements[i].texture);
    }
    check(packets[2].vertices[2].uv==okage::Float4({rescale(w,0)/32768.f,rescale(h,1)/32768.f,0,0}));
    rejects([&]{draw(job,okage::EffectDrawPass::Ordinary);},"Effect submitted to the wrong draw pass");
    const auto first=job;
    for(unsigned i=1;i<40;++i) {
        job.tick(random);
        check(draw(job,okage::EffectDrawPass::Deferred).size()==6);
    }
    //Element flags0x100/0x400 cleared the position/rotation programs.
    check(rng==9&&job.phase==41&&!(job.flags&0x100));
    for(unsigned i=1;i<6;++i)check(job.elements[i].rendered[1]==first.elements[i].rendered[1]);
    job.tick(random);check((job.flags&0x100)&&rng==9&&draw(job,okage::EffectDrawPass::Deferred).empty());
    if(synthetic)std::cout<<"Synthetic strike10 construction/update/draw passed\n";
}
//Template11: rotation3 consumes two draws per element on every update;
//scale8 writes only Y.
void run_projectile(const okage::EffectDefinition& definition,const okage::EffectTextureSizes& sizes) {
    const auto sine=okage::make_sine_table();
    unsigned rng=0;const auto random=[&]{return rng++;};
    okage::MapEffect job(definition,{0,1,2,1},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
    check(rng==0&&job.flags==0&&job.elements.size()==10);
    for(const auto& e:job.elements)check(e.texture==0xffffffffu&&e.rendered[1]==okage::Float4{});
    rejects([&]{draw(job,okage::EffectDrawPass::Ordinary);},"Missing native shape55 template geometry");
    unsigned factory=0;
    job.shape55_geometry=std::make_shared<const std::vector<okage::MapEffectVertex>>(
        okage::make_shadow_effect_shape(55,sine,[&]{return factory++;}));
    job.tick(random);check(rng==20&&job.phase==2);
    {
        TowardZero rounding;
        volatile float one=1.0f,count=80.0f;volatile float inverse=one/count;volatile float t=one*inverse;
        for(unsigned i=0;i<10;++i) {
            const auto& e=job.elements[i];const auto& authored=definition.elements[i].values;
            volatile float jitter_x=float(int((2*i)%10)-5),jitter_z=float(int((2*i+1)%10)-5);
            volatile float x=jitter_x*.001f;volatile float z=jitter_z*.001f;
            check(e.rendered[1]==okage::Float4({x+0.f,0,z+0.f,0}));
            volatile float ratio=(t-0.f)/(authored[3][3]-0.f);volatile float factor=ratio*ratio;
            check(e.rendered[2]==okage::Float4({authored[3][0],factor*authored[3][1],authored[3][2],authored[3][3]}));
            check(e.rendered[3]==authored[0]&&e.rendered[0]==okage::Float4{});
        }
    }
    const auto packets=draw(job,okage::EffectDrawPass::Ordinary);check(packets.size()==10);
    for(const auto& packet:packets) {
        //Primitive0x01: ALPHA0x48, TEST0x50000 (depth GEQUAL), ZBUF ZMSK1.
        check(packet.additive&&!packet.subtractive&&packet.depth_test);
        check(packet.texture==0xffffffffu&&packet.vertices.size()==job.shape55_geometry->size()&&packet.vertices.size()==240);
        for(std::size_t v=0;v<packet.vertices.size();++v) {
            const auto& a=packet.vertices[v];const auto& b=(*job.shape55_geometry)[v];
            check(a.position==b.position&&a.color==b.color&&a.uv==b.uv);
        }
    }
    rejects([&]{draw(job,okage::EffectDrawPass::Deferred);},"Effect submitted to the wrong draw pass");
    for(unsigned i=1;i<9;++i)job.tick(random);
    //Past local time W the Y factor is exactly one.
    for(unsigned i=0;i<10;++i)check(job.elements[i].rendered[2][1]==definition.elements[i].values[3][1]);
    for(unsigned i=9;i<80;++i)job.tick(random);
    check(rng==1600&&!(job.flags&0x100)&&draw(job,okage::EffectDrawPass::Ordinary).size()==10);
    job.tick(random);check((job.flags&0x100)&&rng==1600);
}
}
int main(int argc,char** argv){try{
    //0x101960 compiled words.
    const auto material=[](std::uint32_t p,std::uint64_t z,std::uint64_t a,std::uint64_t t){
        const auto m=okage::compile_effect_material(p);check(m.zbuf==z&&m.alpha==a&&m.test==t);};
    material(0,1ull<<32,0x48,0x30000);material(0x100,1ull<<32,0x48,0x30000);
    material(1,1ull<<32,0x48,0x50000);material(0x20,1ull<<32,0x42,0x30000);
    material(0x40,1ull<<32,0x44,0x3000d);material(0x41,1ull<<32,0x44,0x5000d);
    material(8,1ull<<32,0x48,0x53001);material(2,0,0x48,0x50000);material(4,0,0x48,0x0d);
    material(0x80,1ull<<32,0x01,0x30000);material(0x10,1ull<<32,0x28,0x30000);
    material(0x61,1ull<<32,0x42,0x50000);material(9,1ull<<32,0x48,0x50000);
    //Unrepresentable GS state is rejected, never approximated.
    rejects([]{okage::effect_draw_state(2);},"Unrecovered depth-writing effect material");
    rejects([]{okage::effect_draw_state(4);},"Unrecovered depth-writing effect material");
    rejects([]{okage::effect_draw_state(0x80);},"Unrecovered effect alpha blend material");
    rejects([]{okage::effect_draw_state(0x10);},"Unrecovered effect alpha blend material");
    rejects([]{okage::effect_draw_state(0x48);},"Unrecovered effect depth/alpha test material");
    rejects([]{okage::effect_draw_state(0x28);},"Unrecovered RGB-only effect blend");
    const std::vector<std::pair<unsigned,std::vector<std::uint32_t>>> reviewed={
        {6,{0x20}},{14,{0,0x20,0x300}},{16,{8}},{17,{0x41}},{19,{0x41}},{20,{0x41}},
        {22,{0x120,0x100}},{24,{0x21,0x121,0x321}},{25,{1,0x201,0x101}},{26,{0x41,0x201}},{33,{1}},
        {34,{0x201,0x41,0x141,0x341}},{35,{0x40}},{36,{0x140}},{38,{0,0x100}},{39,{0,0x100}},
        {90,{1}},{91,{1}},{105,{0x41,0x141,0x341}},{106,{0x41}},{115,{0x40}}};
    for(const auto& [id,flags]:reviewed)for(const auto p:flags) {
        const auto state=okage::effect_draw_state(p);
        check(state.additive==legacy_additive(id,p)&&state.depth_test==legacy_depth(id)&&
              state.subtractive==legacy_subtractive(id,p));
    }
    okage::EffectTextureSizes sizes;for(auto& size:sizes)size={64,64};
    sizes[17]={64,32};sizes[1]={32,16};sizes[6]={32,16};
    run_strike(strike_definition(),sizes,true);
    run_projectile(projectile_definition(),sizes);
    std::cout<<"Synthetic projectile11 construction/update/draw passed\n";
    //Negative cases: unsupported variants stay explicit failures.
    unsigned rng=0;const auto random=[&]{return rng++;};
    auto shared=strike_definition();shared.elements[2].flags&=~0x40u;
    {
        okage::MapEffect job(shared,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        rejects([&]{job.tick(random);},"Unrecovered shared effect UV rescale");
    }
    auto depth_write=strike_definition();depth_write.elements[3].primitive_flags=2;
    {
        okage::MapEffect job(depth_write,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        job.tick(random);
        rejects([&]{draw(job,okage::EffectDrawPass::Deferred);},
                "Unrecovered depth-writing effect material (template 10 element 20 shape 7)");
    }
    //Generic admission: orientation8 (0x1132a8), a random start phase, other
    //template ids and element counts are not template-gated.
    auto flipped=strike_definition();flipped.elements[0].flags=8;
    {
        okage::MapEffect job(flipped,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        job.tick(random);check(draw(job,okage::EffectDrawPass::Deferred).size()==6);
    }
    {okage::MapEffect job(strike_definition(),{},{},{1,1,1,1},sizes,random);}
    auto renamed=projectile_definition();renamed.id=12;renamed.elements.pop_back();
    {okage::MapEffect job(renamed,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);check(job.elements.size()==9);}
    auto unlinked=strike_definition();unlinked.elements.erase(unlinked.elements.begin()+4);
    {
        okage::MapEffect job(unlinked,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        rejects([&]{job.tick(random);},"Missing referenced effect rotation");
    }
    auto unknown_shape=projectile_definition();unknown_shape.elements[4].shape=61;
    rejects([&]{okage::MapEffect job(unknown_shape,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);},
            "Unrecovered map effect shape 61 (template 11 element 5)");
    auto ranged=projectile_definition();ranged.elements[1].flags=0x10000;
    rejects([&]{okage::MapEffect job(ranged,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);},
            "Unrecovered map effect element range flag 0x10000 (template 11 element 2)");
    auto timed=projectile_definition();timed.elements[0].program|=1u<<24;
    {
        okage::MapEffect job(timed,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        rejects([&]{job.tick(random);},"Unrecovered effect deformation geometry (template 11 element 1 shape 55)");
    }
    //Source rotation case13 (0x1090f4, 0x103c88 aim) is not implemented natively.
    auto rotation=projectile_definition();rotation.elements[0].program=(rotation.elements[0].program&~0xfc0u)|(13u<<6);
    {
        okage::MapEffect job(rotation,{},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        rejects([&]{job.tick(random);},"Unrecovered map effect rotation program 13 (template 11 element 1 shape 55)");
    }
    std::cout<<"Battle effect negative cases passed\n";
    //Optional owned data.
    if(argc>1) {
        const std::filesystem::path root=argv[1];
        if(!std::filesystem::exists(root/"TES.EFD")||!std::filesystem::exists(root/"ENV_TEX.XPF")) {
            std::cout<<"Owned TES.EFD/ENV_TEX not present; owned battle effect section skipped\n";
            return 0;
        }
        const auto read=[](const std::filesystem::path& path){
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read owned effect fixture");
            return Data(std::istreambuf_iterator<char>(in),{});
        };
        const auto definitions=okage::resolve_effect_references(okage::read_efd(read(root/"TES.EFD")));
        okage::EffectTextureSizes owned;
        for(unsigned i=0;i<owned.size();++i) {
            const auto image=okage::decode_xim(read(root/"ENV_TEX.XPF"/okage::effect_texture_names[i]));
            owned[i]={image.width,image.height};
        }
        const auto find=[&](unsigned id){
            const auto d=std::find_if(definitions.begin(),definitions.end(),[&](const auto& v){return v.id==id;});
            check(d!=definitions.end());return *d;};
        const auto strike=find(10),projectile=find(11);
        //The synthetic structure matches the owned headers and programs.
        const auto synthetic_strike=strike_definition(),synthetic_projectile=projectile_definition();
        check(strike.duration==40&&strike.flags==0x400&&strike.elements.size()==6);
        check(projectile.duration==80&&projectile.flags==0&&projectile.elements.size()==10);
        for(unsigned i=0;i<6;++i) {
            const auto& a=strike.elements[i];const auto& b=synthetic_strike.elements[i];
            check(a.id==b.id&&a.shape==b.shape&&a.flags==b.flags&&a.primitive_flags==b.primitive_flags&&
                  a.channels==b.channels&&a.program==b.program&&a.change_time>1);
        }
        for(const auto& a:projectile.elements)
            check(a.shape==55&&a.flags==0&&a.primitive_flags==1&&a.channels==9&&a.program==0x2000c0&&a.change_time>1);
        run_strike(strike,owned,false);
        run_projectile(projectile,owned);
        unsigned compared=0;
        for(const auto& d:definitions)for(const auto& [id,flags]:reviewed)if(d.id==id&&!(d.flags&0x800))
            for(const auto& e:d.elements) {
                const auto state=okage::effect_draw_state(e.primitive_flags);
                check(state.additive==legacy_additive(id,e.primitive_flags)&&state.depth_test==legacy_depth(id)&&
                      state.subtractive==legacy_subtractive(id,e.primitive_flags));
                ++compared;
            }
        std::cout<<"Owned strike10/projectile11 lifetime and draw state passed; legacy material elements compared="<<compared<<'\n';
    }
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
