#include "okage/field.hpp"
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
    Float4 p{0,0,0,1};for(unsigned i=0;i<3;++i){p[i]=__builtin_bit_cast(float, (word(at+4*i)));if(!std::isfinite(p[i]))throw std::runtime_error("Invalid field point");}return p;
}
OpeningField::OpeningField(Bytes bytes,std::function<void(int)> start,std::function<bool(int)> wait,std::span<const ActorBinding> actors,bool diagnostic):
    bytes_(bytes.begin(),bytes.end()),script_(bytes,3),start_dialogue_(std::move(start)),wait_dialogue_(std::move(wait)) {
    actors_.assign(actors.begin(),actors.end());
    for(std::size_t i=0;i<actors_.size();++i) {
        const auto& binding=actors_[i];
        if(binding.alias>=0 || binding.alias < -2048 || binding.alias==-2 || !binding.actor)throw std::runtime_error("Invalid actor binding");
        for(std::size_t j=0;j<i;++j)if(actors_[j].alias==binding.alias || actors_[j].actor==binding.actor)throw std::runtime_error("Duplicate actor binding");
        binding.actor->select_motion(1); // Original field actor construction 0x138220.
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
    if(alias==-7 && interaction_alias_)alias=interaction_alias_;
    for(const auto& binding:actors_)if(binding.alias==alias)return binding.actor;
    return nullptr;
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
        result.scene=std::stoi(number);result.party=get(data,at);result.startup=get(data,at+12);
        result.music=__builtin_bit_cast(std::int32_t, (get(data,at+40)));result.secondary_music=__builtin_bit_cast(std::int32_t, (get(data,at+44)));
        result.room_directory=string(get(data,at+16));result.room=string(get(data,at+20));result.message=string(get(data,at+24));
        unsigned index=12;
        for(;index<words && get(data,at+index*4);index+=8) {
            if(index+8>words || result.npcs.size()>=256)throw std::runtime_error("Field NPC definition bounds");
            const auto p=at+index*4;FieldNpcDefinition npc;
            npc.alias=-8-int(result.npcs.size());npc.model=string(get(data,p));
            npc.state=__builtin_bit_cast(std::int32_t, (get(data,p+4)));npc.talk=__builtin_bit_cast(std::int32_t, (get(data,p+8)));
            npc.alternate_talk=__builtin_bit_cast(std::int32_t, (get(data,p+12)));npc.position[3]=1;
            for(unsigned k=0;k<3;++k)npc.position[k]=__builtin_bit_cast(float, (get(data,p+16+k*4)));
            npc.facing=__builtin_bit_cast(float, (get(data,p+28)));
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
FieldDefinition prepare_field_definition(Bytes script,Bytes variables,FieldDefinition definition) {
    if(definition.type!=1)throw std::runtime_error("Field construction currently requires interior class 1");
    std::array<std::uint8_t,0x800> setup{};std::fill_n(setup.begin()+0x1a8,256,1);
    auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)setup[at+i]=value>>(i*8);};
    put(0x400,definition.music);put(0x404,definition.secondary_music);put(0x408,definition.type);
    put(0x410,definition.startup);put(0x42c,33);
    run_event_setup(script,variables,setup);
    definition.type=get(setup,0x408);definition.startup=get(setup,0x410);
    definition.music=__builtin_bit_cast(std::int32_t, (get(setup,0x400)));definition.secondary_music=__builtin_bit_cast(std::int32_t, (get(setup,0x404)));
    definition.footstep=get(setup,0x42c);definition.dust=get(setup,0x414);
    for(auto& npc:definition.npcs)npc.flags=setup[0x1a8+(-8-npc.alias)];
    std::erase_if(definition.npcs,[](const auto& npc){return !npc.flags;});
    return definition;
}
void OpeningField::load_interactions(Bytes database,const std::string& room,Bytes variables) {
    if(definition_||!actor(-1))throw std::runtime_error("Field interactions require one configuration and a player");
    auto definition=prepare_field_definition(bytes_,variables,read_field_definition(database,room));
    if(definition.event!="e_"+room)throw std::runtime_error("Field event definition does not match diagnostic room");
    for(const auto& npc:definition.npcs)if(!actor(npc.alias)||npc.state!=0)
        throw std::runtime_error("Field NPC binding/state is not implemented");
    if(definition.npcs.size()+1!=actors_.size())throw std::runtime_error("Field NPC bindings differ from enabled definitions");
    definition_=std::move(definition);
}
void OpeningField::clear_contact() {
    player_.floor_contact=0;player_.trigger_kind=player_.trigger_index=0;
    if(player_.contact_bubble) {
        const auto handle=player_.contact_bubble&0xfff;
        if(handle>=effects_.size())throw std::runtime_error("Invalid contact effect handle");
        if(effects_[handle])effects_[handle]->animation.request_removal();player_.contact_bubble=0;
    }
}
void OpeningField::interact(ExplorationInput input) {
    if(!(input.pressed&0x40)||!definition_)return;
    if(!get_variable||!set_variable)throw std::runtime_error("Field interaction requires native variables");
    if(player_.trigger_kind>=0x80) {
        interaction_alias_=0;interaction_event_=int(player_.trigger_index);player_.trigger_kind=0;
        saved_input_gate_=get_variable(0x40006);set_variable(6,4,1);input_gate_saved_=true;
        script_.start(unsigned(interaction_event_));
        if(event_running()) {exploration_handoff_=false;player_.mask|=1;clear_contact();}
        else {set_variable(6,4,saved_input_gate_);input_gate_saved_=false;}
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
    auto* npc=actor(selected->alias);npc->select_motion(3);selected->state=3;
    if(get_variable(0x40005)==0) {
        const auto p=npc->animation().values().at(1);
        npc->animation().set_channel(0,{0,camera_angle(-(p[0]-position[0]),-(p[2]-position[2]),angles_)+.5f,0,0});
    }
    player_.mask|=1;clear_contact();
    saved_input_gate_=get_variable(0x40006);set_variable(6,4,1);input_gate_saved_=true;
    exploration_handoff_=false;
    // 0x1bb7e0 -> 0x1b5dd0 executes the newly selected event immediately.
    script_.start(unsigned(interaction_event_));
}
void OpeningField::load_effects(Bytes bytes) {
    if(!effect_definitions_.empty())throw std::runtime_error("Field effects already loaded");
    effect_definitions_=read_efd(bytes);
}
unsigned OpeningField::attach_bubble(int parameter,int alias) {
    const auto definition=std::find_if(effect_definitions_.begin(),effect_definitions_.end(),[](const auto& d){return d.id==30;});
    if(definition==effect_definitions_.end())throw std::runtime_error("Bubble requires owned TES.EFD and effect textures");
    if(!actor(alias))throw std::runtime_error("Unresolved effect attachment actor");
    unsigned handle=0;while(handle<effects_.size()&&effects_[handle])++handle;
    if(handle==effects_.size())throw std::runtime_error("Field effect capacity exhausted");
    effects_[handle]=std::make_unique<AttachedEffect>(*definition,parameter,alias);return handle;
}
void OpeningField::load_exploration_camera(Bytes stg) {
    if(!actor(-1)||exploration_camera_)throw std::runtime_error("Exploration camera requires a player and one configuration");
    exploration_camera_=field_anchored_camera(stg);
}
void OpeningField::load_exploration_collision(Bytes xcl,Bytes variables) {
    if(!actor(-1)||collision_||actors_.size()!=3 || actors_[0].alias!=-8 ||
       actors_[1].alias!=-9 || actors_[2].alias!=-1)
        throw std::runtime_error("Exploration currently requires HOME1C MAM/IMO/HERO construction order");
    std::array<std::uint8_t,0x800> setup{};setup[0x42c]=33;
    run_event_setup(bytes_,variables,setup);
    player_.footstep_sound=get(setup,0x42c);player_.dust_effect=get(setup,0x414);
    build_collision(xcl);
}
void OpeningField::build_collision(Bytes xcl) {
    collision_=read_xcl(xcl);
    Rounding rounding;
    for(unsigned i=0;i<actors_.size();++i) {
        CollisionBody body;body.id=i;body.priority=10;body.floor_reach=4;
        const float radius=__builtin_bit_cast(float, (0x3ff33333u));body.broad_radius=radius+radius;
        body.shapes={{8,{{0,0,0,1},{},0}},{0x72,{{0,-2.5f,0,1},{0,-5,0,1},radius}}};
        body.position=body.previous=actors_[i].actor->animation().values().at(1);
        if(i&&!actors_[i].actor->visible)body.flags|=2;
        bodies_.push_back(std::move(body));
    }
}
void OpeningField::enter(FieldDefinition definition,Bytes stg,Bytes xcl,unsigned entrance) {
    Rounding rounding;
    if(definition_||actors_.empty()||actors_.back().alias!=-1||definition.type!=1||definition.party)
        throw std::runtime_error("Unrecovered field entry construction");
    if(definition.npcs.size()+1!=actors_.size())throw std::runtime_error("Field entry NPC bindings differ");
    const auto placement=field_entrance(stg,entrance);
    for(const auto& npc:definition.npcs) {
        auto* object=actor(npc.alias);
        if(!object||npc.state!=0||(npc.flags&~3u))throw std::runtime_error("Unrecovered field NPC construction state/flags");
        object->animation().set_channel(0,{0,npc.facing,0,1});object->animation().set_channel(1,npc.position);
        object->visible=(npc.flags&2)==0;
    }
    definition_=std::move(definition);player_.footstep_sound=definition_->footstep;player_.dust_effect=definition_->dust;
    auto* hero=actor(-1);hero->animation().set_channel(0,placement.rotation);hero->animation().set_channel(1,placement.position);
    build_collision(xcl);
    // 0x135300: settle with a Y=1 collision move, then finish placement
    // by reading back the corrected output and initializing all 32 history entries.
    auto& body=bodies_.back();auto desired=placement.position;desired[1]+=1;
    MapCollisionSettings settings;settings.wall_iterations=3;
    resolve_collision(*collision_,bodies_,bodies_.size()-1,desired,settings,__builtin_bit_cast(float, (0x3efff17du)));
    body.position=body.previous=desired;hero->animation().set_channel(1,desired);
    player_.floor_contact=(body.contact.flags&1)?body.contact.materials[0]:-1;
    player_.camera_area=(std::uint32_t(player_.floor_contact)&0x38)>>3;
    player_.history.fill(desired);
    player_.mask=0;player_.motion=1;exploration_handoff_=true;
    load_exploration_camera(stg);
    const auto camera=anchored_camera(desired,*exploration_camera_,angles_,sine_);
    script_.set_channel(0,camera.position);requested_target_=camera.target;camera_mode_=2;
    if(!get_variable||!set_variable)throw std::runtime_error("Field entry requires native variables");
    if(definition_->startup) {
        saved_input_gate_=get_variable(0x40006);set_variable(6,4,1);input_gate_saved_=true;
        script_.start(definition_->startup);
        if(event_running()){exploration_handoff_=false;player_.mask|=1;}
        else{set_variable(6,4,saved_input_gate_);input_gate_saved_=false;}
    }
    for(const auto& binding:actors_)binding.actor->evaluate();
    evaluate();
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
        for(unsigned i=0;i<bodies_.size();++i)bodies_[i].position=actors_[i].actor->animation().values().at(1);
        auto& body=bodies_.back();body.previous=body.position;Float4 desired;
        for(unsigned i=0;i<4;++i)desired[i]=body.position[i]+delta[i];
        MapCollisionSettings settings;settings.wall_iterations=3;
        resolve_collision(*collision_,bodies_,bodies_.size()-1,desired,settings,__builtin_bit_cast(float, (0x3efff17du)));
        body.position=desired;
        // 0x1ba948 commits the mover and copies every current position to history.
        // Pushed NPC positions are native animation channels, not detached copies.
        for(unsigned i=0;i<bodies_.size();++i) {
            if(i==bodies_.size()-1)actors_[i].actor->animation().set_channel(1,bodies_[i].position);
            else actors_[i].actor->animation().write_channel_output(1,bodies_[i].position);
            bodies_[i].previous=bodies_[i].position;
        }
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
            effects_[state.contact_bubble&0xfff]->player_contact=true;
        }
        if(state.camera_area || kind==0x81)
            throw std::runtime_error("Field contact camera/scene transition is unrecovered");
    }
    if(motion==3) {
        if(__builtin_bit_cast(std::int32_t, (state.run_delay))<24){if(state.motion>=0)motion=state.motion;++state.run_delay;}
        else state.run_delay=0;
    } else ++state.run_delay;
    if(motion==state.motion) {
        const auto counter=__builtin_bit_cast(std::int32_t, (state.effect_counter));
        if((motion==2&&counter<0)||(motion==3)) {
            if(state.dust_effect && ((motion==2&&!(state.effect_counter&3)) ||
               (motion==3&&counter<40&&!(state.effect_counter&7))))
                throw std::runtime_error("Player dust effect is unrecovered");
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
        case 11: {
            if(!get_variable)return pending();
            const auto value=get_variable(word(pc+4)),expected=__builtin_bit_cast(std::int32_t, (word(pc+8)));
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
            if(!definition_||definition_->type!=1)return pending();
            return {pc+4};
        case 7:
            if(b==-2)script_.set_channel_target(nullptr);
            else if(auto* target=actor(b))script_.set_channel_target(&target->animation());
            else return pending();
            group_=b;return {pc+4};
        case 0x4b:
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
        case 0x4c:
            if(a || b)throw std::runtime_error("Field dialogue mode not yet implemented");
            start_dialogue_(__builtin_bit_cast(std::int32_t, (word(pc+4))));return {pc+8};
        case 10:return wait_dialogue_(b) ? Animation::ServiceResult{pc+4} : Animation::ServiceResult{pc,true};
        case 0x36:
            if(auto* target=actor(b);target && a<0x800) {target->select_motion(a);return {pc+4};}
            return pending();
        case 0x3b:
            if(auto* target=actor(b);target && (a&0x100)) {
                target->visible=(a&255)!=0;
                // Original 0x1baa60 excludes collision index zero.
                for(unsigned i=1;i<bodies_.size();++i)if(actors_[i].actor==target)
                    bodies_[i].flags=target->visible?(bodies_[i].flags&~2u):(bodies_[i].flags|2u);
                return {pc+4};
            }
            return pending();
        case 0x45:
            if(audio_service && audio_service(a,b)){pending_opcode=0;return {pc+4};}
            return pending();
        case 0x68:
            if(sound_cue && sound_cue(a,b,__builtin_bit_cast(float, (word(pc+4))),word(pc+8))){pending_opcode=0;return {pc+12};}
            return pending();
        case 0x47: {
            const auto selector=word(pc+4),index=selector&0xffff,bank=selector>>16;
            if(bank>4 || (bank==4 && index==6))throw std::runtime_error("Unrecovered field variable storage or controller side effect");
            if(!set_variable)return pending();
            set_variable(index,bank,b);pending_opcode=0;return {pc+8};
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
            return {pc+4};
        }
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
        default:throw std::runtime_error("Unrecovered field service "+std::to_string(op));
    }
}
void OpeningField::evaluate() {
    position=script_.values().at(0);target=requested_target_;blend.apply(position,target);
    if(camera_mode_==2) {
        rotation=camera_look_at(position,target,angles_,sine_);
        // Original look-at 0x1b9440 writes the result back into channel 1.
        script_.set_channel(1,rotation);
    } else {
        rotation=script_.values().at(1);
        requested_target_=target;
    }
}
void OpeningField::tick(ExplorationInput input) {
    // Field manager 0x133a98 runs before the event pass. Case 5 detects the
    // ended script and selects the room camera; case 0 updates it next frame.
    const bool was_transitioning=bool(transition_);
    bool move=false;
    if(transition_) {
        if(transition_->ready)throw std::runtime_error("Destination loader did not consume field transition");
        if(transition_->countdown) {
            --transition_->countdown;
            if(!transition_service)throw std::runtime_error("Field transition requires fade services");
            transition_service(transition_->countdown);
        } else transition_->ready=true;
    }
    if(!was_transitioning&&exploration_camera_&&!event_running()) {
        if(exploration_handoff_) {
            interact(input);
            move=!event_running()&&!transition_;
        } else {
            exploration_handoff_=true;
            // 0x132ca8 resets contact state, clears movement bit 0 and invalidates
            // selected motion; 0x134c18 chooses it on the next player update.
            player_.floor_contact=0;player_.trigger_kind=player_.trigger_index=0;
            player_.mask&=~1u;player_.motion=-1;
            if(input_gate_saved_) {
                // Mode 5 clears field +0x64 before 0x132ca8. The NPC therefore
                // retains state 3 and its event-selected motion/interpolation;
                // only the direct-message mode-4 path requests NPC state 8.
                set_variable(6,4,saved_input_gate_);input_gate_saved_=false;
            }
        }
    }
    advance_effects(true);
    // Mode14 changes the field manager, not the player's movement mask.
    // 0x1354d8 still runs movement while bit0 is clear during the countdown.
    if(move||transition_)move_player(input);
    if(move) {
        const auto camera=anchored_camera(actor(-1)->animation().values().at(1),*exploration_camera_,angles_,sine_);
        script_.set_channel(0,camera.position);requested_target_=camera.target;camera_mode_=2;
    }
    if(collision_)remember_player();
    script_.tick_script();
    for(const auto& binding:actors_)binding.actor->tick_script();
    script_.tick_channels();evaluate();
    // Actor groups have flag1 (not global-advance flag2). Original 0x1b9be0
    // calls 0x1b70a0 only for visible instances; hidden motion scripts still run.
    for(const auto& binding:actors_)if(binding.actor->visible){binding.actor->tick_channels();binding.actor->evaluate();}
    advance_effects(false);
}
void OpeningField::advance_effects(bool player_contacts) {
    for(auto& effect:effects_)if(effect&&effect->player_contact==player_contacts) {
        auto& flags=effect->animation.flags;
        if(flags&0x200){effect.reset();continue;}
        if(flags&0x100){flags|=0x200;continue;}
        Rounding rounding;
        effect->position=actor(effect->actor_alias)->animation().values().at(1);
        effect->position[1]-=8.5f;
        effect->animation.tick({-rotation[0],-rotation[1],-rotation[2],0});
    }
}
Matrix4 OpeningField::view() const {
    Float4 p=position,r=rotation;for(unsigned i=0;i<3;++i){p[i]=-p[i];r[i]=-r[i];}p[3]=1;r[3]=0;
    return camera_matrix(p,r,{1,1,1,0},sine_);
}
Float4 field_projection(Bytes stg) {
    Rounding rounding;
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const auto begin=get(stg,8),count=get(stg,12);
    if(begin<32 || count>4096 || std::uint64_t(begin)+std::uint64_t(count)*20>stg.size())throw std::runtime_error("STG settings bounds");
    Float4 settings{};bool found=false;
    for(unsigned i=0;i<count;++i)if(get(stg,begin+i*20)==0x44) {
        if(found)throw std::runtime_error("Multiple STG projection records");found=true;
        for(unsigned j=0;j<4;++j){settings[j]=__builtin_bit_cast(float, (get(stg,begin+i*20+4+j*4)));if(!std::isfinite(settings[j]))throw std::runtime_error("Non-finite STG projection");}
    }
    if(!found || settings[0]<=0 || settings[1]<=0 || settings[2]<=0 || settings[3]>=10)throw std::runtime_error("Unsupported STG projection");
    return {(settings[2]*settings[0])/320.f,(settings[2]*settings[1])/112.f,
        (1024.f+3.f)/(1024.f-3.f),((1024.f*-2.f)*3.f)/(1024.f-3.f)};
}
AnchoredCamera field_anchored_camera(Bytes stg) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const std::size_t begin=get(stg,16),count=get(stg,20),end=get(stg,24);
    if(begin<32||begin%4||!count||count>30||end>stg.size()||begin+count*28>end||
       (end-begin-count*28)%28||(end-begin-count*28)/28>30)
        throw std::runtime_error("Invalid STG camera tables");
    auto number=[&](std::size_t at) {
        const auto v=__builtin_bit_cast(float, (get(stg,at)));
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
FieldEntrance field_entrance(Bytes stg,unsigned entrance) {
    if(get(stg,0)!=0x475453)throw std::runtime_error("Invalid STG magic");
    const std::uint64_t start=get(stg,24),count=get(stg,28);
    if(start<32||(start&3)||count>30||entrance>=count||start+count*28>stg.size())
        throw std::runtime_error("Invalid STG entrance table/index");
    FieldEntrance result;result.position[3]=result.rotation[3]=1;
    for(unsigned i=0;i<6;++i) {
        const float value=__builtin_bit_cast(float, (get(stg,start+entrance*28+4+i*4)));
        if(!std::isfinite(value))throw std::runtime_error("Non-finite STG entrance");
        (i<3?result.position:result.rotation)[i%3]=value;
    }
    return result;
}
}
