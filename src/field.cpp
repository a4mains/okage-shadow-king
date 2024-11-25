#include "okage/field.hpp"
#include "okage/map.hpp"
#include "okage/map_objects.hpp"
#include "okage/event.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cfenv>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
std::uint32_t get(Bytes b,std::size_t at) {
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("Field asset word bounds");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|(std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
struct Rounding {
    int old=std::fegetround();
    Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Field rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
}
std::uint32_t OpeningField::word(std::size_t at) const{return get(bytes_,at);}
Float4 OpeningField::point(std::size_t at) const {
    Float4 p{0,0,0,1};for(unsigned i=0;i<3;++i){p[i]=std::bit_cast<float>(word(at+4*i));if(!std::isfinite(p[i]))throw std::runtime_error("Invalid field point");}return p;
}
OpeningField::OpeningField(Bytes bytes,std::function<void(int)> start,std::function<bool(int)> wait,std::span<const ActorBinding> actors,bool diagnostic):
    bytes_(bytes.begin(),bytes.end()),script_(bytes,3),start_dialogue_(std::move(start)),wait_dialogue_(std::move(wait)) {
    actors_.assign(actors.begin(),actors.end());
    for(std::size_t i=0;i<actors_.size();++i) {
        const auto& binding=actors_[i];
        if(binding.alias>=0 || binding.alias < -2048 || binding.alias==-2 || !binding.actor)throw std::runtime_error("Invalid actor binding");
        for(std::size_t j=0;j<i;++j)if(actors_[j].alias==binding.alias || actors_[j].actor==binding.actor)throw std::runtime_error("Duplicate actor binding");
        if(!binding.collidable && binding.alias!=-3)throw std::runtime_error("Unrecovered noncolliding field actor");
        if(binding.alias==-3) {
            if(binding.collidable||i+2!=actors_.size()||actors_.back().alias!=-1)
                throw std::runtime_error("STUN requires no collision and construction immediately before HERO");
            //0x134858 constructs STUN at(0,-2,0), then0x134980 hides it.
            binding.actor->animation().set_channel(1,{0,-2,0,1});
            binding.actor->visible=false;shadow_.emplace();
            shadow_->stun_position={0,-2,0,1};
        }
        binding.actor->select_motion(1); //0x138220 NPC and0x134858 STUN/HERO setup.
    }
    if(!start_dialogue_ || !wait_dialogue_)throw std::runtime_error("Field requires dialogue services");
    script_.set_service_callback([this](auto op,auto a,auto b,auto pc){return service(op,a,b,pc);});
    if(!diagnostic)return;
    if(!actors_.empty() && !actor(-8))throw std::runtime_error("Opening actor diagnostic requires MAM alias -8");
    if(actor(-1) && !actor(-9))throw std::runtime_error("Opening player diagnostic requires IMO alias -9");
    // This diagnostic entry is tied to the documented USA HOME1C script.
    if(word(0xb88)!=0xffe00007 || word(0xb8c)!=0x30 || word(0xbb8)!=0x20000a)
        throw std::runtime_error("Unsupported opening event layout");
    if(actor(-8)) {
        if(word(0xb6c)!=0xff800007 || word(0xb70)!=0x400030 || word(0xb80)!=0x10002e)
            throw std::runtime_error("Unsupported opening actor setup layout");
        if(actor(-9) && word(0xb68)!=0xff71003b)throw std::runtime_error("Unsupported opening visibility layout");
        if(actor(-1) && word(0xb64)!=0xfff1003b)throw std::runtime_error("Unsupported opening player visibility layout");
        script_.start_at(actor(-1) ? 0xb64 : (actor(-9) ? 0xb68 : 0xb6c));
        for(const auto& binding:actors_)binding.actor->evaluate();
    } else script_.start_at(0xb88);
    evaluate();
}
FieldActor* OpeningField::actor(int alias) const {
    if(alias==-7&&alias7_actor_)return alias7_actor_;
    for(const auto& binding:actors_)if(binding.alias==alias)return binding.actor;
    for(const auto& binding:map_objects_)if(binding.alias==alias)return binding.actor;
    const auto instance=alias>=0?std::optional<int>(alias):
        (resolve_instance_alias?resolve_instance_alias(alias):std::nullopt);
    if(instance&&*instance>=0) {
        for(const auto& binding:actors_)if(binding.instance==*instance)return binding.actor;
        for(const auto& binding:map_objects_)if(binding.alias==*instance)return binding.actor;
    }
    return nullptr;
}
FieldBattleReturnSnapshot OpeningField::battle_return_snapshot() {
    auto* hero=actor(-1);
    if(!definition_||!hero||hero->animation().channel_outputs().size()<2||script_.channel_outputs().empty())
        throw std::runtime_error("Battle return snapshot requires an entered field and HERO/camera channels");
    // 0x1301a8 reads HERO group+1, HERO group, then camera group. values()
    // resolves the same dirty output as0x1b43c0; it never ticks interpolation.
    const auto player=hero->animation().values();
    const auto camera=script_.values();
    return {player[1],player[0],camera[0],definition_->scene,definition_->secondary_music,
        definition_->battle_record_selection};
}
RetiringSceneEffects OpeningField::retire_effects() {
    RetiringSceneEffects retired{std::move(map_effects_),std::move(effects_)};
    retired.mark();effect_submissions_.clear();
    return retired;
}
void OpeningField::validate_retirement() const {
    if(hat_requested_||hat_actor_||camera_motion_||alias7_actor_)
        throw std::runtime_error("Exceptional field child retirement is unrecovered");
    if(shadow_&&shadow_->voice>=0)
        throw std::runtime_error("Field retirement owned voice release is unrecovered");
}
bool OpeningField::request_actor_motion(int alias,unsigned entry) {
    //0x130158 resolves a negative alias then calls0x1b6fb8 directly. Unlike
    // event36, this caller has no camera selector or high-entry stop branch.
    if(auto* target=actor(alias)){target->select_motion(entry);return true;}
    const auto instance=alias>=0?std::optional<int>(alias):
        (resolve_instance_alias?resolve_instance_alias(alias):std::nullopt);
    return instance&&(*instance<0||(map_scene_&&map_scene_->select_motion(*instance,entry)));
}
void OpeningField::bind_map_objects(std::span<const ActorBinding> bindings) {
    if(!map_objects_.empty())throw std::runtime_error("Map objects already bound");
    for(std::size_t i=0;i<bindings.size();++i) {
        const auto& b=bindings[i];
        if(b.alias<0||b.alias>=640||!b.actor)throw std::runtime_error("Invalid map object binding");
        for(std::size_t j=0;j<i;++j)if(bindings[j].alias==b.alias||bindings[j].actor==b.actor)
            throw std::runtime_error("Duplicate map object binding");
        for(const auto& npc:actors_)if(npc.actor==b.actor)throw std::runtime_error("Map object also bound as field actor");
    }
    map_objects_.assign(bindings.begin(),bindings.end());
}
void OpeningField::bind_map_scene(MapObjects& scene) {
    if(map_scene_||collision_)throw std::runtime_error("Map scene binding lifecycle");
    bind_map_objects(scene.bindings());map_scene_=&scene;
    channel_pool_=std::make_shared<AnimationChannelPool>();
    script_.bind_channels(channel_pool_);
    scene.bind_channels(channel_pool_);
    for(const auto& binding:actors_)binding.actor->animation().bind_channels(channel_pool_);
    map_visibility=[this](int id,bool value) {
        const auto index=map_scene_->collision_index(id);
        if(!index)return false;
        bool visible=value;
        set_instance_visibility(visible,*index,bodies_,value);
        return map_scene_->set_visible(id,value);
    };
}
void OpeningField::sync_collision_positions() {
    for(unsigned i=0;i<map_body_count_;++i)
        bodies_[i].position=map_scene_->position(map_scene_->collision_bodies()[i].placement);
    for(unsigned i=0;i<actors_.size();++i)if(actors_[i].collidable)
        bodies_[actor_collision_indices_[i]].position=actors_[i].actor->animation().values().at(1);
}
void OpeningField::commit_collision_positions(unsigned mover) {
    for(unsigned i=0;i<map_body_count_;++i)
        map_scene_->write_position(map_scene_->collision_bodies()[i].placement,bodies_[i].position);
    for(unsigned i=0;i<actors_.size();++i) {
        if(!actors_[i].collidable)continue;
        const auto index=actor_collision_indices_[i];
        if(index==mover)actors_[i].actor->animation().set_channel(1,bodies_[index].position);
        else actors_[i].actor->animation().write_channel_output(1,bodies_[index].position);
    }
    for(auto& body:bodies_)body.previous=body.position;
}
static FieldDefinition read_definition(Bytes data,const std::string& room,int scene) {
    if(get(data,0)!=0x424458 || get(data,16)!=0xffffffff)throw std::runtime_error("Invalid field XDB header");
    const auto count=get(data,12);const std::uint64_t base=32ull+16ull*count,strings=base+get(data,4);
    if(count>65536 || strings>data.size() || strings+get(data,8)!=data.size())throw std::runtime_error("Field XDB section bounds");
    auto string=[&](std::uint32_t offset) {
        const auto at=strings+offset;if(at>=data.size())throw std::runtime_error("Field XDB string bounds");
        const auto first=data.begin()+at,last=std::find(first,data.end(),0);
        if(last==data.end())throw std::runtime_error("Unterminated field XDB string");
        return std::string(first,last);
    };
    std::optional<FieldDefinition> found;
    for(unsigned i=0;i<count;++i) {
        const auto row=32+i*16;const auto name=string(get(data,row));
        if(!name.starts_with("scene"))continue;
        const auto at=base+get(data,row+4);const auto words=get(data,row+8);
        if((at&3)||words<13||at+std::uint64_t(words)*4>strings)throw std::runtime_error("Field XDB record bounds");
        if(scene>=0 ? name!="scene"+std::to_string(scene) : string(get(data,at+20))!=room)continue;
        if(found)throw std::runtime_error("Ambiguous field room definition");
        FieldDefinition result;result.type=get(data,at+4);result.event=string(get(data,at+28));
        const auto number=name.substr(5);
        if(number.empty()||number.size()>6||!std::all_of(number.begin(),number.end(),[](char c){return c>='0'&&c<='9';}))
            throw std::runtime_error("Invalid field scene identifier");
        result.scene=std::stoi(number);result.party=get(data,at);result.startup=get(data,at+12);result.field_flags=get(data,at+8);
        result.music=std::bit_cast<std::int32_t>(get(data,at+40));result.secondary_music=std::bit_cast<std::int32_t>(get(data,at+44));
        result.room_directory=string(get(data,at+16));result.room=string(get(data,at+20));result.message=string(get(data,at+24));
        unsigned index=12;
        for(;index<words && get(data,at+index*4);index+=8) {
            if(index+8>words || result.npcs.size()>=256)throw std::runtime_error("Field NPC definition bounds");
            const auto p=at+index*4;FieldNpcDefinition npc;
            npc.alias=-8-int(result.npcs.size());npc.model=string(get(data,p));
            npc.state=std::bit_cast<std::int32_t>(get(data,p+4));npc.talk=std::bit_cast<std::int32_t>(get(data,p+8));
            npc.alternate_talk=std::bit_cast<std::int32_t>(get(data,p+12));npc.position[3]=1;
            for(unsigned k=0;k<3;++k)npc.position[k]=std::bit_cast<float>(get(data,p+16+k*4));
            npc.facing=std::bit_cast<float>(get(data,p+28));
            if(!std::all_of(npc.position.begin(),npc.position.end(),[](float v){return std::isfinite(v);})||!std::isfinite(npc.facing))
                throw std::runtime_error("Non-finite field NPC definition");
            result.npcs.push_back(std::move(npc));
        }
        if(index>=words)throw std::runtime_error("Missing field NPC terminator");
        found=std::move(result);
    }
    if(!found)throw std::runtime_error("Missing field room definition");
    return *found;
}
FieldDefinition read_field_definition(Bytes data,const std::string& room) {return read_definition(data,room,-1);}
FieldDefinition read_field_definition(Bytes data,int scene) {
    if(scene<0)throw std::runtime_error("Negative field scene requires another game mode");
    return read_definition(data,"",scene);
}
std::array<std::uint8_t,0x800> field_setup_state(Bytes script,Bytes variables,const FieldDefinition& definition) {
    std::array<std::uint8_t,0x800> setup{};std::fill_n(setup.begin()+0x1a8,256,1);
    auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)setup[at+i]=value>>(i*8);};
    put(0x2b4,0xffffffff);put(0x2b8,0xffffffff);put(0x114,0xc5700000);
    put(0x400,definition.music);put(0x404,definition.secondary_music);put(0x408,definition.type);
    put(0x40c,definition.field_flags);put(0x410,definition.startup);
    put(0x414,definition.type==1?0:17);put(0x41c,0x40000000);put(0x428,0xffffffff);put(0x42c,33);
    auto path=[&](unsigned at,const std::string& value) {
        if(value.size()>=64)throw std::runtime_error("Field resource path exceeds inline buffer");
        std::copy(value.begin(),value.end(),setup.begin()+at);setup[at+value.size()]=0;
    };
    path(0x2c0,definition.room+".xpf");path(0x300,"flddata/"+definition.room_directory+"/");
    run_event_setup(script,variables,setup);
    return setup;
}
FieldDefinition prepare_field_definition(Bytes script,Bytes variables,FieldDefinition definition) {
    const auto setup=field_setup_state(script,variables,definition);
    auto path=[&](unsigned at) {
        const auto first=setup.begin()+at,last=std::find(first,first+64,0);
        if(last==first+64)throw std::runtime_error("Unterminated field resource path");
        return std::string(first,last);
    };
    const auto map=path(0x2c0),directory=path(0x300);
    if(!map.ends_with(".xpf")||!directory.starts_with("flddata/")||!directory.ends_with("/"))
        throw std::runtime_error("Invalid field resource path format");
    definition.room=map.substr(0,map.size()-4);
    definition.room_directory=directory.substr(8,directory.size()-9);
    definition.type=get(setup,0x408);definition.startup=get(setup,0x410);
    definition.field_flags=get(setup,0x40c);
    definition.music=std::bit_cast<std::int32_t>(get(setup,0x400));definition.secondary_music=std::bit_cast<std::int32_t>(get(setup,0x404));
    definition.battle_record_selection=std::bit_cast<std::int32_t>(get(setup,0x428));
    definition.footstep=get(setup,0x42c);definition.dust=get(setup,0x414);
    for(auto& npc:definition.npcs)npc.flags=setup[0x1a8+(-8-npc.alias)];
    std::erase_if(definition.npcs,[](const auto& npc){return !npc.flags;});
    return definition;
}
FieldDefinition prepare_field_load(Bytes script,std::span<std::uint8_t> variables,
                                    FieldDefinition definition,unsigned entrance) {
    // 0x131d28 applies the persistent party disable and writes the encounter
    // gate BEFORE entry zero. The constructor owns the earlier125ad0 clear.
    if(variables.size()<0x588 || definition.scene<0 || entrance>0x7fffffff)
        throw std::runtime_error("Invalid field entry variables/scene/entrance");
    set_event_variable(variables,0,4,definition.scene);
    set_event_variable(variables,1,4,int(entrance));
    if(event_variable(variables,0x40005))definition.party=0;
    set_event_variable(variables,6,4,definition.party==0);
    definition=prepare_field_definition(script,variables,std::move(definition));
    definition.followers.clear();
    if(definition.party)for(unsigned index=25;index<30;++index)
        if(event_variable(variables,0x40000|index)==3)definition.followers.push_back(index);
    return definition;
}
FieldDefinition prepare_field_entry(Bytes script,std::span<std::uint8_t> variables,
                                    FieldDefinition definition,unsigned entrance) {
    // Preserve the original convenience API's validation/clear ordering.
    if(variables.size()<0x588 || definition.scene<0 || entrance>0x7fffffff)
        throw std::runtime_error("Invalid field entry variables/scene/entrance");
    std::fill(variables.begin()+0x10,variables.begin()+0x110,0); //125ad0
    return prepare_field_load(script,variables,std::move(definition),entrance);
}
FieldDefinition prepare_field_return(Bytes script,std::span<std::uint8_t> variables,
                                     FieldDefinition definition) {
    // 0x132720 interprets a negative entrance as the saved-pose path. Unlike
    // ordinary entry, the command-13 path does not call 0x125ad0, so preserve
    // the temporary flag block byte-for-byte.
    if(variables.size()<0x588||definition.scene<0)
        throw std::runtime_error("Invalid field return variables/scene");
    set_event_variable(variables,0,4,definition.scene);
    set_event_variable(variables,1,4,-1);
    if(event_variable(variables,0x40005))definition.party=0;
    set_event_variable(variables,6,4,definition.party==0);
    definition=prepare_field_definition(script,variables,std::move(definition));
    definition.followers.clear();
    if(definition.party)for(unsigned index=25;index<30;++index)
        if(event_variable(variables,0x40000|index)==3)definition.followers.push_back(index);
    return definition;
}
void OpeningField::load_interactions(Bytes database,const std::string& room,Bytes variables) {
    if(definition_||!actor(-1))throw std::runtime_error("Field interactions require one configuration and a player");
    auto definition=prepare_field_definition(bytes_,variables,read_field_definition(database,room));
    if(definition.event!="e_"+room)throw std::runtime_error("Field event definition does not match diagnostic room");
    for(const auto& npc:definition.npcs)if(!actor(npc.alias)||npc.state!=0)
        throw std::runtime_error("Field NPC binding/state is not implemented");
    if(definition.npcs.size()+1+unsigned(shadow_.has_value())!=actors_.size())throw std::runtime_error("Field NPC bindings differ from enabled definitions");
    definition_=std::move(definition);initialize_npcs();
}
void OpeningField::initialize_npcs() {
    if(!definition_||!enemies_.empty())throw std::runtime_error("NPC configuration lifecycle");
    for(std::size_t i=0;i<definition_->npcs.size();++i) {
        const auto& npc=definition_->npcs[i];
        if(actors_.at(i).alias!=npc.alias)throw std::runtime_error("NPC collision bindings must preserve construction order");
        if(!random_value)throw std::runtime_error("NPC construction requires the shared game RNG");
        const auto cooldown=EncounterEnemy::initial_cooldown(random_value(),definition_->type==2?2:0);
        enemies_.push_back(npc.state==5?std::optional<EncounterEnemy>({0,cooldown,-.2f}):std::nullopt);
        wanderers_.push_back(npc.state==1?std::make_optional<WanderingNpc>():std::nullopt);
        actor(npc.alias)->encounter_model=npc.model=="rkumo"||npc.model=="wkumo"||npc.model=="bkumo";
    }
}
void OpeningField::update_npcs() {
    if(!definition_)return;
    Rounding rounding;
    for(std::size_t index=0;index<enemies_.size();++index) {
        if(wanderers_[index]&&definition_->npcs[index].state==1) {
            auto& wander=*wanderers_[index];const auto alias=definition_->npcs[index].alias;
            auto* object=actor(alias);auto& animation=object->animation();
            if(observe_wander)observe_wander(alias,false,wander,animation);
            const auto before=animation.values().at(1);
            if(wander.counter<=0) {
                const auto facing=wander.begin_leg(before,0,random_value,angles_);
                object->select_motion(2);animation.set_channel(0,facing);
            } else {
                if(!collision_)throw std::runtime_error("Wandering NPC requires field collision");
                sync_collision_positions();const auto body_index=actor_collision_indices_.at(index);
                auto desired=before;for(unsigned k=0;k<4;++k)desired[k]+=wander.velocity[k];
                MapCollisionSettings settings;settings.wall_iterations=3;settings.disabled_mask=collision_mask->bits;
                const bool corrected=resolve_collision(*collision_,bodies_,body_index,desired,settings,std::bit_cast<float>(0x3efff17du));
                if(!corrected||!(bodies_[body_index].flags&1)||desired[3]!=1)
                    throw std::runtime_error("Uncorrected NPC wander W semantics are unrecovered");
                bodies_[body_index].position=desired;commit_collision_positions(body_index);
                wander.finish_move(before,animation.values().at(1));
            }
            if(observe_wander)observe_wander(alias,true,wander,animation);
            continue;
        }
        if(!enemies_[index])continue;
        auto* object=actor(definition_->npcs[index].alias);auto* hero=actor(-1);
        const auto& channel=object->animation().values();const auto& player=hero->animation().values();
        const int manager_mode=battle_request_ ? (battle_request_->phase?int(battle_request_->phase):
            (exploration_handoff_?0:5)) : (event_running()?5:(transition_?14:0));
        EnemyObservation observation{channel.at(1),channel.at(0),channel.at(3),player.at(1),player.at(0),rotation,
            get_variable(0x40006),player_.motion,manager_mode,get_variable(0x40004)};
        const auto action=enemies_[index]->tick(observation,random_value,angles_,sine_);
        if(action.visible) {
            set_instance_visibility(object->visible,actor_collision_indices_.at(index),bodies_,*action.visible);
        }
        if(action.translucent)object->render_flags|=0x100000;
        if(action.position)object->animation().set_channel(1,*action.position);
        if(action.rotation)object->animation().set_channel(0,*action.rotation);
        if(action.color)object->animation().set_channel(3,*action.color);
        if(action.alpha)object->animation().set_component(3,3,*action.alpha);
        if(action.displacement) {
            if(!collision_)throw std::runtime_error("Enemy pursuit requires field collision");
            sync_collision_positions();const auto body_index=actor_collision_indices_.at(index);
            auto desired=bodies_[body_index].position;for(unsigned k=0;k<4;++k)desired[k]+=(*action.displacement)[k];
            MapCollisionSettings settings;settings.wall_iterations=3;settings.disabled_mask=collision_mask->bits;
            resolve_collision(*collision_,bodies_,body_index,desired,settings,std::bit_cast<float>(0x3efff17du));
            bodies_[body_index].position=desired;
            commit_collision_positions(body_index);
        }
        rear_attack_|=action.rear_attack;
        if(action.encounter_column) {
            if(!resolve_encounter_formation)
                throw std::runtime_error("Natural encounter requires owned bunpu table service");
            const auto formation=resolve_encounter_formation(definition_->party,*action.encounter_column);
            encounter_=FieldEncounter{*action.encounter_column,action.field_mode,rear_attack_,definition_->party,formation};
            return;
        }
    }
}
void OpeningField::clear_contact() {
    player_.floor_contact=0;player_.trigger_kind=player_.trigger_index=0;
    if(player_.contact_bubble) {
        const auto handle=player_.contact_bubble&0xfff;
        if(handle>=effects_.size())throw std::runtime_error("Invalid contact effect handle");
        if(effects_[handle])effects_[handle]->animation.request_removal();player_.contact_bubble=0;
    }
}
void OpeningField::restore_event_gate() {
    if(saved_input_gate_==0&&get_variable(0x40005)!=0)saved_input_gate_=1;
    set_variable(6,4,saved_input_gate_);input_gate_saved_=false;
}
void OpeningField::interact(ExplorationInput input) {
    if(!definition_||(!(input.pressed&0x40)&&player_.trigger_kind!=0x81))return;
    if(!get_variable||!set_variable)throw std::runtime_error("Field interaction requires native variables");
    if(player_.trigger_kind>=0x80) {
        interaction_alias_=0;interaction_event_=int(player_.trigger_index);player_.trigger_kind=0;
        saved_input_gate_=get_variable(0x40006);set_variable(6,4,1);input_gate_saved_=true;
        script_.start(unsigned(interaction_event_));
        if(event_running()) {
            // 0x1326e8 disables follow via0x135c78(-1) only when the
            // newly started floor event remains active. Otherwise the next
            // manager update would cancel its scripted camera interpolation.
            exterior_camera_enabled_=false;
            exploration_handoff_=false;player_.mask|=1;clear_contact();
        }
        else restore_event_gate();
        return;
    }
    Rounding rounding;const auto position=actor(-1)->animation().values().at(1);
    float distance=10;FieldNpcDefinition* selected=nullptr;
    for(auto& npc:definition_->npcs) {
        auto* object=actor(npc.alias);if(npc.state>=4||!object->visible)continue;
        const auto target=object->animation().values().at(1);
        const float y=position[1]-target[1];if(y>4||y< -4)continue;
        const float x=position[0]-target[0],z=position[2]-target[2];
        const float candidate=std::sqrt(x*x+z*z);
        if(candidate<distance){distance=candidate;selected=&npc;}
    }
    if(!selected)return;
    if(selected->talk>=0)throw std::runtime_error("Direct NPC message lifecycle is unrecovered");
    if(selected->talk==INT32_MIN)throw std::runtime_error("Invalid NPC event entry");
    interaction_alias_=selected->alias;interaction_event_=-selected->talk;
    auto* hero=actor(-1);hero->select_motion(1);player_.motion=1;player_.effect_counter=0;
    auto* npc=actor(selected->alias);alias7_actor_=npc;
    if(set_instance_alias)set_instance_alias(-7,npc);
    npc->select_motion(3);selected->state=3;
    const auto selected_index=std::size_t(selected-definition_->npcs.data());
    if(wanderers_[selected_index])wanderers_[selected_index]->counter=-1;
    if(get_variable(0x40005)==0) {
        const auto p=npc->animation().values().at(1);
        npc->animation().set_channel(0,{0,camera_angle(-(p[0]-position[0]),-(p[2]-position[2]),angles_)+.5f,0,0});
    }
    player_.mask|=1;clear_contact();
    saved_input_gate_=get_variable(0x40006);set_variable(6,4,1);input_gate_saved_=true;
    exploration_handoff_=false;
    // 0x132b00 disables the exploration camera via0x135c78(-1) before
    // starting a negative-ID event. Its scripted camera must remain in control.
    exterior_camera_enabled_=false;
    // 0x1bb7e0 -> 0x1b5dd0 executes the newly selected event immediately.
    script_.start(unsigned(interaction_event_));
}
void OpeningField::load_effects(Bytes bytes,std::shared_ptr<const EffectTemplatePreparation> prepared) {
    if(!effect_definitions_.empty())throw std::runtime_error("Field effects already loaded");
    effect_definitions_=resolve_effect_references(read_efd(bytes));
    effect_preparation_=std::move(prepared);
}
void OpeningField::bind_effect_geometry(MapEffect& effect) const {
    if(effect.flame_geometry&&effect_preparation_)effect.bind_flame_geometry(effect_preparation_->flame);
    for(const auto& e:effect.elements)if(e.shape==55) {
        if(!effect_preparation_||!effect_preparation_->geometry[55])
            throw std::runtime_error("Field shape55 requires shared template preparation");
        const auto& geometry=effect_preparation_->geometry[55];
        effect.shape55_geometry=std::shared_ptr<const std::vector<MapEffectVertex>>(geometry,&geometry->vertices);
        break;
    }
}
void OpeningField::load_map_effects(Bytes bytes,const EffectTextureSizes& sizes) {
    if(effect_definitions_.empty()||!random_value)throw std::runtime_error("Map effects require templates and shared RNG");
    if(map_effects_loaded_)throw std::runtime_error("Map effects already constructed");
    const auto map=read_xmp(bytes);
    for(const auto& p:map.placements) {
        if(!p.handler())continue;
        if(p.handler()!=1)throw std::runtime_error("Unrecovered map placement handler");
        const auto id=p.handler_argument()&127;
        const auto definition=std::find_if(effect_definitions_.begin(),effect_definitions_.end(),[&](const auto& d){return d.id==id;});
        if(definition==effect_definitions_.end())throw std::runtime_error("Missing map effect definition");
        unsigned handle=0;while(handle<effects_.size()&&(effects_[handle]||map_effects_[handle]))++handle;
        if(handle==effects_.size())throw std::runtime_error("Field effect capacity exhausted");
        const auto t=map_transform(p,map.definitions.at(p.definition()));
        try {
            map_effects_[handle]=std::make_unique<MapEffect>(*definition,t.position,t.rotation,t.scale,sizes,random_value);
            bind_effect_geometry(*map_effects_[handle]);
        } catch(const std::exception& error) {
            throw std::runtime_error("Map effect "+std::to_string(id)+": "+error.what());
        }
    }
    effect_texture_sizes_=sizes;map_effects_loaded_=true;
}
void OpeningField::spawn_dust(const Float4& position) {
    if(player_.dust_effect!=17)
        throw std::runtime_error("Unrecovered player dust resources/template");
    spawn_positioned_effect(17,position);
}
unsigned OpeningField::spawn_positioned_effect(unsigned template_id,const Float4& position) {
    if(!map_effects_loaded_||!random_value)
        throw std::runtime_error("Positioned effect requires owned templates, textures and shared RNG");
    const auto definition=std::find_if(effect_definitions_.begin(),effect_definitions_.end(),
        [&](const auto& d){return d.id==template_id;});
    if(definition==effect_definitions_.end())throw std::runtime_error("Missing owned positioned effect template");
    unsigned handle=0;while(handle<effects_.size()&&(effects_[handle]||map_effects_[handle]))++handle;
    if(handle==effects_.size())throw std::runtime_error("Field effect capacity exhausted");
    //0x1012f8 stores the current position and starts phase1, without attachment
    // or a random starting phase. Particle RNG runs on its first effect update.
    map_effects_[handle]=std::make_unique<MapEffect>(*definition,position,Float4{},Float4{1,1,1,1},
        effect_texture_sizes_,random_value,EffectStart::FirstFrame);
    bind_effect_geometry(*map_effects_[handle]);
    return handle;
}
unsigned OpeningField::attach_map_effect(unsigned template_id,int alias) {
    if(!map_effects_loaded_||!random_value||!actor(alias))
        throw std::runtime_error("Attached map effect requires owned resources and an actor");
    const auto definition=std::find_if(effect_definitions_.begin(),effect_definitions_.end(),
        [&](const auto& d){return d.id==template_id;});
    if(definition==effect_definitions_.end())throw std::runtime_error("Missing attached map effect template");
    unsigned handle=0;while(handle<effects_.size()&&(effects_[handle]||map_effects_[handle]))++handle;
    if(handle==effects_.size())throw std::runtime_error("Field effect capacity exhausted");
    //0x1015e8/0x101558 retain constructor transforms until callback0x101118.
    auto effect=std::make_unique<MapEffect>(*definition,definition->position,definition->rotation,
        definition->scale,effect_texture_sizes_,random_value,EffectStart::FirstFrame);
    bind_effect_geometry(*effect);effect->actor_alias=alias;
    map_effects_[handle]=std::move(effect);
    return handle;
}
unsigned OpeningField::attach_bubble(int parameter,int alias) {
    const auto definition=std::find_if(effect_definitions_.begin(),effect_definitions_.end(),[](const auto& d){return d.id==30;});
    if(definition==effect_definitions_.end())throw std::runtime_error("Bubble requires owned TES.EFD and effect textures");
    if(!actor(alias))throw std::runtime_error("Unresolved effect attachment actor");
    unsigned handle=0;while(handle<effects_.size()&&(effects_[handle]||map_effects_[handle]))++handle;
    if(handle==effects_.size())throw std::runtime_error("Field effect capacity exhausted");
    effects_[handle]=std::make_unique<AttachedEffect>(*definition,parameter,alias);return handle;
}
void OpeningField::load_exploration_camera(Bytes stg) {
    if(!actor(-1)||exploration_camera_)throw std::runtime_error("Exploration camera requires a player and one configuration");
    exploration_camera_=field_anchored_camera(stg);
}
void OpeningField::configure_exterior_camera(unsigned area) {
    if(!exterior_camera)throw std::runtime_error("Exterior camera requires session state");
    configured_camera_area_=area;exterior_camera_enabled_=true;
    if(area==0){exterior_camera->select_preset(0);return;}
    const auto setting=std::find_if(camera_settings_.begin(),camera_settings_.end(),[&](const auto& s){return s.id==int(area)-1;});
    // 0x138a50 provides these defaults when a settings ID is absent.
    const auto value=setting==camera_settings_.end()?FieldCameraSetting{0,
        std::bit_cast<float>(0xc1a01449u),std::bit_cast<float>(0x3f7c2917u),std::bit_cast<float>(0xc11028d4u),0}:*setting;
    if(value.mode!=0)throw std::runtime_error("Alternate exterior camera mode is unrecovered");
    exterior_camera->configure(value.height,value.scale,value.target);
}
void OpeningField::load_exploration_collision(Bytes xcl,Bytes variables) {
    if(!actor(-1)||collision_||actors_.size()!=3+unsigned(shadow_.has_value()) || actors_[0].alias!=-8 ||
       actors_[1].alias!=-9 || actors_.back().alias!=-1)
        throw std::runtime_error("Exploration currently requires HOME1C MAM/IMO/HERO construction order");
    std::array<std::uint8_t,0x800> setup{};setup[0x42c]=33;
    run_event_setup(bytes_,variables,setup);
    player_.footstep_sound=get(setup,0x42c);player_.dust_effect=get(setup,0x414);
    build_collision(xcl);
}
void OpeningField::build_collision(Bytes xcl) {
    collision_=read_xcl(xcl);
    Rounding rounding;
    if(map_scene_)for(const auto& binding:map_scene_->collision_bodies())bodies_.push_back(binding.body);
    map_body_count_=bodies_.size();actor_collision_indices_.resize(actors_.size());
    for(unsigned i=0;i<actors_.size();++i) {
        if(!actors_[i].collidable)continue;
        actor_collision_indices_[i]=unsigned(bodies_.size());
        CollisionBody body;body.id=unsigned(bodies_.size());body.priority=10;body.floor_reach=4;
        const float radius=std::bit_cast<float>(0x3ff33333u);body.broad_radius=radius+radius;
        body.shapes={{8,{{0,0,0,1},{},0}},{0x72,{{0,-2.5f,0,1},{0,-5,0,1},radius}}};
        body.position=body.previous=actors_[i].actor->animation().values().at(1);
        if(body.id&&!actors_[i].actor->visible)body.flags|=2;
        bodies_.push_back(std::move(body));
    }
}
void OpeningField::prepare(FieldDefinition definition,Bytes stg,Bytes xcl) {
    Rounding rounding;
    if(definition_||actors_.empty()||actors_.back().alias!=-1||definition.type>1)
        throw std::runtime_error("Unrecovered field entry construction");
    if(prepared_||activated_)throw std::runtime_error("Field staging lifecycle");
    if(!definition.followers.empty())throw std::runtime_error("Field follower construction is unrecovered");
    if(definition.type==0&&!exterior_camera)throw std::runtime_error("Exterior field requires session camera state");
    if(definition.npcs.size()+1+unsigned(shadow_.has_value())!=actors_.size())throw std::runtime_error("Field entry NPC bindings differ");
    if(exterior_camera)exterior_camera->snap_target=true;
    staged_stg_.assign(stg.begin(),stg.end());staged_xcl_.assign(xcl.begin(),xcl.end());
    for(const auto& npc:definition.npcs) {
        auto* object=actor(npc.alias);
        if(!object||(npc.state!=0&&npc.state!=1&&npc.state!=5)||(npc.flags&~15u))
            throw std::runtime_error("Unrecovered field NPC construction: alias="+std::to_string(npc.alias)+
                " model="+npc.model+" state="+std::to_string(npc.state)+" flags="+std::to_string(npc.flags)+
                " bound="+std::to_string(object!=nullptr));
        object->animation().set_channel(0,{0,npc.facing,0,1});object->animation().set_channel(1,npc.position);
        object->visible=(npc.flags&2)==0;
        // 0x131d28 -> 0x1baae8 writes a shadow selector, not collision or
        // visibility flags. The second call takes precedence when both are set.
        if(npc.flags&4)object->shadow_mode_override=2;
        if(npc.flags&8)object->shadow_mode_override=6;
    }
    definition_=std::move(definition);player_.footstep_sound=definition_->footstep;player_.dust_effect=definition_->dust;
    initialize_npcs();
    prepared_=true;
}
void OpeningField::activate_pose(const Float4& player_position,const Float4& player_rotation,
                                 const std::optional<Float4>& raw_camera_position) {
    if(!prepared_||activated_||!definition_)throw std::runtime_error("Field activation lifecycle");
    Rounding rounding;
    auto* hero=actor(-1);hero->animation().set_channel(0,player_rotation);hero->animation().set_channel(1,player_position);
    build_collision(staged_xcl_);
    // Both 0x132720 branches pass through 0x135300. The return branch starts
    // from the saved pose supplied by 0x130220, then performs the same +1Y
    // collision settle as ordinary entrance.
    auto desired=player_position;desired[1]+=1;
    MapCollisionSettings settings;settings.wall_iterations=3;settings.disabled_mask=collision_mask->bits;
    resolve_collision(*collision_,bodies_,bodies_.size()-1,desired,settings,std::bit_cast<float>(0x3efff17du));
    auto& body=bodies_.back();
    body.position=body.previous=desired;hero->animation().set_channel(1,desired);
    player_.history.fill(desired);
    player_.floor_contact=(body.contact.flags&1)?body.contact.materials[0]:-1;
    player_.camera_area=(std::uint32_t(player_.floor_contact)&0x38)>>3;
    // 0x135458 enables the initial automatic-contact latch; 0x135300 records
    // its index without arming it, so spawning inside one does not retrigger it.
    if((std::uint32_t(player_.floor_contact)&0x83)==0x81)
        player_.trigger_index=std::uint32_t(player_.floor_contact)>>8;
    player_.mask=0;player_.motion=1;exploration_handoff_=true;
    if(definition_->type==0) {
        camera_settings_=field_camera_settings(staged_stg_);
        script_.set_channel(0,exterior_camera->begin(desired));
        configure_exterior_camera(player_.camera_area);
        exterior_camera_enabled_=true;
    } else {
        load_exploration_camera(staged_stg_);
        const auto camera=anchored_camera(desired,*exploration_camera_,angles_,sine_);
        script_.set_channel(0,camera.position);requested_target_=camera.target;camera_mode_=2;
    }
    if(raw_camera_position) {
        // 0x130248 restores the raw camera channel, independently of the
        // rendered position after CameraBlend.
        script_.set_channel(0,*raw_camera_position);
    }
    if(!get_variable||!set_variable)throw std::runtime_error("Field entry requires native variables");
    if(activation_ready)activation_ready();
    if(definition_->startup) {
        saved_input_gate_=get_variable(0x40006);set_variable(6,4,1);input_gate_saved_=true;
        // Entry0x132720 calls0x1326e8: execute immediately, then freeze follow
        // only if the startup is still active. Later opcode53 may enable it.
        script_.start(definition_->startup);
        if(event_running()){
            exterior_camera_enabled_=false;
            if(exterior_camera)exterior_camera->manual_rotate=false;
            exploration_handoff_=false;player_.mask|=1;
        }
        else restore_event_gate();
    }
    activated_=true;
}
void OpeningField::activate(unsigned entrance) {
    if(!prepared_||!definition_)throw std::runtime_error("Field activation before preparation");
    const auto placement=field_entrance(staged_stg_,entrance);
    activate_pose(placement.position,placement.rotation,std::nullopt);
}
void OpeningField::activate_return(const FieldBattleReturnSnapshot& saved,ExplorationInput input) {
    if(!prepared_||!definition_||saved.scene!=definition_->scene)
        throw std::runtime_error("Field return activation scene mismatch");
    if(!std::all_of(saved.player_position.begin(),saved.player_position.end(),[](float v){return std::isfinite(v);})||
       !std::all_of(saved.player_rotation.begin(),saved.player_rotation.end(),[](float v){return std::isfinite(v);})||
       !std::all_of(saved.raw_camera_position.begin(),saved.raw_camera_position.end(),[](float v){return std::isfinite(v);}))
        throw std::runtime_error("Field return pose contains non-finite values");
    activate_pose(saved.player_position,saved.player_rotation,saved.raw_camera_position);
    tick_callbacks(input,false);
}
void OpeningField::activate_entry(unsigned entrance,ExplorationInput input) {
    activate(entrance);
    tick_callbacks(input,false);
}
void OpeningField::enter(FieldDefinition definition,Bytes stg,Bytes xcl,unsigned entrance,ExplorationInput input) {
    prepare(std::move(definition),stg,xcl);
    activate_entry(entrance,input);
}
void OpeningField::remember_player() {
    const auto position=actor(-1)->animation().values().at(1);
    const auto& old=player_.history[player_.history_cursor];
    if(old[0]!=position[0]||old[2]!=position[2]) {
        player_.history_cursor=(player_.history_cursor+1)&31;
        player_.history[player_.history_cursor]=position;
    }
}
void OpeningField::move_player(ExplorationInput input) {
    if(!collision_ || (player_.mask&1))return;
    Rounding rounding;auto* hero=actor(-1);auto& state=player_;
    int x=input.x,z=input.z;
    if(input.held&0x8000)x=0;
    if(input.held&0x2000)x=255;
    if(input.held&0x1000)z=0;
    if(input.held&0x4000)z=255;
    x-=128;z-=128;
    const float direction=(1.f-rotation[1])+camera_angle(float(x),float(z),angles_);
    const float magnitude=std::sqrt(float(x)*float(x)+float(z)*float(z));
    Float4 delta{0,0,0,1};std::int32_t motion=1;
    const bool moving=magnitude>64;
    if(moving) {
        const float speed=magnitude>=120?.55f:.19f;motion=magnitude>=120?3:2;
        const auto index=(std::uint32_t(std::int32_t(direction*32768.f))>>1)&0x3fff;
        delta[0]=speed*sine_[index];delta[2]=(-speed)*sine_[(index+4096)&0x3fff];
        auto facing=hero->animation().values().at(0);facing[1]=1.f-direction;
        hero->animation().set_channel(0,facing); // 0x1b4220 also clears prior rates.
    }
    if(state.floor_contact<0)delta[1]=1;
    if(moving||state.floor_contact<0) {
        sync_collision_positions();
        auto& body=bodies_.back();body.previous=body.position;Float4 desired;
        for(unsigned i=0;i<4;++i)desired[i]=body.position[i]+delta[i];
        MapCollisionSettings settings;settings.wall_iterations=3;settings.disabled_mask=collision_mask->bits;
        resolve_collision(*collision_,bodies_,bodies_.size()-1,desired,settings,std::bit_cast<float>(0x3efff17du));
        body.position=desired;
        // 0x1ba948 commits the mover and copies every current position to history.
        // Pushed NPC positions are native animation channels, not detached copies.
        commit_collision_positions(bodies_.size()-1);
        state.floor_contact=(body.contact.flags&1)?body.contact.materials[0]:-1;
    }
    if(state.floor_contact>=0) {
        const auto contact=std::uint32_t(state.floor_contact),kind=contact&0x83;
        state.upper_material=contact>>14;state.camera_area=(contact&0x38)>>3;
        if(!kind) {if(state.trigger_index){state.trigger_kind=0;state.trigger_index=0;}}
        else if(state.trigger_index!=(contact>>8)){state.trigger_kind=kind;state.trigger_index=(contact>>8)&0x3f;}
        if(!kind||kind==0x81) {
            if(state.contact_bubble) {
                const auto handle=state.contact_bubble&0xfff;
                if(handle>=effects_.size())throw std::runtime_error("Invalid contact effect handle");
                if(effects_[handle])effects_[handle]->animation.request_removal();
                state.contact_bubble=0;
            }
        } else if(!state.contact_bubble) {
            // 0x101558(30,HERO,2) attaches at the ordinary head offset.
            // 0x1016a8(handle,0) sets the symbol parameter float4 at +0x50;
            // 0x105a40's texture case 0x19 consumes W to choose the atlas/UV.
            state.contact_bubble=attach_bubble(0,-1)|0x1000;
        }
        if(state.camera_area&&(!definition_||definition_->type!=0))
            throw std::runtime_error("Field contact camera area is unrecovered");
    }
    if(motion==3) {
        if(std::bit_cast<std::int32_t>(state.run_delay)<24){if(state.motion>=0)motion=state.motion;++state.run_delay;}
        else state.run_delay=0;
    } else ++state.run_delay;
    if(motion==state.motion) {
        const auto counter=std::bit_cast<std::int32_t>(state.effect_counter);
        if((motion==2&&counter<0)||(motion==3)) {
            if(state.dust_effect && ((motion==2&&!(state.effect_counter&3)) ||
               (motion==3&&counter<40&&!(state.effect_counter&7))))
                spawn_dust(hero->animation().values().at(1));
            ++state.effect_counter;
        }
    } else {
        hero->select_motion(unsigned(motion));state.motion=motion;state.effect_counter=0;
        state.footstep_period=motion==3?15:0;
        if(motion==3)state.footstep_clock=std::uint32_t(-10);
    }
    if(state.footstep_period && ++state.footstep_clock==state.footstep_period) {
        state.footstep_clock=0;
        // Retail 0x12df80 table entries 33..35: speed 1, gains 45/60/60.
        // Other bank-0 cues need their own recovered table/driver contract.
        if(state.footstep_sound<33||state.footstep_sound>35)
            throw std::runtime_error("Unrecovered footstep sound parameters");
        const unsigned gain=state.footstep_sound==33?45:60;
        if(!sound_cue||!sound_cue(0,state.footstep_sound,1,gain))
            throw std::runtime_error("Footsteps require the owned ALL sound bank");
    }
}
Animation::ServiceResult OpeningField::service(unsigned op,unsigned a,std::int32_t b,std::uint32_t pc) {
    auto pending=[&] {pending_opcode=op;pending_actor=b;pending_animation=a;return Animation::ServiceResult{pc,true};};
    switch(op) {
        case 0x59: {
            if(b!=0x111||!set_save_caption)
                throw std::runtime_error("Unrecovered live field string service");
            const std::uint64_t base=32ull+word(4)+word(8),at=base+word(pc+4);
            if(at>=bytes_.size())throw std::runtime_error("Field save caption string bounds");
            const auto first=bytes_.begin()+at,last=std::find(first,bytes_.end(),0);
            if(last==bytes_.end())throw std::runtime_error("Unterminated field save caption");
            set_save_caption(std::string_view(reinterpret_cast<const char*>(&*first),last-first));
            pending_opcode=0;return {pc+8};
        }
        case 0x62:
            if(b==14) {
                if(!request_chapter||!definition_)
                    throw std::runtime_error("Chapter request needs field mode owner");
                if(rest_requested_||save_requested_||save_menu_active_||battle_request_||transition_||encounter_)
                    throw std::runtime_error("Overlapping chapter field request");
                request_chapter(a,word(pc+4));
                chapter_selector_=a;
                pending_opcode=0;return {pc+8};
            }
            if(b==10) {
                if(!recover_party||!definition_)throw std::runtime_error("Rest request needs field party service");
                if(rest_requested_||save_requested_||save_menu_active_||battle_request_||transition_||encounter_)
                    throw std::runtime_error("Overlapping Rest field request");
                recover_party();rest_requested_=true;
                pending_opcode=0;return {pc+8};
            }
            if(b==12) {
                if(!open_save_menu||!definition_)throw std::runtime_error("Save request needs field manager service");
                if(save_requested_||save_menu_active_||battle_request_||transition_||encounter_)
                    throw std::runtime_error("Overlapping Save field request");
                save_requested_=true;pending_opcode=0;return {pc+8};
            }
            // 0x131338 ->0x1248d8. Only the reached scripted mode7 is
            // implemented; its payload is a stage index, not another opcode.
            if(b!=7||!battle_transition_service||!definition_)
                throw std::runtime_error("Unrecovered field mode request "+std::to_string(b));
            if(battle_request_||transition_||encounter_)
                throw std::runtime_error("Overlapping field battle request");
            battle_request_=FieldBattleRequest{a,std::bit_cast<std::int32_t>(word(pc+4)),
                battle_return_value_|0x1000u}; // 0x131390 reads field+28, not scene.
            pending_opcode=0;return {pc+8};
        case 11: {
            if(!get_variable)return pending();
            const auto value=get_variable(word(pc+4)),expected=std::bit_cast<std::int32_t>(word(pc+8));
            bool take=true;
            switch(a){case 1:take=value==expected;break;case 2:take=value!=expected;break;
                case 3:take=value>expected;break;case 4:take=value<expected;break;
                case 5:take=value>=expected;break;case 6:take=value<=expected;break;}
            const auto next=take?std::int64_t(pc)+12:std::int64_t(pc)+(b>>2)*4;
            if(next<32||std::uint64_t(next)+4>bytes_.size())throw std::runtime_error("Field condition branch bounds");
            pending_opcode=0;return {std::uint32_t(next)};
        }
        case 0x43:
            if(transition_)throw std::runtime_error("Overlapping field transition request");
            transition_=FieldTransition{b,a};return {pc+4};
        case 0x53:
            // Original 0x1307e0 only changes the camera for exterior class 0.
            if(!definition_)return pending();
            if(definition_->type==0) {
                if(!exterior_camera)return pending();
                exterior_camera_enabled_=b>=0;
                if(b>=0){exterior_camera->select_preset(unsigned(b));exterior_camera->phase=0;}
            }
            return {pc+4};
        case 0x54: {
            auto* hero=actor(-1);
            if(!hero)return pending();
            const auto request=std::uint32_t(b);
            //0x130850 handles the direct selector independently of transition state.
            if(request&0x100) {
                hero->shadow_mode_override=(request&255)<<2;
                pending_opcode=0;return {pc+4};
            }
            if(!request_player_shadow(request))return pending();
            pending_opcode=0;return {pc+4};
        }
        case 7:
            if(b==-2)script_.set_channel_target(nullptr);
            else if(auto* target=actor(b))script_.set_channel_target(&target->animation());
            else {
                const auto instance=b>=0?std::optional<int>(b):
                    (resolve_instance_alias?resolve_instance_alias(b):std::nullopt);
                auto* channels=instance&&map_scene_?map_scene_->channel_group(*instance):nullptr;
                if(!channels)return pending();
                script_.set_channel_target(channels);
            }
            group_=b;return {pc+4};
        case 9:
            // 0x1b5830 -> 0x1bb608 waits for the selected object's motion
            // context to clear running bit0x100, not for a guessed duration.
            if(auto* object=actor(b);object&&!(object->animation().script_state()&0x100)) {
                pending_opcode=0;return {pc+4};
            }
            return pending();
        case 0x4b:
            if(b<0) {
                // 0x130670 resolves the alias once; 0x1b9720 stores that
                // instance and selects mode 3, without restarting the blend.
                camera_actor_=actor(b);
                if(!camera_actor_)throw std::runtime_error("Field camera target actor is not bound");
                camera_mode_=3;return {pc+4};
            }
            if(b==0) {
                // 0x130670 resets the automatic rate, begins a .01 blend and
                // selects mode 0. Rotation retains the last evaluated channel.
                blend.begin(0.01f,script_.values().at(0),requested_target_);
                camera_mode_=0;return {pc+4};
            }
            if(b!=1)throw std::runtime_error("Field camera target mode not yet implemented");
            requested_target_=point(pc+4);camera_mode_=2;return {pc+16};
        case 0x52: {
            Rounding rounding;const float rate=b ? 1.f/float(b) : 0.f;
            blend.begin(rate,script_.values().at(0),requested_target_);return {pc+4};
        }
        case 0x4f:
            if(!scene_fade)return pending();
            scene_fade(a,b);pending_opcode=0;return {pc+4};
        case 0x4c:
            if(a || b)throw std::runtime_error("Field dialogue mode not yet implemented");
            start_dialogue_(std::bit_cast<std::int32_t>(word(pc+4)));return {pc+8};
        case 0x4d:
            //0x1308c0 only writes the manager sentinel. Construction happens
            // on the following mode5 manager callback, not in this script pass.
            if(hat_requested_||hat_actor_)throw std::runtime_error("Repeated hat scene construction is unrecovered");
            hat_requested_=true;return {pc+4};
        case 10:return wait_dialogue_(b) ? Animation::ServiceResult{pc+4} : Animation::ServiceResult{pc,true};
        case 0x60:
            // 0x1305a8 -> 0x11fd78 changes body-layer flag0x10 only.
            // Exactly B=1 enables confirmation; every other value clears it.
            if(!dialogue_input)return pending();
            dialogue_input(b==1);pending_opcode=0;return {pc+4};
        case 0x36:
            if(b==-2) {
                //0x1b7a68 ->0x1baf50: camera resource absence is a real no-op.
                if(camera_motion_) {
                    if(a<0x100){camera_motion_->select(a);camera_mode_=1;}
                    else camera_mode_=0;
                }
                pending_opcode=0;return {pc+4};
            }
            if(a<0x800&&request_actor_motion(b,a)) {
                pending_opcode=0;return {pc+4};
            }
            return pending();
        case 0x3c:
            // 0x1b7bb8 -> 0x1bb890: mode bits accumulate until mode0 clears
            // both; other selectors preserve flags. Positive instance indices
            // are not the map-object binding namespace and remain pending.
            if(b<0)if(auto* target=actor(b)) {
                if(a==0)target->render_flags&=~0x180000u;
                else if(a==1)target->render_flags|=0x80000;
                else if(a==2)target->render_flags|=0x100000;
                pending_opcode=0;return {pc+4};
            }
            return pending();
        case 0x3d:
            screen_filter.select(a,b);return {pc+4};
        case 0x39: {
            //0x130be0 attaches A in mode0. Negative B uses the actor-alias
            //namespace; nonnegative B is an instance index. The shared actor
            //resolver preserves those namespaces for the attachment callback.
            const auto handle=attach_map_effect(a,b);
            effect_ring_[effect_ring_index_]=handle;effect_ring_index_=(effect_ring_index_+1)&15;
            pending_opcode=0;return {pc+4};
        }
        case 0x3e: {
            //0x130c50 ignores A, consumes XYZ, and records the allocated handle
            // in the same ring used by attachment/removal/parameter services.
            const auto handle=spawn_positioned_effect(unsigned(b),point(pc+4));
            effect_ring_[effect_ring_index_]=handle;effect_ring_index_=(effect_ring_index_+1)&15;
            return {pc+16};
        }
        case 0x3b:
            if((a&0x100)&&b<0&&resolve_instance_alias&&map_scene_&&map_visibility) {
                const auto instance=resolve_instance_alias(b);
                if(instance&&map_scene_->collision_index(*instance)) {
                    if(!map_visibility(*instance,(a&255)!=0))return pending();
                    pending_opcode=0;return {pc+4};
                }
            }
            if((a&0x100)&&b>=0&&map_visibility) {
                if(!map_visibility(b,(a&255)!=0))return pending();
                pending_opcode=0;return {pc+4};
            }
            if(auto* target=actor(b);target && (a&0x100)) {
                // Original 0x1baa60 excludes collision index zero.
                unsigned index=0;
                for(unsigned i=0;i<actors_.size();++i)if(actors_[i].actor==target&&!bodies_.empty())index=actor_collision_indices_.at(i);
                set_instance_visibility(target->visible,index,bodies_,(a&255)!=0);
                pending_opcode=0;return {pc+4};
            }
            return pending();
        case 0x44: {
            // 0x130a30: b=0 selects this field's default, a>0 starts at zero
            // and requests the fixed rate8 fade. Selection need not restart.
            if(!b&&!definition_)return pending();
            const int id=b?b:definition_->music;
            if(music_service&&music_service(id,a>0)){pending_opcode=0;return {pc+4};}
            return pending();
        }
        case 0x5f:
            if(music_channels&&music_channels(b,a,word(pc+4))){pending_opcode=0;return {pc+8};}
            return pending();
        case 0x45: {
            FieldAudioRequest request{a,b,{}};
            if(a<1||a>5) {
                request.operation=0;
                if(b<=0) {
                    if(!definition_)return pending();
                    if(definition_->music>0)request.primary_music=definition_->music;
                    request.argument=definition_->secondary_music;
                }
                //No primary selection and no secondary load is a source no-op.
                if(!request.primary_music&&request.argument==0) {
                    pending_opcode=0;return {pc+4};
                }
            }
            if(audio_service && audio_service(request)){pending_opcode=0;return {pc+4};}
            return pending();
        }
        case 0x68:
            if(sound_cue && sound_cue(a,b,std::bit_cast<float>(word(pc+4)),word(pc+8))){pending_opcode=0;return {pc+12};}
            return pending();
        case 0x67:
            // 0x130ab8: every negative B calls12de98 ->1756c0(0),
            // retiring all registered stream voices, independent of A.
            // Music sequences have a separate owner/stop service.
            if(b<0&&stop_sound_cues&&stop_sound_cues()){pending_opcode=0;return {pc+4};}
            if(b>=0&&sound_cue&&sound_cue(a,b,1,128)){pending_opcode=0;return {pc+4};}
            return pending();
        case 0x48: {
            //0x130308 ->0x1259d0 adds B to byte/short/word storage, ignores A,
            // and leaves bit banks unchanged. No saved encounter-gate write.
            const auto selector=word(pc+4),index=selector&0xffff,bank=selector>>16;
            if((bank>4 && (bank<8 || bank>15)) || (bank>=8 && index>=0x28))
                throw std::runtime_error("Unrecovered field variable addition storage");
            if(bank>1) {
                if(!get_variable||!set_variable)return pending();
                const auto sum=std::bit_cast<std::uint32_t>(get_variable(selector))+std::uint32_t(b);
                set_variable(index,bank,std::bit_cast<std::int32_t>(sum));
            }
            pending_opcode=0;return {pc+8};
        }
        case 0x51: {
            if(b!=4)throw std::runtime_error("Unrecovered field variable operation "+std::to_string(b));
            if(!get_variable||!set_variable)return pending();
            const auto first=word(pc+4),second=word(pc+8);
            std::int32_t maximum=0,index=-1;
            for(unsigned i=0;i<8;++i) {
                const auto value=get_variable(0x40010+i);
                if(value>=maximum){maximum=value;index=i;}
            }
            set_variable(first&0xffff,first>>16,maximum);
            set_variable(second&0xffff,second>>16,index);
            pending_opcode=0;return {pc+12};
        }
        case 0x47: {
            const auto selector=word(pc+4),index=selector&0xffff,bank=selector>>16;
            if((bank>4 && (bank<8 || bank>15)) || (bank>=8 && index>=0x28))
                throw std::runtime_error("Unrecovered field variable storage");
            if(!set_variable)return pending();
            set_variable(index,bank,b);
            // 0x130270 also updates field+0xf4 for explicit encounter-gate
            // writes. Completion must restore this new value, not the old one.
            if(bank==4&&index==6)saved_input_gate_=b;
            pending_opcode=0;return {pc+8};
        }
        case 0x5e: {
            const auto handle=attach_bubble(int(a),b);
            effect_ring_[effect_ring_index_]=handle;effect_ring_index_=(effect_ring_index_+1)&15;
            return {pc+4};
        }
        case 0x5c: {
            if(a<0x100)throw std::runtime_error("Effect replacement template is unrecovered");
            const auto handle=(b&0x100)?unsigned(b&255):effect_ring_[(effect_ring_index_-unsigned(b+1))&15];
            if(handle>=effects_.size())throw std::runtime_error("Invalid effect removal handle");
            if(effects_[handle])effects_[handle]->animation.request_removal();
            if(map_effects_[handle])map_effects_[handle]->request_removal();
            return {pc+4};
        }
        case 0x5d: {
            const auto handle=(b&0x100)?unsigned(b&255):effect_ring_[(effect_ring_index_-unsigned(b+1))&15];
            if(handle>=effects_.size())throw std::runtime_error("Invalid effect parameter handle");
            if(effects_[handle])throw std::runtime_error("Attached effect parameter service is unrecovered");
            if(!map_effects_loaded_)throw std::runtime_error("Effect parameter service requires initialized map jobs");
            const auto payload=a>=0xf00?point(pc+4):Float4{};
            if(map_effects_[handle])map_effects_[handle]->set_parameter(a,payload);
            return {pc+(a>=0xf00?16u:4u)};
        }
        case 0x3f:
            collision_mask->event(a,b);return {pc+4};
        case 0x61:
            // 0x1314f8 copies constructed follower count/identities to bank4.
            // enter() still rejects selected followers until their construction
            // is recovered; an enabled party with no followers reports zeros.
            if(b!=1)throw std::runtime_error("Unrecovered party service mode "+std::to_string(b));
            if(!definition_||!set_variable)return pending();
            if(!definition_->followers.empty())throw std::runtime_error("Follower identity reporting requires constructed followers");
            set_variable(13,4,0);set_variable(14,4,0);set_variable(15,4,0);return {pc+4};
        case 0x5a:
        case 0x5b: {
            auto* moving=actor(group_);auto* facing=op==0x5a ? actor(b) : nullptr;
            if(!moving || (op==0x5a && !facing))return pending();
            Rounding rounding;
            const auto destination=facing ? facing->animation().values().at(1) : point(pc+4);
            const auto position=moving->animation().values().at(1);
            const float angle=camera_angle(-(destination[0]+(-position[0])),-(destination[2]+(-position[2])),angles_);
            const auto duration=op==0x5a ? int(a) : b;
            if(duration==0)moving->animation().set_component(0,1,angle);
            else moving->animation().turn_component(0,1,angle,duration);
            return {pc+(op==0x5a ? 4u : 16u)};
        }
        default:throw std::runtime_error("Unrecovered field service "+std::to_string(op)+
            " at PC "+std::to_string(pc)+" (a="+std::to_string(a)+", b="+std::to_string(b)+")");
    }
}
void OpeningField::evaluate() {
    const bool animated_camera=camera_mode_==1;
    if(animated_camera) {
        if(!camera_motion_)throw std::runtime_error("Camera motion resource is missing");
        const auto values=camera_motion_->values();
        script_.set_channel(0,values.at(0));requested_target_=values.at(1);
        // The final sample still gets look-at/blending on the update that
        // notices the motion has ended (0x1b9830..0x1b98f8).
        if(!(camera_motion_->script_state()&0x100))camera_mode_=0;
    }
    if(camera_mode_==3) {
        // 0x1b9894 reads the actor position before visible actor channels tick.
        // The offset addition at 0x1b98d4 is overwritten by the following
        // lq/sq at 0x1b98dc: the raw actor position is the blend input.
        requested_target_=camera_actor_->animation().values().at(1);
    }
    position=script_.values().at(0);target=requested_target_;blend.apply(position,target);
    if(animated_camera || camera_mode_==2 || camera_mode_==3) {
        rotation=camera_look_at(position,target,angles_,sine_);
        // Original look-at 0x1b9440 writes the result back into channel 1.
        script_.set_channel(1,rotation);
    } else {
        rotation=script_.values().at(1);
        requested_target_=target;
    }
}
void OpeningField::close_save_menu() {
    if(!save_menu_active_)throw std::runtime_error("Closing absent field Save menu");
    save_menu_active_=false;save_resuming_=true;
}
void OpeningField::tick(ExplorationInput input) {
    if(save_menu_active_) {
        tick_callbacks(input,false,true,true);return;
    }
    if(encounter_)throw std::runtime_error("Pending native battle request was not consumed");
    // Field manager 0x133a98 runs before the event pass. Case 5 detects the
    // ended script and selects the room camera; case 0 updates it next frame.
    const bool battle_transitioning=battle_request_&&battle_request_->phase>=11;
    const bool was_transitioning=bool(transition_)||battle_transitioning;
    const bool exterior=definition_&&definition_->type==0;
    auto update_exterior_camera=[&] {
        // 0x135e88/0x1363b8 select cubic mode for later scripted camera moves.
        script_.set_channel_mode(0,0x10);
        const auto& values=actor(-1)->animation().values();
        const auto camera=exterior_camera->tick(values.at(1),values.at(0),script_.values().at(0),input.held,input.look_x,
                                                *collision_,angles_,sine_,collision_mask->bits);
        script_.set_channel(0,camera.position);requested_target_=camera.target;camera_mode_=2;
    };
    if(save_resuming_) {
        save_resuming_=false;
        if(exterior&&exterior_camera_enabled_)update_exterior_camera();
        tick_callbacks(input,!exterior);return;
    }
    bool move=false;
    if(battle_transitioning) {
        if(battle_request_->dispatched)
            throw std::runtime_error("Global manager did not consume native battle request");
        if(!definition_||!definition_->followers.empty())
            throw std::runtime_error("Battle transition requires recovered follower return state");
        battle_transition_service(battle_request_->phase,*battle_request_);
        if(battle_request_->phase==13)battle_request_->dispatched=true;
        else ++battle_request_->phase;
    }
    if(transition_) {
        if(transition_->ready)throw std::runtime_error("Destination loader did not consume field transition");
        if(transition_->countdown) {
            --transition_->countdown;
            //134278/134294 skip both capture and overlay for request14.
            if(chapter_selector_<0) {
                if(!transition_service)throw std::runtime_error("Field transition requires fade services");
                transition_service(transition_->countdown);
            }
        } else transition_->ready=true;
    }
    if(!was_transitioning&&exterior&&!exploration_handoff_&&exterior_camera_enabled_)update_exterior_camera();
    if(hat_requested_) {
        if(exploration_handoff_||!create_hat_scene)
            throw std::runtime_error("Hat scene requires its manager and owned resource loader");
        auto resources=create_hat_scene();
        if(!resources.actor)throw std::runtime_error("Hat scene loader returned no actor");
        if(channel_pool_)resources.actor->animation().bind_channels(channel_pool_);
        auto motion=std::make_unique<Animation>(resources.camera_motion,2);
        if(channel_pool_)motion->bind_channels(channel_pool_);
        motion->start(0); //0x1b5c38 ->0x1b5bd8 initializes the separate context.
        camera_motion_=std::move(motion);camera_mode_=0; //0x1baf50(-1).
        hat_actor_=resources.actor;alias7_actor_=hat_actor_;
        hat_actor_->animation().set_channel(1,{std::bit_cast<float>(0x422a0000u),
            std::bit_cast<float>(0x401eb852u),std::bit_cast<float>(0x443d4000u),1});
        hat_actor_->visible=false;hat_requested_=false;
    }
    if(!was_transitioning&&(exploration_camera_||exterior)&&!event_running()) {
        if(exploration_handoff_) {
            if(rest_requested_) {
                if(!recover_party)throw std::runtime_error("Rest completion needs field party service");
                recover_party();rest_requested_=false;
            }
            if(save_requested_) {
                save_requested_=false;save_menu_active_=true;
                open_save_menu();
            } else if(battle_request_) {
                // 0x133450 consumes queued mode7 before camera/interactions.
                battle_request_->phase=11;
            } else {
                if(exterior&&exterior_camera_enabled_)update_exterior_camera();
                if(exterior&&player_.camera_area!=configured_camera_area_)configure_exterior_camera(player_.camera_area);
                interact(input);
                move=!event_running()&&!transition_;
            }
        } else {
            exploration_handoff_=true;
            // 0x132ca8 resets contact state, clears movement bit 0 and invalidates
            // selected motion; 0x134c18 chooses it on the next player update.
            player_.floor_contact=0;player_.trigger_kind=player_.trigger_index=0;
            player_.mask&=~1u;player_.motion=-1;
            if(exterior){exterior_camera_enabled_=true;exterior_camera->manual_rotate=true;exterior_camera->phase=0;}
            if(input_gate_saved_) {
                // Mode 5 clears field +0x64 before 0x132ca8. The NPC therefore
                // retains state 3 and its event-selected motion/interpolation;
                // only the direct-message mode-4 path requests NPC state 8.
                restore_event_gate();
            }
        }
    }
    tick_callbacks(input,move&&!exterior,save_menu_active_,false);
}
void OpeningField::tick_callbacks(ExplorationInput input,bool interior_camera,bool pause_actors,bool frozen_resource) {
    // Manager0x133a98 updates the anchored camera from the preceding player
    // position. Global scripts and scene evaluation precede player callbacks.
    if(interior_camera) {
        const auto camera=anchored_camera(actor(-1)->animation().values().at(1),*exploration_camera_,angles_,sine_);
        script_.set_channel(0,camera.position);requested_target_=camera.target;camera_mode_=2;
    }
    script_.tick_script();
    for(const auto& binding:map_objects_)binding.actor->tick_script();
    for(const auto& binding:actors_)binding.actor->tick_script();
    if(camera_motion_)camera_motion_->tick_script();
    if(hat_actor_)hat_actor_->tick_script();
    if(!frozen_resource) {
    if(camera_mode_==1)camera_motion_->tick_channels();
    else script_.tick_channels();
    evaluate();
    screen_filter.tick(script_.values().at(2)[0]);
    // Actor groups have flag1 (not global-advance flag2). Original 0x1b9be0
    // calls 0x1b70a0 only for visible instances; hidden motion scripts still run.
    if(map_scene_)map_scene_->tick_static_channels();
    for(const auto& binding:map_objects_)if(binding.actor->visible){binding.actor->tick_channels();binding.actor->evaluate();}
    for(const auto& binding:actors_)if(binding.actor->visible){binding.actor->tick_channels();binding.actor->evaluate();}
    if(hat_actor_&&hat_actor_->visible){hat_actor_->tick_channels();hat_actor_->evaluate();}
    // 0x1b9c74 has evaluated the camera before0x134c18 queries channel1.
    // The player wrapper tests the movement mask independently of manager
    // mode, including the handoff update and transition countdown.
    if(render_projection)render_projection->begin_field();
    advance_effects();
    }
    if(pause_actors)return;
    update_npcs();
    if(encounter_)return;
    move_player(input);
    update_player_shadow();
    if(collision_)remember_player();
    for(const auto& binding:map_objects_)if(binding.actor->visible)binding.actor->evaluate();
    for(const auto& binding:actors_)if(binding.actor->visible)binding.actor->evaluate();
}
bool OpeningField::request_player_shadow(std::uint32_t request) {
    if(!shadow_)return false;
    auto& state=*shadow_;
    const auto mode=request&127;
    if(mode<=1&&!state.locked&&state.phase==(mode?0u:3u)) {
        if(!set_variable||(!(request&0x80)&&!sound_cue))return false;
        if(!mode&&state.voice>=0)
            throw std::runtime_error("Shadow transition owned voice stop is unrecovered");
        if(!mode)state.voice=-1;
        state.phase=mode?1:4;state.factor=1;
        //0x1346e0 discards cue handles; +0x26c is not their owner.
        if(!(request&0x80)&&!sound_cue(0,mode?23:12,1,mode?100:64))
            throw std::runtime_error("Shadow transition sound is unavailable");
        attach_map_effect(24,-1);
        set_variable(2,4,int(mode));
    } else if(mode==2)state.follow_disabled=0;
    else if(mode==3)state.follow_disabled=1;
    else if(mode==4)state.locked=0;
    else if(mode==5)state.locked=1;
    // Other low-seven-bit modes have no action in the original switch.
    return true;
}
void OpeningField::update_player_shadow() {
    //0x1354d8 tests mask bit1 independently of movement bit0, before history.
    if(!shadow_||(player_.mask&2))return;
    auto* stun=actor(-3);auto* hero=actor(-1);
    const auto& channels=stun->animation().values();
    shadow_->stun_rotation=channels.at(0);shadow_->stun_position=channels.at(1);
    shadow_->stun_scale=channels.at(2);shadow_->stun_visible=stun->visible;
    shadow_->hero_shadow=std::uint16_t(hero->shadow_mode_override.value_or(4));
    const auto phase=shadow_->phase;
    shadow_->tick(hero->animation().values().at(0),hero->animation().values().at(1));
    if(!shadow_->follow_disabled) {
        stun->animation().set_channel(0,shadow_->stun_rotation);
        stun->animation().set_channel(1,shadow_->stun_position);
    }
    if(phase==2||phase==4)stun->animation().set_channel(2,shadow_->stun_scale);
    if(phase==2||(phase==4&&shadow_->phase==5))stun->visible=shadow_->stun_visible;
    if((phase==1&&shadow_->phase==2)||phase==5)hero->shadow_mode_override=shadow_->hero_shadow;
}
void OpeningField::advance_effects() {
    effect_submissions_.clear();
    if(render_projection)render_projection->begin_effects();
    std::array<unsigned,84> deferred{};unsigned deferred_count=0;
    const auto update=[&](unsigned slot) {
        if(auto& effect=map_effects_[slot]) {
            if(effect->actor_alias) {
                auto* object=actor(*effect->actor_alias);
                if(!object)throw std::runtime_error("Attached map effect actor disappeared");
                const auto& channels=object->animation().values();
                effect->position=channels.at(1);effect->rotation=channels.at(0);
            }
            effect->tick(random_value,true,&position);
            if(!(effect->flags&0x100)) {
                if(render_projection)render_projection->select_effect_near(effect->near_clip);
                effect_submissions_.push_back({slot,effect->snapshot()});
            }
        } else if(auto& effect=effects_[slot]) {
            Rounding rounding;
            effect->position=actor(effect->actor_alias)->animation().values().at(1);
            effect->position[1]-=8.5f;
            effect->animation.tick({-rotation[0],-rotation[1],-rotation[2],0});
            if(!(effect->animation.flags&0x100)) {
                if(render_projection)render_projection->select_effect_near(effect->near_clip);
                effect_submissions_.push_back({slot,*effect});
            }
        }
    };
    for(unsigned slot=0;slot<84;++slot) {
        auto& map=map_effects_[slot];auto& attached=effects_[slot];
        if(map&&attached)throw std::runtime_error("Shared effect handle has two owners");
        if(!map&&!attached)continue;
        auto& flags=map?map->flags:attached->animation.flags;
        if(flags&0x200){map.reset();attached.reset();continue;}
        if(flags&0x100){flags|=0x200;continue;}
        if(flags&0x4000)continue;
        if(flags&0x400)deferred[deferred_count++]=slot;
        else update(slot);
    }
    if(render_projection)render_projection->begin_deferred();
    for(unsigned i=0;i<deferred_count;++i)update(deferred[i]);
    if(render_projection)render_projection->end_effects();
}
Matrix4 OpeningField::view() const {
    Float4 p=position,r=rotation;for(unsigned i=0;i<3;++i){p[i]=-p[i];r[i]=-r[i];}p[3]=1;r[3]=0;
    return camera_matrix(p,r,{1,1,1,0},sine_);
}
void SceneClearColor::load_stage(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid clear-color STG");
    const auto begin=get(stg,8),count=get(stg,12);
    if(begin<32||count>4096||std::uint64_t(begin)+std::uint64_t(count)*20>stg.size())
        throw std::runtime_error("STG clear-color settings bounds");
    auto next=rgb;
    for(unsigned i=0;i<count;++i)if(get(stg,begin+i*20)==0x60) {
        // 0x138b40 converts authored floats to integer selector arguments;
        // 0x1bb088 then takes each integer's low byte. Reading STG bitpatterns
        // directly bypasses the first operation (invisible only for zero).
        for(unsigned c=0;c<4;++c) {
            const auto value=std::bit_cast<float>(get(stg,begin+i*20+4+c*4));
            if(!std::isfinite(value)||value < -2147483648.f||value >= 2147483648.f)
                throw std::runtime_error("STG clear-color conversion range");
            if(std::trunc(value)!=value)
                throw std::runtime_error("Fractional STG clear-color rounding is unrecovered");
            if(c<3)next[c]=std::uint8_t(std::int32_t(value));
        }
    }
    rgb=next;
}
FieldOrdinaryLighting field_ordinary_lighting(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const auto begin=get(stg,8),count=get(stg,12);
    if(begin<32||count>4096||std::uint64_t(begin)+std::uint64_t(count)*20>stg.size())
        throw std::runtime_error("STG ordinary lighting bounds");
    FieldOrdinaryLighting result;Rounding rounding;
    for(unsigned i=0;i<count;++i) {
        const auto at=begin+i*20,kind=get(stg,at);
        if(!(kind>=1&&kind<=3)&&!(kind>=0x11&&kind<=0x13)&&kind!=0x40&&kind!=0x41&&kind!=0x45)continue;
        Float4 value;
        for(unsigned c=0;c<4;++c) {
            value[c]=std::bit_cast<float>(get(stg,at+4+c*4));
            if(!std::isfinite(value[c]))throw std::runtime_error("Non-finite STG ordinary lighting");
        }
        if(kind<=3)result.colors[kind-1]=value;
        else if(kind<=0x13){value[3]=0;result.directions[kind-0x11]=value;}
        else if(kind==0x40)result.base=value;
        else {
            if(kind==0x45) {
                result.base=value;
                for(unsigned c=0;c<3;++c)value[c]*=.25f;
            }
            result.colors[3]=value;
        }
    }
    return result;
}
Float4 field_effect_tint(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const auto begin=get(stg,8),count=get(stg,12);
    if(begin<32||count>4096||std::uint64_t(begin)+std::uint64_t(count)*20>stg.size())throw std::runtime_error("STG lighting bounds");
    std::optional<Float4> base,ambient,light;
    Rounding rounding;
    for(unsigned i=0;i<count;++i) {
        const auto at=begin+i*20,kind=get(stg,at);
        if(kind!=1&&kind!=0x40&&kind!=0x41&&kind!=0x45)continue;
        Float4 v;for(unsigned k=0;k<4;++k){v[k]=std::bit_cast<float>(get(stg,at+4+k*4));if(!std::isfinite(v[k]))throw std::runtime_error("Invalid STG lighting");}
        if(kind==1)light=v;
        else if(kind==0x40)base=v;
        else if(kind==0x41)ambient=v;
        else {base=v;for(unsigned k=0;k<3;++k)v[k]*=.25f;ambient=v;}
    }
    if(!base||!ambient||!light)throw std::runtime_error("Effect lighting requires explicit STG colors");
    return effect_light_tint(*base,*ambient,*light);
}
Float4 field_sky_follow(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const auto begin=get(stg,8),count=get(stg,12);
    if(begin<32||count>4096||std::uint64_t(begin)+std::uint64_t(count)*20>stg.size())
        throw std::runtime_error("STG sky settings bounds");
    Float4 factors{.5f,0,.5f,0}; // 0x1b8028 defaults; later records overwrite.
    for(unsigned i=0;i<count;++i)if(get(stg,begin+i*20)==0x46)
        for(unsigned j=0;j<4;++j){factors[j]=std::bit_cast<float>(get(stg,begin+i*20+4+j*4));
            if(!std::isfinite(factors[j]))throw std::runtime_error("Non-finite STG sky factor");}
    return factors;
}
Float4 field_projection(Bytes stg,float* far_clip) {
    FieldProjection projection;projection.load_stage(stg);
    if(far_clip)*far_clip=projection.current().clip_far;
    return field_clip_projection(projection.current());
}

