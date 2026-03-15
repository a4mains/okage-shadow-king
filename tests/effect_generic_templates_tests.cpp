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
template<class F>void rejects_message(F f,const char* prefix,const std::source_location where=std::source_location::current()){
    try{f();}catch(const std::runtime_error& error){
        if(std::string_view(error.what()).starts_with(prefix))return;
        throw std::runtime_error("Unexpected rejection at line "+std::to_string(where.line())+": "+error.what());
    }
    throw std::runtime_error("Invalid effect accepted at line "+std::to_string(where.line()));
}
struct TowardZero {
    int old=std::fegetround();
    TowardZero(){check(std::fesetround(FE_TOWARDZERO)==0);}
    ~TowardZero(){std::fesetround(old);}
};
okage::EffectElement element(unsigned id,unsigned shape,std::uint32_t program,const okage::Float4& position={},
                             const okage::Float4& scale={1,1,1,1},const okage::Float4& color={1,1,1,1},
                             const okage::Float4& rotation={},const okage::Float4& interval={0,1,1,0}) {
    okage::EffectElement e;e.id=id;e.shape=shape;e.program=program;e.channels=31;e.change_time=1.1f;
    e.values={color,rotation,position,scale,interval};
    return e;
}
okage::EffectDefinition single(std::vector<okage::EffectElement> elements,unsigned duration=4) {
    okage::EffectDefinition d;d.id=120;d.duration=duration;d.elements=std::move(elements);return d;
}
unsigned angle_index(float turns){
    volatile float scaled=turns*32768.0f;
    return (std::uint32_t(std::int32_t(scaled))>>1)&16383;
}
void program_checks() {
    const auto sine=okage::make_sine_table();
    okage::EffectTextureSizes sizes;for(auto& size:sizes)size={64,32};sizes[17]={64,64};sizes[10]={128,64};sizes[1]={32,32};
    unsigned calls=0;std::vector<std::uint32_t> words{7,13,29,41,55,68};
    const auto random=[&]{return words[calls++%words.size()];};
    const auto run=[&](const okage::EffectDefinition& d,unsigned updates){
        okage::MapEffect job(d,{0,0,0,1},{},{1,1,1,1},sizes,random,okage::EffectStart::FirstFrame);
        for(unsigned i=0;i<updates;++i)job.tick(random);
        return job;
    };
    const auto pos=[](unsigned v){return v<<12;};const auto rot=[](unsigned v){return v<<6;};
    const auto scl=[](unsigned v){return v<<18;};const auto tim=[](unsigned v){return v<<24;};
    {
        //Position21 (0x1074f8): X = authored X + W*t.
        const auto job=run(single({element(1,31,pos(21),{2,0,0,8})}),1);
        TowardZero z;volatile float x=2.f,w=8.f,t=.25f;
        check(job.elements[0].rendered[0][0]==x+w*t,"position21");
    }
    {
        //Position7 (0x106f30): rendered/authored XZ from element ID-1's rendered point.
        const auto job=run(single({element(1,31,0,{5,6,7,0}),element(2,31,pos(7),{1,2,3,0})}),1);
        check(job.elements[1].rendered[0]==okage::Float4({5,2,7,0})&&job.elements[1].position==okage::Float4({5,2,7,0}),"position7");
    }
    {
        //Position40 (0x107f68): (1-t)*XYZ spiral (cos/sin of angle(t)) + source XYZ of element int(W).
        const auto job=run(single({element(1,31,0,{1,2,3,0}),element(2,31,pos(40),{4,5,6,1})}),1);
        TowardZero z;volatile float keep=1.f-.25f;const auto a=angle_index(.25f);
        volatile float x=(keep*4.f)*sine[(a+4096)&16383];volatile float zz=(keep*6.f)*sine[a];
        volatile float y=keep*5.f;
        check(job.elements[1].rendered[0][0]==x+1.f&&job.elements[1].rendered[0][1]==y+2.f&&
              job.elements[1].rendered[0][2]==zz+3.f,"position40");
    }
    {
        //Position29 (0x1076a8): 0x10d600 point int(X)%12/12, scaled by W*(t+Z).
        const auto job=run(single({element(1,31,pos(29),{5,9,.5f,2})}),1);
        TowardZero z;volatile float n=5.f,d=12.f;const auto a=angle_index(n/d);volatile float along=.25f+.5f;
        volatile float c=sine[(a+4096)&16383]*2.f,s=sine[a]*2.f;
        check(job.elements[0].rendered[0][0]==c*along&&job.elements[0].rendered[0][2]==s*along&&
              job.elements[0].rendered[0][1]==9.f,"position29");
    }
    {
        //Position45 (0x1080fc): raw t ring + two draws (*.3, *.1); Y=0.
        calls=0;const auto job=run(single({element(1,31,pos(45),{1,2,3,.25f})}),1);
        TowardZero z;const auto a=angle_index(.25f);volatile float r=.25f*2.f;
        volatile float j0=float(int(7%40)-20),j1=float(int(13%40)-20);
        volatile float x=(r*sine[(a+4096)&16383]+j0*.3f)+1.f;volatile float zz=(r*sine[a]+j1*.1f)+3.f;
        check(calls==2&&job.elements[0].rendered[0]==okage::Float4({x,0,zz,.25f}),"position45");
    }
    {
        //Position3 (0x106d48): three draws scale authored XYZ.
        calls=0;const auto job=run(single({element(1,31,pos(3),{1,2,3,0})}),1);
        check(calls==3&&job.elements[0].rendered[0]==okage::Float4({-13.f,-14.f,27.f,0}),"position3");
    }
    {
        //Rotation6 (0x108dfc): three draws (rand%360+360)*.00278f.
        calls=0;const auto job=run(single({element(1,31,rot(6))}),1);
        TowardZero z;volatile float k=.00278f;
        check(calls==3&&job.elements[0].rendered[1]==okage::Float4({367.f*k,373.f*k,389.f*k,0}),"rotation6");
    }
    {
        //Rotation9 (0x108fcc): f=frac(t*W) added to authored X and Y.
        const auto job=run(single({element(1,31,rot(9),{},{1,1,1,1},{1,1,1,1},{.5f,.25f,.75f,3})}),1);
        TowardZero z;volatile float f=.25f*3.f;volatile float frac=f-float(int(f));
        check(job.elements[0].rendered[1]==okage::Float4({frac+.5f,frac+.25f,.75f,3}),"rotation9");
    }
    {
        //Scale5 (0x10934c), scale22 (0x109830), scale28 (0x109ac8), scale19 (0x1096ec).
        TowardZero z;
        auto job=run(single({element(1,31,scl(5),{},{2,3,4,1})}),1);
        volatile float q=-.1f/.25f;volatile float f=q+1.f;
        check(job.elements[0].rendered[2]==okage::Float4({f*2.f,f*3.f,f*4.f,1}),"scale5");
        job=run(single({element(1,31,scl(22),{},{2,3,4,3})}),1);
        volatile float cycle=.25f*3.f;volatile float g=(1.f-(cycle-float(int(cycle))))*.15f+1.f;
        check(job.elements[0].rendered[2]==okage::Float4({g*2.f,g*3.f,g*4.f,3}),"scale22");
        job=run(single({element(1,31,scl(28),{},{2,3,4,3})}),1);
        check(job.elements[0].rendered[2]==okage::Float4({.75f*2.f,.75f*3.f,.75f*4.f,3}),"scale28");
        job=run(single({element(1,31,scl(19),{},{2,3,4,.5f})}),1);
        check(job.elements[0].rendered[2]==okage::Float4({.5f*2.f,3.f,.5f*4.f,.5f}),"scale19");
    }
    {
        //Colours19 (head .1, tail .6), 4 (knee .2), 22, 12 (0x109c44 tables 0x221e60..0x221ec0).
        TowardZero z;
        auto job=run(single({element(1,31,19,{},{1,1,1,1},{1,1,1,.8f})}),3);
        volatile float t=.75f;volatile float a19=(1.f-(t-.6f)/(1.f-.6f))*.8f;
        check(job.elements[0].rendered[3][3]==a19,"colour19");
        job=run(single({element(1,31,4,{},{1,1,1,1},{1,1,1,.8f})}),1);
        volatile float u=.25f;volatile float a4=(1.f-(u-.2f)/(1.f-.2f))*.8f;
        check(job.elements[0].rendered[3][3]==a4,"colour4");
        job=run(single({element(1,31,22,{},{1,1,1,1},{1,1,1,.8f})}),1);
        check(job.elements[0].rendered[3][3]==1.f,"colour22");
        job=run(single({element(1,31,12,{},{1,1,1,1},{1,1,1,.8f})}),1);
        volatile float c5=.25f*5.f;volatile float f12=c5-float(int(c5));
        check(job.elements[0].rendered[3][3]==f12/.4f,"colour12");
    }
    {
        //Time9 (0x1061fc): t'=frac(W*t) feeds scale3 (t*XYZ).
        const auto job=run(single({element(1,31,tim(9)|scl(3),{},{2,3,4,1},{1,1,1,1},{},{0,1,1,3})}),1);
        check(job.elements[0].rendered[2]==okage::Float4({.75f*2.f,.75f*3.f,.75f*4.f,1}),"time9");
    }
    {
        //Time23 (0x1066e8 ->0x104ba8): vertical scroll of the private quad UVs.
        auto e=element(1,31,tim(23),{},{1,1,1,1},{1,1,1,1},{},{0,1,1,8});e.flags=0x40;
        const auto job=run(single({e}),1);
        TowardZero z;
        volatile float extent=float(64)-8.f;volatile float along=(1.f-.25f)*extent;const int cell=int(along);
        const int dy=(64*32)/8;
        const auto remap=[&](int original){return std::uint16_t(((original*4096)/dy>>7)+cell*32);};
        const auto w=std::uint16_t(64*32),h=std::uint16_t(65*32);
        check(job.elements[0].quad_uv[2]==std::array<std::uint16_t,2>{w,remap(std::int16_t(h))}&&
              job.elements[0].quad_uv[0]==std::array<std::uint16_t,2>{0,remap(0)},"time23");
        auto shared=e;shared.flags=0;
        rejects_message([&]{run(single({shared}),1);},"Unrecovered atlas scroll geometry");
    }
    {
        //Shape60 (0x110c88): twelve white triangles P_i,P_i+1,P_i+2 (i even) on the unit ring.
        const auto job=run(single({element(1,60,0)}),1);
        const auto draws=okage::map_effect_draws(job,{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},{},450,sine);
        check(draws.size()==1&&draws[0].vertices.size()==36&&draws[0].texture==0xffffffffu,"shape60 count");
        const auto a=angle_index(1.f/24);
        check(draws[0].vertices[1].position==okage::Float4({std::round(sine[(a+4096)&16383]*64)/64,0,std::round(sine[a]*64)/64,1})&&
              draws[0].vertices[0].position==okage::Float4({1,0,0,1})&&draws[0].vertices[2].color==okage::Float4({255,255,255,255}),"shape60 ring");
    }
    {
        //Shape54 (0x1111d0): 11x6 quads, alpha 255*(1-row/11), textured (bound10).
        const auto job=run(single({element(1,54,0)}),1);
        const auto draws=okage::map_effect_draws(job,{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},{},450,sine);
        check(draws.size()==1&&draws[0].vertices.size()==396&&draws[0].texture==10,"shape54 count");
        TowardZero z;volatile float last=1.f-float(1)/float(11);
        check(draws[0].vertices[0].color[3]==float(unsigned(255.f*last))&&draws[0].vertices[1].color[3]==255.f,"shape54 alpha");
        //B of the first quad: column0,row1: h=1/6 (point rotated about X), UV (0,1/11).
        volatile float v=float(1)/float(11);
        check(draws[0].vertices[0].uv==okage::Float4({0,float(std::uint16_t(unsigned(v*float(65)*32)))/32768,0,0}),"shape54 uv");
    }
    {
        //Shapes36/61/57: diamond quad, unit cube, ring of twelve cubes.
        const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
        auto job=run(single({element(1,36,0),element(2,61,0),element(3,57,0)}),1);
        const auto draws=okage::map_effect_draws(job,identity,{},450,sine);
        check(draws.size()==3&&draws[0].vertices.size()==6&&draws[1].vertices.size()==36&&draws[2].vertices.size()==432,"cube counts");
        check(draws[0].vertices[0].position==okage::Float4({1,0,0,1})&&draws[0].vertices[1].position==okage::Float4({0,-1,0,1}),"diamond");
        check(draws[1].vertices[0].position==okage::Float4({.5f,-.5f,-.5f,1})&&draws[1].vertices[1].position==okage::Float4({-.5f,-.5f,-.5f,1}),"cube");
        check(draws[1].texture==10&&draws[1].vertices[0].uv==okage::Float4({0,0,0,0})&&
              draws[1].vertices[2].uv[0]==float(128*32)/32768,"cube uv");
    }
    {
        //Shape26 (0x10fe28(30)) and time20 mode3 (0x105790): T=int(t*1200) on the lower ring.
        auto e=element(1,26,tim(20),{},{1,1,1,1},{1,1,1,1},{},{0,1,1,3});e.flags=0x40;
        const auto job=run(single({e}),1);
        check(job.elements[0].texture==1&&job.elements[0].packet_uv.size()==62&&
              job.elements[0].packet_uv[5][1]==std::uint16_t(int(.25f*1200.f))&&job.elements[0].packet_uv[36][1]==33*32,"shape26 time20");
        const auto draws=okage::map_effect_draws(job,{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},{},450,sine);
        check(draws.size()==1&&draws[0].vertices.size()==180,"shape26 draw");
    }
    std::cout<<"Recovered program/factory transcription checks passed\n";
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
    program_checks();
    //The shape table covers exactly the recovered factories.
    for(unsigned shape:{1u,2u,4u,8u,18u,27u,34u,37u,44u,46u,53u,63u,64u,66u})
        try{okage::effect_shape_texture(shape);check(false,"unrecovered shape "+std::to_string(shape)+" accepted");}
        catch(const std::runtime_error& error){check(std::string(error.what())=="Unrecovered map effect shape "+std::to_string(shape));}
    check(okage::effect_shape_texture(30)==6&&okage::effect_shape_texture(31)==17&&okage::effect_shape_texture(55)==0xffffffffu);
    if(argc<2) {std::cout<<"No owned data directory; legacy comparison skipped\n";return 0;}
    const auto owned=load_owned(argv[1]);
    if(!owned){std::cout<<"Owned TES.EFD/ENV_TEX not present; legacy comparison skipped\n";return 0;}
    //Parameter-textured factories bind the first requester's EFD texture.
    const auto first=first_requesters(owned->raw);
    for(unsigned shape:{24u,26u,28u,29u,33u,35u,36u,54u,56u,57u,59u,61u,62u,65u}) {
        const auto found=first.find(shape);
        check(found!=first.end()&&std::bit_cast<std::int32_t>(okage::effect_shape_texture(shape))==found->second.second,
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
