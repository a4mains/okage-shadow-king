#include "okage/ui_draw.hpp"
#include "okage/ruw.hpp"
#include "okage/enemy.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>

namespace okage {
struct UiWindowDrawTestAccess {
    static void put(UiWindow& w,unsigned at,std::uint32_t v){w.put(at,v);}
    static void glyph(UiWindow& w,unsigned group,unsigned side){
        UiNativePacket p;p.operations.push_back({UiPacketOperationKind::Register,0x1aedd8,{0x47,0x30003},{}});
        w.glyph_packets_[{group,side}]=std::move(p);w.put(0x3c08+group*4,w.word(0x3c08+group*4)&~4u);
    }
    static void sprite(UiWindow& w,unsigned element){
        auto& p=w.sprite_packets_[{0,0}];p.elements[element].emplace();
        w.put(0x3c08,w.word(0x3c08)&~8u);
        for(unsigned j=0;j<2;++j)w.sprite_[0].put_word(element*16+j*4,__builtin_bit_cast(std::uint32_t, (float(10+element*3+j))));
        for(unsigned j=0;j<4;++j)w.put(0x3e40+element*16+j*4,__builtin_bit_cast(std::uint32_t, (float(j+1))));
    }
};
}
namespace {
using namespace okage;
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
void put(UiWindow& w,unsigned at,std::uint32_t v){UiWindowDrawTestAccess::put(w,at,v);}
void fput(UiWindow& w,unsigned at,float v){put(w,at,__builtin_bit_cast(std::uint32_t, (v)));}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected UI draw guard");}
std::shared_ptr<UiWindow> window(unsigned slot=0){
    auto w=std::make_shared<UiWindow>(2,2,slot,std::make_shared<UiWindowStream>(UiWindowStream{1,{}}));
    w->set_opacity_tween(0,64,0,128);put(*w,0x25c,0x10);put(*w,0x268c,0);put(*w,0x2690,0);
    put(*w,0x4f4,1);fput(*w,0x2f0,3);fput(*w,0x2f4,4);fput(*w,0x2f8,5);
    return w;
}
std::vector<UiDrawReplay> replays(const UiDrawList& list){std::vector<UiDrawReplay> out;
    for(const auto& c:list.commands())if(auto p=std::get_if<UiDrawReplay>(&c))out.push_back(*p);return out;}
void ordering_and_ownership(){
    std::array<std::shared_ptr<UiWindow>,8> registry{};registry[0]=window();auto& w=*registry[0];
    UiWindowDrawTestAccess::glyph(w,1,0);put(w,0xcc0,1); //Capacity scan, not cc0 upper bound.
    UiWindowDrawTestAccess::sprite(w,0);UiWindowDrawTestAccess::sprite(w,2);put(w,0xcc4,1);
    fput(w,0x54c,30);UiDrawState state{127,255};const auto sine=make_sine_table();
    auto list=build_ui_draw_list(registry,state,initial_ui_projection(),sine);
    check(state.background_alpha_top==0&&state.background_alpha_bottom==128,"Counter source wrap");
    check(w.word(0x4f4)==0,"Active window clears4f4");
    const auto draws=replays(list);check(draws.size()==6,"Two sprites plus two glyph replays");
    check(draws[0].family==UiDrawPacketFamily::SpriteHeader&&draws[1].family==UiDrawPacketFamily::SpriteElement&&
          draws[2].family==UiDrawPacketFamily::SpriteHeader&&draws[3].family==UiDrawPacketFamily::SpriteElement,
          "Header repeats immediately before each sprite's uploads and element");
    check(draws[0].element==0&&draws[2].element==2&&draws[0].packet==draws[2].packet,"Sparse elements share one immutable cache");
    check(draws[4].family==UiDrawPacketFamily::Glyph&&draws[4].group==1&&draws[4].pass==0&&draws[5].pass==1&&
          draws[4].packet==draws[5].packet,"Glyph selected cache replayed twice without color injection");
    unsigned auxiliary=0,colors=0;bool adjacent=false;
    for(unsigned i=0;i<list.commands().size();++i){const auto& c=list.commands()[i];
        if(auto v=std::get_if<UiDrawVectors>(&c)){
            if(v->kind==UiDrawVectors::Kind::Auxiliary){++auxiliary;check(v->values[0]==Float4({128,128,128,64}),"Current opacity auxiliary");}
            else{++colors;check(v->first==5||v->first==7,"Element color cache row");}
        }
        if(auto p=std::get_if<UiDrawReplay>(&c);p&&p->family==UiDrawPacketFamily::Glyph&&p->pass==0)
            adjacent=i+1<list.commands().size()&&std::holds_alternative<UiDrawReplay>(list.commands()[i+1]);
    }
    check(auxiliary==2&&colors==2&&adjacent,"No fabricated glyph shadow color uploads");
    UiWindowDrawTestAccess::glyph(w,1,0);registry.fill(nullptr);
    check(draws[4].packet->operations[0].arguments[1]==0x30003,"Submission retains snapshot after window destruction");
}
void gates_and_carry(){
    const auto sine=make_sine_table();UiDrawState state;
    std::array<std::shared_ptr<UiWindow>,8> registry{};
    registry[7]=window(7);UiWindowDrawTestAccess::glyph(*registry[7],0,0);put(*registry[7],0xcc0,1);
    check(replays(build_ui_draw_list(registry,state,initial_ui_projection(),sine)).empty()&&registry[7]->word(0x4f4)==1,"Slot7 excluded from draw and latch mutation");
    registry[0]=window();auto& w=*registry[0];w.set_opacity_tween(0,0,0,128);
    build_ui_draw_list(registry,state,initial_ui_projection(),sine);check(w.word(0x4f4)==1,"Zero opacity plus delta skips window");
    w.set_opacity_tween(.2f,0,0,128);build_ui_draw_list(registry,state,initial_ui_projection(),sine);
    check(w.word(0x4f4)==0,"Nonzero delta processes zero current opacity");
    registry[1]=window(1);auto& next=*registry[1];w.set_opacity_tween(0,64,0,128);
    for(auto* p:{&w,&next}){UiWindowDrawTestAccess::glyph(*p,0,0);put(*p,0xcc0,1);}
    fput(w,0x54c,0);fput(next,0x54c,30);fput(next,0x530,2);fput(next,0x534,3);fput(next,0x538,4);
    const auto list=build_ui_draw_list(registry,state,initial_ui_projection(),sine);std::vector<Matrix4> matrices;
    for(const auto& c:list.commands())if(auto m=std::get_if<UiDrawMatrix>(&c))matrices.push_back(m->value);
    check(matrices.size()==4&&matrices[2][3]==Float4({3,4,5,1})&&matrices[3][3]==Float4({5,7,9,1}),"Source glyph counter carries to next window translation");
    put(w,0x1900,w.word(0x1900)|0x8000);check(replays(build_ui_draw_list(registry,state,initial_ui_projection(),sine)).size()==3,"Draw does not reapply preparation inactive flag");
    put(w,0x268c,1);rejects([&]{build_ui_draw_list(registry,state,initial_ui_projection(),sine);});
    put(w,0x268c,0);put(w,0xcc4,1);put(w,0x3c08,w.word(0x3c08)&~8u);
    rejects([&]{build_ui_draw_list(registry,state,initial_ui_projection(),sine);});
    put(w,0xcc4,0);
    put(w,0x220,1);rejects([&]{build_ui_draw_list(registry,state,initial_ui_projection(),sine);});
}
void history_and_extra(){
    std::array<std::shared_ptr<UiWindow>,8> registry{};registry[0]=window();auto& w=*registry[0];
    UiWindowDrawTestAccess::sprite(w,0);put(w,0xcc4,1);put(w,0x4f8,0x41);put(w,0x4f0,2);
    for(unsigned j=0;j<3;++j){fput(w,0x300+j*4,float(3+j));fput(w,0x310+j*4,float(6+j));}
    UiDrawState state;const auto sine=make_sine_table();const auto list=build_ui_draw_list(registry,state,initial_ui_projection(),sine);
    const auto draws=replays(list);check(draws.size()==12,"Two unique histories times three sprite passes");
    check(draws[0].history==0&&draws[6].history==2&&draws[4].pass==2,"Duplicate history skipped in source order");
    std::vector<Float4> translations;for(const auto& c:list.commands())if(auto m=std::get_if<UiDrawMatrix>(&c))translations.push_back(m->value[3]);
    check(translations[1][1]==15&&translations[2][1]==16&&translations[3][1]==14,"Extra sprite passes shift plus and minus one");
    fput(w,0x3e4c,0);check(replays(build_ui_draw_list(registry,state,initial_ui_projection(),sine)).empty(),"Zero sprite alpha skips every pass");
    state.background_alpha_top=-2;build_ui_draw_list(registry,state,initial_ui_projection(),sine);
    check(state.background_alpha_top==-1&&state.background_alpha_bottom==127,"Counters use signed remainder");
}
}
int main(){try{ordering_and_ownership();gates_and_carry();history_and_extra();std::cout<<"UI draw submission tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
