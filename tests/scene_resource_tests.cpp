#include "okage/scene_resource.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
using namespace okage;
void check(bool value,unsigned where=__builtin_LINE()) {
    if(!value)throw std::runtime_error("Scene resource assertion line "+std::to_string(where));
}
template<class F>void rejects(F f) {
    bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}check(rejected);
}
int main(){try{
    std::vector<std::uint8_t> variables(0x2000,0xa5);
    std::array<std::uint32_t,3> command{2,96,17};
    SceneFade fade;fade.current=.75f;fade.target=1;fade.step=.125f;
    SceneClearColor clear;clear.rgb={12,34,56};
    BattleUiShared shared;
    auto& layout=shared.resource.layout_shared();
    layout.sprite_groups=8;layout.layer=7;layout.sprite_dirty=true;layout.glyph_metric={{2,65}};
    shared.resource.draw_state().background_alpha_top=17;
    shared.resource.draw_state().background_alpha_bottom=23;
    shared.resource_accumulators.fill(42);shared.atlas_dirty=true;
    const auto stream=shared.streams[0];
    DefeatFieldReturn field(variables,command,fade,clear,shared);
    for(unsigned i=0;i<variables.size();++i)check(variables[i]==(i>=0x10&&i<0x110?0:0xa5));
    check(command==std::array<std::uint32_t,3>{});
    check(fade.current==0&&fade.target==0&&fade.step==.125f&&fade.color==0);
    check(clear.rgb==std::array<std::uint8_t,3>{0,0,0});
    check(layout.sprite_groups==0&&layout.layer==0&&layout.sprite_dirty);
    check(layout.glyph_metric==std::optional<std::array<std::uint32_t,2>>({{2,65}}));
    check(shared.resource.draw_state().background_alpha_top==17&&shared.resource.draw_state().background_alpha_bottom==23);
    check(shared.streams[0]==stream&&shared.atlas_dirty&&shared.resource_accumulators[15]==42);
    check(field.resource().live()&&!field.resource().rendered_image());

    EffectDefinition definition;definition.id=25;definition.duration=400;definition.flags=0x9000;definition.elements.resize(8);
    for(unsigned i=0;i<8;++i){definition.elements[i].shape=i<3?24:29;definition.elements[i].primitive_flags=i<3?1:0x101;}
    EffectTextureSizes sizes;for(auto& size:sizes)size={64,64};
    RetiringSceneEffects effects;
    effects.map[4]=std::make_unique<MapEffect>(definition,Float4{0,0,420,1},Float4{},Float4{1,1,1,1},sizes,[]{return 1u;});
    effects.map[4]->flags=0x4120;
    Animation camera(3);reset_scene_camera(camera);
    FieldProjection projection;GameRandom random{0x12345678};const auto random_before=random.state;
    const auto sine=make_sine_table();
    GameModeParent parent({2,96,0});
    unsigned resource_ticks=0,destroyed=0,released=0,constructed=0;
    auto traversal=[&]{
        parent.tick([&]{++destroyed;field.retire(camera,projection,&effects,shared,fade);},
            [&]{++released;check(!field.resource().live());},
            [&](auto selection){++constructed;check(selection==GameModeSelection{8,4,0});});
        if(parent.has_child()&&parent.current()[0]==2){
            field.tick([&](auto selection){parent.request(selection);});
            check(parent.requested()==GameModeSelection{8,4,0});
            field.resource().tick(camera,projection,&effects,shared,{},sine,random);++resource_ticks;
        }
    };
    traversal();check(resource_ticks==1&&effects.flags()[4]==0x4320);
    const auto* image=field.resource().rendered_image();check(image&&image->width==640&&image->height==224);
    for(unsigned i=0;i<image->rgba.size();++i)check(image->rgba[i]==(i%4==3?64:0));
    check(std::bit_cast<std::uint32_t>(field.resource().camera_position()[0])==0x415170a4);
    check(std::bit_cast<std::uint32_t>(field.resource().camera_rotation()[1])==0x3d1ba5e3);
    check(random.state==random_before);
    traversal();check(destroyed==1&&released==0&&resource_ticks==1&&!field.resource().rendered_image());
    check(effects.flags()[4]==0x4320&&std::bit_cast<std::uint32_t>(fade.step)==0x3ca3d70a);
    traversal();check(released==1&&constructed==0&&resource_ticks==1&&effects.flags()[4]==0x4320);
    traversal();check(constructed==1&&resource_ticks==1&&effects.flags()[4]==0x4320);
    rejects([&]{field.resource().tick(camera,projection,&effects,shared,{},sine,random);});
    rejects([&]{field.tick([](auto){});});
    EmptySceneResource next(clear,fade,shared);
    next.tick(camera,projection,&effects,shared,{},sine,random);check(!effects.map[4]);
    check(random.state==random_before&&shared.resource.draw_state().background_alpha_top==17);
    effects.map[4]=std::make_unique<MapEffect>(definition,Float4{0,0,420,1},Float4{},Float4{1,1,1,1},sizes,[]{return 1u;});
    effects.map[4]->flags=0x4020;
    rejects([&]{next.tick(camera,projection,&effects,shared,{},sine,random);});
    effects.map[4].reset();
    command={2,0,0};fade.color=0x123456;
    DefeatFieldReturn colored(variables,command,fade,clear,shared);
    check(fade.color==0&&fade.published_color()==0x123456&&fade.alpha()==128);
    fade.tick();check(fade.published_color()==0);
    command={1,0,0};rejects([&]{DefeatFieldReturn bad(variables,command,fade,clear,shared);});
    command={2,0,0};rejects([&]{DefeatFieldReturn bad(std::span(variables).first(0x10f),command,fade,clear,shared);});
    shared.registry[0]=std::make_shared<UiWindow>(4,4,0,stream);
    rejects([&]{EmptySceneResource bad(clear,fade,shared);});
    rejects([&]{next.tick(camera,projection,&effects,shared,{},sine,random);});
    std::cout<<"Scene resource lifecycle tests passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