AnchoredCamera field_anchored_camera(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const std::size_t begin=get(stg,16),count=get(stg,20),end=get(stg,24);
    if(begin<32||begin%4||!count||count>30||end>stg.size()||begin+count*28>end||
       (end-begin-count*28)%28||(end-begin-count*28)/28>30)
        throw std::runtime_error("Invalid STG camera tables");
    auto number=[&](std::size_t at) {
        const auto v=std::bit_cast<float>(get(stg,at));
        if(!std::isfinite(v))throw std::runtime_error("Non-finite STG camera setting");
        return v;
    };
    AnchoredCamera result;result.anchor={number(begin+4),number(begin+8),number(begin+12),1};
    for(auto at=begin+count*28;at<end;at+=28) {
        // 0x138a50 selects the first settings record with signed ID zero.
        if((get(stg,at)>>16)!=0)continue;
        if(number(at+16)!=1)throw std::runtime_error("Unrecovered STG exploration camera mode");
        result.distance_offset=number(at+4);result.distance_scale=number(at+8);
        result.target_height=number(at+12);return result;
    }
    throw std::runtime_error("STG anchored camera has no default settings");
}
std::vector<FieldCameraSetting> field_camera_settings(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const std::uint64_t begin=get(stg,16),count=get(stg,20),end=get(stg,24);
    if(begin<32||begin%4||count>30||end>stg.size()||begin+count*28>end||
       (end-begin-count*28)%28||(end-begin-count*28)/28>30)throw std::runtime_error("Invalid STG camera tables");
    std::vector<FieldCameraSetting> result;
    for(auto at=begin+count*28;at<end;at+=28) {
        FieldCameraSetting setting;setting.id=std::bit_cast<std::int16_t>(std::uint16_t(get(stg,at)>>16));
        std::array<float*,4> fields{&setting.height,&setting.scale,&setting.target,&setting.mode};
        for(unsigned i=0;i<4;++i){*fields[i]=std::bit_cast<float>(get(stg,at+4+i*4));if(!std::isfinite(*fields[i]))throw std::runtime_error("Invalid STG camera setting");}
        result.push_back(setting);
    }
    return result;
}
FieldEntrance field_entrance(Bytes stg,unsigned entrance) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const std::uint64_t start=get(stg,24),count=get(stg,28);
    if(start<32||(start&3)||count>30||entrance>=count||start+count*28>stg.size())
        throw std::runtime_error("Invalid STG entrance table/index");
    FieldEntrance result;result.position[3]=result.rotation[3]=1;
    for(unsigned i=0;i<6;++i) {
        const float value=std::bit_cast<float>(get(stg,start+entrance*28+4+i*4));
        if(!std::isfinite(value))throw std::runtime_error("Non-finite STG entrance");
        (i<3?result.position:result.rotation)[i%3]=value;
    }
    return result;
}
}
