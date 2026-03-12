#include "effect_template_harness.hpp"
#include "legacy/effect_7d876aa.hpp"
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace {
using namespace effect_harness;
struct Result {bool supported=false;std::string stage,message;unsigned updates=0;};
template<class Effect,class Draw,class Bind>
Result run(const okage::EffectDefinition& d,const okage::EffectTextureSizes& sizes,Draw draw,Bind bind) {
    const auto in=make_inputs();const auto sine=okage::make_sine_table();
    Random random;Result result;
    std::optional<Effect> job;
    try{job.emplace(d,okage::Float4{3,-2,7,1},okage::Float4{},okage::Float4{1,1,1,1},sizes,std::ref(random),okage::EffectStart::FirstFrame);
        bind(*job);}
    catch(const std::exception& e){result.stage="construct";result.message=e.what();return result;}
    const auto pass=(job->flags&0x400)?okage::EffectDrawPass::Deferred:okage::EffectDrawPass::Ordinary;
    const unsigned updates=update_count(d,400);
    for(unsigned tick=1;tick<=updates;++tick) {
        try{job->tick(std::ref(random),true,&in.eye,&in.projection);}
        catch(const std::exception& e){result.stage="update "+std::to_string(tick);result.message=e.what();return result;}
        try{draw(*job,in.view,in.camera,450.f,sine,&in.tint,pass);}
        catch(const std::exception& e){result.stage="draw "+std::to_string(tick);result.message=e.what();return result;}
        result.updates=tick;
        if(job->flags&0x100)break;
    }
    result.supported=true;return result;
}
std::string feature(const std::string& message) {
    const auto at=message.find(" (template ");
    return at==std::string::npos?message:message.substr(0,at);
}
std::string json(const std::string& s) {
    std::string out="\"";
    for(char c:s){if(c=='"'||c=='\\')out+='\\';out+=c;}
    return out+"\"";
}
std::uint32_t word(const Data& b,std::size_t p){return std::uint32_t(b.at(p))|(std::uint32_t(b.at(p+1))<<8)|(std::uint32_t(b.at(p+2))<<16)|(std::uint32_t(b.at(p+3))<<24);}
std::int16_t half(const Data& b,std::size_t p){return std::int16_t(std::uint16_t(b.at(p)|(b.at(p+1)<<8)));}
std::map<unsigned,unsigned> battle_schedule(const Data& b) {
    if(b.size()<136||std::string(reinterpret_cast<const char*>(b.data()),7)!="OKBACT1")throw std::runtime_error("Not an OKBACT1 artifact");
    const auto pairs=word(b,20),generic=word(b,24),secondary=word(b,28),record=word(b,32);
    if(record!=0x88)throw std::runtime_error("Unexpected battle descriptor size");
    std::size_t p=136+1024;
    std::map<unsigned,unsigned> uses;
    const auto scan=[&](std::size_t at) {
        for(unsigned i=0;i<4;++i) {
            const auto entry=at+0x38+i*20;
            const auto id=half(b,entry),time=half(b,entry+2);const auto type=word(b,entry+4);
            if(time<0)break;
            if(type<=5&&id>=0)++uses[unsigned(id)];
        }
    };
    for(unsigned i=0;i<pairs*2+generic+secondary;++i)scan(p+i*0x88);
    if(p+(pairs*2+generic+secondary)*0x88!=b.size())throw std::runtime_error("Battle descriptor extent");
    return uses;
}
}
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Usage: effect-template-coverage CMNDATA_2D_DIR BATTLE_ACTIONS_BIN OUT.json");
    const auto owned=load_owned(argv[1]);
    if(!owned)throw std::runtime_error("Owned TES.EFD/ENV_TEX not found");
    const auto schedule=battle_schedule(read_file(argv[2]));
    const std::set<unsigned> code_paths={11,35,36,38,39,40};
    Random prep_random;
    const auto prepared=std::make_shared<const okage::EffectTemplatePreparation>(owned->raw,owned->sizes,okage::make_sine_table(),std::ref(prep_random));
    const auto bind_new=[&](okage::MapEffect& e){
        e.bind_preparation(prepared);
        if(e.flame_geometry)e.bind_flame_geometry(std::make_shared<okage::EffectFlameGeometry>(*prepared->flame));
        if(e.id==35)e.set_numeric_parameter(-573);
    };
    const auto bind_old=[&](okage::legacy::MapEffect& e){
        for(const auto& x:e.elements)if(x.shape==55)
            e.shape55_geometry=std::shared_ptr<const std::vector<okage::MapEffectVertex>>(prepared->geometry[55],&prepared->geometry[55]->vertices);
        if(e.flame_geometry)e.bind_flame_geometry(std::make_shared<okage::EffectFlameGeometry>(*prepared->flame));
        if(e.id==35)e.set_numeric_parameter(-573);
    };
    const auto draw_new=[](const okage::MapEffect& e,const okage::Matrix4& v,const okage::Float4& c,float f,const okage::SineTable& s,const okage::Float4* t,okage::EffectDrawPass p){
        return okage::map_effect_draws(e,v,c,f,s,t,p);};
    const auto draw_old=[](const okage::legacy::MapEffect& e,const okage::Matrix4& v,const okage::Float4& c,float f,const okage::SineTable& s,const okage::Float4* t,okage::EffectDrawPass p){
        return okage::legacy::map_effect_draws(e,v,c,f,s,t,p);};
    std::ostringstream out;
    out<<"{\n \"schema\": \"okage-effect-template-coverage-v1\",\n";
    out<<" \"inputs\": {\"efd_dir\": "<<json(argv[1])<<", \"battle_actions\": "<<json(argv[2])<<"},\n";
    out<<" \"harness\": \"FirstFrame start; xorshift32 RNG; full lifetime (<=400 updates, looping jobs sampled); every update drawn in the job's pass with camera, eye, projection and light tint supplied\",\n";
    out<<" \"templates\": [\n";
    unsigned before=0,after=0,battle_total=0,battle_before=0,battle_after=0;
    std::map<std::string,std::vector<unsigned>> blockers,battle_blockers;
    bool comma=false;
    for(const auto& d:owned->resolved) {
        const auto runtime=d.runtime_id.value_or(d.id);
        const auto uses=schedule.contains(d.id)?schedule.at(d.id):0u;
        const bool battle=uses>0||code_paths.contains(d.id);
        const auto old=run<okage::legacy::MapEffect>(d,owned->sizes,draw_old,bind_old);
        const auto now=run<okage::MapEffect>(d,owned->sizes,draw_new,bind_new);
        before+=old.supported;after+=now.supported;
        if(battle){++battle_total;battle_before+=old.supported;battle_after+=now.supported;}
        if(!now.supported){blockers[feature(now.message)].push_back(d.id);if(battle)battle_blockers[feature(now.message)].push_back(d.id);}
        std::set<unsigned> shapes;for(const auto& e:d.elements)shapes.insert(e.shape);
        out<<(comma?",\n":"")<<"  {\"id\": "<<d.id<<", \"runtime_id\": "<<runtime<<", \"reference\": "<<(d.id!=runtime?"true":"false")
           <<", \"flags\": "<<d.flags<<", \"duration\": "<<d.duration<<", \"elements\": "<<d.elements.size()<<", \"shapes\": [";
        bool c2=false;for(auto s:shapes){out<<(c2?", ":"")<<s;c2=true;}
        out<<"], \"battle_schedule_uses\": "<<uses<<", \"battle_code_path\": "<<(code_paths.contains(d.id)?"true":"false")
           <<",\n   \"before\": {\"supported\": "<<(old.supported?"true":"false")<<", \"stage\": "<<json(old.stage)<<", \"message\": "<<json(old.message)<<"}"
           <<",\n   \"after\": {\"supported\": "<<(now.supported?"true":"false")<<", \"stage\": "<<json(now.stage)<<", \"message\": "<<json(now.message)
           <<", \"updates\": "<<now.updates<<"}}";
        comma=true;
    }
    out<<"\n ],\n \"summary\": {\"templates\": "<<owned->resolved.size()<<", \"supported_before\": "<<before<<", \"supported_after\": "<<after
       <<", \"battle_relevant\": "<<battle_total<<", \"battle_supported_before\": "<<battle_before<<", \"battle_supported_after\": "<<battle_after<<"},\n";
    const auto dump=[&](const char* name,const std::map<std::string,std::vector<unsigned>>& map,bool last){
        out<<" \""<<name<<"\": {";bool c=false;
        for(const auto& [k,v]:map){out<<(c?",":"")<<"\n  "<<json(k)<<": [";bool c3=false;for(auto id:v){out<<(c3?", ":"")<<id;c3=true;}out<<"]";c=true;}
        out<<"\n }"<<(last?"\n":",\n");
    };
    dump("blockers_after",blockers,false);
    dump("battle_blockers_after",battle_blockers,true);
    out<<"}\n";
    std::ofstream file(argv[3]);file<<out.str();
    if(!file)throw std::runtime_error("Cannot write coverage report");
    std::cout<<"templates="<<owned->resolved.size()<<" supported before="<<before<<" after="<<after
             <<" battle-relevant="<<battle_total<<" before="<<battle_before<<" after="<<battle_after<<'\n';
    for(const auto& [k,v]:battle_blockers){std::cout<<"  "<<k<<":";for(auto id:v)std::cout<<' '<<id;std::cout<<'\n';}
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
