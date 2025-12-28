#include "okage/field.hpp"
#include "okage/event.hpp"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F&& call) {
    try {call();} catch(const std::runtime_error&) {return;}
    throw std::runtime_error("Expected field staging rejection");
}
void word(std::vector<std::uint8_t>& bytes,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=std::uint8_t(value>>(8*i));
}
std::vector<std::uint8_t> idle() {
    std::vector<std::uint8_t> result(80);
    word(result,0,0x544d58);word(result,4,48);word(result,16,0xffffffff);
    return result;
}
struct Fixture {
    std::vector<std::uint8_t> motion=idle(),stage=std::vector<std::uint8_t>(116),collision=std::vector<std::uint8_t>(69);
    okage::FieldDefinition definition;
    std::vector<std::unique_ptr<okage::FieldActor>> actors;
    std::vector<okage::ActorBinding> bindings;
    std::array<std::uint8_t,0x1000> variables{};
    std::vector<std::uint32_t> draws;
    std::vector<int> rounding;
    std::unique_ptr<okage::OpeningField> field;
    explicit Fixture(std::vector<std::int32_t> states={0,5},bool reversed=false,bool deferred=false,bool stun=false) {
        word(stage,0,0x475453);word(stage,16,32);word(stage,20,1);word(stage,24,88);word(stage,28,1);
        word(stage,64,std::bit_cast<std::uint32_t>(10.f));word(stage,76,std::bit_cast<std::uint32_t>(1.f));
        for(unsigned i=0;i<3;++i)word(collision,i*4,1);word(collision,65,64);
        definition.scene=72;definition.type=1;definition.footstep=41;definition.dust=0;
        for(unsigned i=0;i<states.size();++i) {
            okage::FieldNpcDefinition npc;
            npc.alias=-8-int(i);npc.state=states[i];npc.model=i==0?"rkumo":"wkumo";
            npc.position={30.f+20.f*i,0,30,1};npc.facing=.125f*i;npc.flags=i==0?5:1;
            definition.npcs.push_back(npc);
        }
        for(unsigned i=0;i<states.size()+1+unsigned(stun);++i) {
            okage::ModelFile model;model.models.emplace_back();
            actors.push_back(std::make_unique<okage::FieldActor>(std::move(model),motion,okage::TextureAnimation{},
                false,false,std::shared_ptr<okage::TextureBindings>{},deferred));
            const bool is_stun=stun&&i==states.size();
            bindings.push_back({is_stun?-3:i==states.size()+unsigned(stun)?-1:-8-int(i),actors.back().get(),!is_stun});
        }
        if(reversed&&states.size()>1)std::swap(bindings[0],bindings[1]);
        field=std::make_unique<okage::OpeningField>(motion,[](int){},[](int){return true;},bindings,false,deferred);
        okage::set_event_variable(variables,6,4,1);
        field->get_variable=[this](auto selector){return okage::event_variable(variables,selector);};
        field->set_variable=[this](auto index,auto bank,auto value){okage::set_event_variable(variables,index,bank,value);};
        field->random_value=[this] {
            rounding.push_back(std::fegetround());
            const auto value=draws.size()==0?0x20u:draws.size()==1?0x40u:0u;
            draws.push_back(value);return value;
        };
    }
    void begin() {field->begin_prepare(definition,stage,collision);}
    void prepare() {field->prepare(definition,stage,collision);}
};
void equal_actors(const Fixture& a,const Fixture& b) {
    require(a.actors.size()==b.actors.size(),"actor count differs");
    for(unsigned i=0;i<a.actors.size();++i) {
        auto& x=*a.actors[i];auto& y=*b.actors[i];
        require(x.animation().values()==y.animation().values()&&x.visible==y.visible&&
                x.encounter_model==y.encounter_model&&x.shadow_mode_override==y.shadow_mode_override,
                "staged and complete preparation actor state differs");
    }
}
}

int main() {try {
    const int prior_rounding=std::fegetround();
    struct RestoreRounding {int value;~RestoreRounding(){std::fesetround(value);}} restore{prior_rounding};
    std::fesetround(FE_UPWARD);
    {
        auto legacy=std::make_unique<Fixture>(),staged=std::make_unique<Fixture>();
        rejects([&]{staged->field->prepare_next_npc();});
        rejects([&]{staged->field->finish_prepare();});
        staged->begin();
        require(staged->draws.empty()&&std::fegetround()==FE_UPWARD,"begin consumes no RNG and restores rounding");
        require(!staged->actors[0]->encounter_model&&!staged->actors[1]->encounter_model,
                "begin does not initialize NPC runtime flags");
        rejects([&]{staged->begin();});rejects([&]{staged->prepare();});
        rejects([&]{staged->field->finish_prepare();});rejects([&]{staged->field->activate(0);});
        rejects([&]{staged->field->tick();});
        staged->field->prepare_next_npc();
        require(staged->draws==std::vector<std::uint32_t>{0x20}&&staged->actors[0]->encounter_model&&
                !staged->actors[1]->encounter_model,"one next initializes exactly the first actual NPC");
        rejects([&]{staged->field->finish_prepare();});rejects([&]{staged->field->tick();});
        require(staged->draws.size()==1,"interruption/lifecycle checks consume no draws");
        staged->field->prepare_next_npc();
        require(staged->draws==std::vector<std::uint32_t>({0x20,0x40})&&staged->actors[1]->encounter_model,
                "second next initializes exactly the second NPC without precomputation");
        rejects([&]{staged->field->prepare_next_npc();});
        staged->field->finish_prepare();
        rejects([&]{staged->field->finish_prepare();});rejects([&]{staged->field->prepare_next_npc();});
        legacy->prepare();
        require(legacy->draws==staged->draws&&legacy->rounding==staged->rounding&&
                staged->rounding==std::vector<int>({FE_TOWARDZERO,FE_TOWARDZERO}),
                "complete prepare preserves construction draw sequence and rounding");
        equal_actors(*legacy,*staged);
        require(staged->field->player_movement().footstep_sound==41&&staged->field->collision_bodies().empty(),
                "begin preserves settings while activation remains deferred");
        // Activation must read the copies owned at begin, even after a delayed continuation.
        std::fill(staged->stage.begin(),staged->stage.end(),0xff);
        std::fill(staged->collision.begin(),staged->collision.end(),0xff);
        legacy->field->activate(0);staged->field->activate(0);
        equal_actors(*legacy,*staged);
        // The real state-5 cooldown from the second draw 0x40 is 400. No
        // further draw occurs in its first 400 callbacks; callback401 draws.
        for(unsigned tick=0;tick<401;++tick) {
            legacy->field->tick();staged->field->tick();
            require(legacy->draws==staged->draws,"staged cooldown changes subsequent shared RNG sequence");
            require(staged->draws.size()==(tick<400?2u:3u),"real initialized cooldown differs from source draw");
            equal_actors(*legacy,*staged);
        }
        require(std::fegetround()==FE_UPWARD,"field operations restore caller rounding");
    }
    {
        auto legacy=std::make_unique<Fixture>(std::vector<std::int32_t>{1});
        auto staged=std::make_unique<Fixture>(std::vector<std::int32_t>{1});
        legacy->prepare();staged->begin();staged->field->prepare_next_npc();staged->field->finish_prepare();
        std::vector<std::pair<int,okage::Float4>> a,b;
        legacy->field->observe_wander=[&](int,bool,const auto& state,const auto&){a.emplace_back(state.counter,state.velocity);};
        staged->field->observe_wander=[&](int,bool,const auto& state,const auto&){b.emplace_back(state.counter,state.velocity);};
        legacy->field->activate(0);
        legacy->field->tick();
        unsigned boundary_calls=0;
        staged->field->activate_entry(0,{},[&] {
            ++boundary_calls;
            require(staged->draws.size()==1&&b.empty()&&staged->field->exploration_ready(),
                    "132720 return hook sees activation before the first wandering descendant");
        });
        require(boundary_calls==1,"activation return has exactly one boundary hook");
        require(a==b&&a.size()==2&&a.front().first==0&&a.front().second==okage::Float4{}&&a.back().first>0,
                "staged wandering NPC starts with the same real runtime state");
        require(legacy->draws==staged->draws&&a.back().second!=okage::Float4{}&&staged->draws.size()==3,
                "wander callback consumes its two real draws after the constructor draw");
        equal_actors(*legacy,*staged);
        auto returned=std::make_unique<Fixture>(std::vector<std::int32_t>{1});returned->prepare();
        unsigned return_descendants=0,return_boundaries=0;
        returned->field->observe_wander=[&](int,bool,const auto&,const auto&){++return_descendants;};
        const okage::FieldBattleReturnSnapshot saved{{3,0,4,1},{0,.25f,0,1},{10,-20,30,1},72,0,-1};
        returned->field->activate_return(saved,{},[&]{
            ++return_boundaries;
            require(returned->draws.size()==1&&return_descendants==0&&returned->field->exploration_ready(),
                    "return activation hook precedes same-frame wandering descendants");
            require(returned->field->player_movement().history.front()==okage::Float4({3,1,4,1}),
                    "return activation hook sees the settled source player pose");
        });
        require(return_boundaries==1&&return_descendants==2&&returned->draws.size()==3,
                "return activation runs one boundary and one real descendant traversal");
        require(returned->field->position==saved.raw_camera_position,
                "the subsequent camera consumer observes the restored raw channel");
    }
    {
        auto empty=std::make_unique<Fixture>(std::vector<std::int32_t>{});
        empty->field->random_value={};empty->begin();
        rejects([&]{empty->field->prepare_next_npc();});
        empty->field->finish_prepare();empty->field->activate(0);
        require(empty->draws.empty()&&empty->field->exploration_ready(),"zero-NPC staging needs no RNG service");
        auto legacy=std::make_unique<Fixture>(std::vector<std::int32_t>{});
        legacy->field->random_value={};legacy->prepare();legacy->field->activate(0);equal_actors(*legacy,*empty);
    }
    {
        auto bad=std::make_unique<Fixture>(std::vector<std::int32_t>{0,5},true);
        bad->begin();rejects([&]{bad->field->prepare_next_npc();});
        require(bad->draws.empty(),"binding-order failure precedes its NPC draw");
        auto legacy=std::make_unique<Fixture>(std::vector<std::int32_t>{0,5},true);
        rejects([&]{legacy->prepare();});require(legacy->draws.empty(),"legacy binding-order error is unchanged");
        auto invalid=std::make_unique<Fixture>(std::vector<std::int32_t>{2});
        rejects([&]{invalid->begin();});require(invalid->draws.empty(),"invalid NPC state fails validation before RNG");
    }
    {
        auto interrupted=std::make_unique<Fixture>();
        interrupted->begin();interrupted->field->prepare_next_npc();
        require(interrupted->draws.size()==1,"only reached NPC initialized before cancellation");
        interrupted->field.reset();
        require(interrupted->draws.size()==1,"destroying partial field invents no completion or RNG draw");
    }
    {
        auto legacy=std::make_unique<Fixture>(std::vector<std::int32_t>{0,5},false,false,true);
        auto staged=std::make_unique<Fixture>(std::vector<std::int32_t>{0,5},false,true,true);
        rejects([&]{staged->field->tick();});
        rejects([&]{staged->field->setup_actor(1);});
        rejects([&]{staged->field->setup_actor(0);}); //source preparation has not begun.
        const auto untouched=staged->actors[0]->animation().channel_outputs();
        staged->begin();
        rejects([&]{staged->field->setup_actor(0);}); //source motion is not initialized.
        require(staged->actors[0]->animation().channel_outputs()==untouched&&
                !staged->actors[0]->shadow_mode_override&&staged->actors[2]->visible,
                "deferred begin validates without posing NPCs or hiding STUN early");
        rejects([&]{staged->field->prepare_next_npc();});
        require(staged->draws.empty(),"NPC resource/setup guard precedes its real RNG draw");
        std::vector<std::string> order;
        const auto draw=staged->field->random_value;
        staged->field->random_value=[&]{order.push_back("rng"+std::to_string(staged->draws.size()));return draw();};
        for(unsigned i=0;i<staged->actors.size();++i) {
            order.push_back("resource"+std::to_string(i));
            staged->actors[i]->initialize_motion();
            staged->field->setup_actor(i);order.push_back("instance"+std::to_string(i));
            rejects([&]{staged->field->setup_actor(i);});
            if(i<staged->definition.npcs.size()) {
                rejects([&]{staged->field->setup_actor(i+1);});
                require(staged->draws.size()==i,"next actor cannot bypass the preceding NPC draw");
                staged->field->prepare_next_npc();
                require(staged->actors[i]->animation().value(1)==staged->definition.npcs[i].position,
                        "authored NPC pose follows source initial world reset");
            }
            if(i+1<staged->actors.size()) {
                rejects([&]{staged->field->tick();});rejects([&]{staged->field->finish_prepare();});
            }
        }
        require(order==std::vector<std::string>({"resource0","instance0","rng0","resource1","instance1","rng1",
                    "resource2","instance2","resource3","instance3"}),
                "resources, source instances and NPC constructor draws remain interleaved");
        require(!staged->actors[2]->visible&&staged->actors[2]->animation().value(1)==okage::Float4({0,-2,0,1}),
                "STUN pose/hide occurs only at its reached source setup");
        staged->field->finish_prepare();legacy->prepare();
        require(legacy->draws==staged->draws&&legacy->rounding==staged->rounding,
                "deferring source setup invents no RNG draw or rounding change");
        equal_actors(*legacy,*staged);
        legacy->field->activate_entry(0);staged->field->activate_entry(0);equal_actors(*legacy,*staged);
        for(unsigned i=0;i<12;++i){legacy->field->tick();staged->field->tick();equal_actors(*legacy,*staged);}
        require(legacy->draws==staged->draws,"later NPC callbacks retain the same random chain");
        rejects([&]{okage::OpeningField diagnostic(staged->motion,[](int){},[](int){return true;},staged->bindings,true,true);});
    }
    {
        auto batched=std::make_unique<Fixture>(std::vector<std::int32_t>{0,5},false,true);
        batched->begin();batched->actors[0]->initialize_motion();batched->actors[1]->initialize_motion();
        batched->field->setup_actor(0);rejects([&]{batched->field->setup_actor(1);});
        require(batched->draws.empty(),"preinitialized next actor cannot bypass the preceding NPC draw");
        batched->field->prepare_next_npc();batched->field->setup_actor(1);batched->field->prepare_next_npc();
        require(batched->draws==std::vector<std::uint32_t>({0x20,0x40}),
                "recovering from a rejected batch preserves exactly the real constructor draws");
    }
    std::cout<<"field NPC staging tests passed\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
