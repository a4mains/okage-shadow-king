#import <AppKit/AppKit.h>
#include "okage/dialogue.hpp"
#include "okage/event.hpp"
#include "okage/field.hpp"
#include "okage/music.hpp"
#include "okage/audio_output.hpp"
#include "okage/stream_bank.hpp"
#include "okage/scene_renderer.hpp"
#include "okage/map_objects.hpp"
#include <bit>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>

namespace fs=std::filesystem;
static std::vector<std::uint8_t> readData(const fs::path& path) {
    const auto n=fs::file_size(path);if(n>128*1024*1024)throw std::runtime_error("Runtime input limit");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read runtime input");return b;
}
static okage::TextureAnimation readActorTextures(const fs::path& path) {
    // 0x1b6a18 initializes empty bindings and ignores 0x1b6848's missing-XTA
    // return. RKUMO/BKUMO have no companion; texture operations still validate.
    if(!fs::exists(path))return {};
    const auto data=readData(path);
    return okage::read_xta({reinterpret_cast<const char*>(data.data()),data.size()});
}
struct DirectionReplay {
    std::vector<std::pair<unsigned,std::uint16_t>> entries;
    void load(const fs::path& path){
        std::ifstream in(path);if(!in)throw std::runtime_error("Cannot read direction replay");
        std::string line;unsigned previous=0;
        while(std::getline(in,line)){
            if(line.empty() || line[0]=='#')continue;
            std::istringstream row(line);unsigned frame,mask;std::string extra;
            if(!(row>>frame>>std::hex>>mask) || (row>>extra) || frame<=previous || frame>36000 || (mask&~0xf000u))
                throw std::runtime_error("Direction replay requires increasing FRAME and hexadecimal directional mask");
            entries.push_back({frame,std::uint16_t(mask)});previous=frame;
        }
    }
    std::uint16_t held(unsigned frame) const {
        const auto found=std::upper_bound(entries.begin(),entries.end(),frame,[](unsigned f,const auto& entry){return f<entry.first;});
        return found==entries.begin()?0:std::prev(found)->second;
    }
};
struct Session {
    std::array<okage::Font,3> fonts;
    okage::MessageBank messages;
    fs::path message_directory;
    std::string message_bank,player_name;
    okage::DialogueText text;
    okage::TextClock clock;
    okage::DialogueController controller;
    okage::DialoguePadRepeat pad_repeat;
    okage::DialogueOpacity opacity;
    okage::DialogueClosure closure;
    okage::SceneFade scene_fade;
    okage::Narration narration;
    okage::ChoiceMenu choices;
    std::vector<std::uint32_t> choice_trace;
    std::array<std::uint8_t,0x1000> variables{};
    okage::GameRandom random{1};
    okage::CollisionMask collision_mask;
    okage::ExteriorCamera exterior_camera;
    okage::DialogueWaitState wait{5,1};
    unsigned frame=0,release_frame=0,close_frame=0,close_release_frame=0;
    unsigned render_from=0,rendered_frame=0;
    okage::FrameFeedback feedback;
    std::optional<okage::Texture> rendered_surface;
    std::vector<std::uint32_t> filter_trace;
    std::unique_ptr<okage::MusicRouter> music;
    std::unique_ptr<okage::AudioOutput> audio_output;
    std::vector<std::int16_t> audio_capture;
    std::vector<std::uint32_t> music_trace;
    bool record_audio=false;
    fs::path sound_directory,music_directory;
    std::map<unsigned,std::unique_ptr<okage::StreamPlayer>> sound_banks;
    std::vector<std::uint32_t> sound_trace;
    okage::MusicResource read_music_resource(const fs::path& sequence,const fs::path& bank,const fs::path& alternate={}) {
        okage::MusicResource resource;
        resource.sequence=readData(sequence);resource.bank=okage::read_music_bank(readData(bank));
        const auto second_path=sequence.parent_path()/(sequence.stem().string()+"1.XV");
        if(fs::exists(second_path))resource.second=okage::read_music_bank(readData(second_path));
        if(!alternate.empty())resource.alternate=readData(alternate);
        return resource;
    }
    okage::MusicResource read_music_resource(int id) {
        if(music_directory.empty())throw std::runtime_error("Destination music requires an imported BGM directory");
        const auto names=okage::music_resource_names(id);
        const auto alternate=*names.alternate?music_directory/(std::string(names.alternate)+".XSQ"):fs::path{};
        return read_music_resource(music_directory/(std::string(names.sequence)+".XSQ"),
                                   music_directory/(std::string(names.primary_bank)+".XV"),alternate);
    }
    void load_music(const fs::path& sequence,const fs::path& bank,bool record,const fs::path& sounds){
        // 0x12d158 loads <sequence>1.xv if present and binds it to bank slot1.
        // The supplied primary bank can differ (the original jingles share one).
        music_directory=sequence.parent_path();music=std::make_unique<okage::MusicRouter>();
        int id=0;
        for(int candidate:{10,26,51})if(sequence.stem()==okage::music_resource_names(candidate).sequence &&
                                      bank.stem()==okage::music_resource_names(candidate).primary_bank)id=candidate;
        const auto alternate=id==26?music_directory/"SNT_DNM2.XSQ":fs::path{};
        music->install(0,id,read_music_resource(sequence,bank,alternate));music->select(id);
        record_audio=record;sound_directory=sounds;
        if(!sound_directory.empty())sound_banks.emplace(0,std::make_unique<okage::StreamPlayer>(
            okage::read_stream_bank(readData(sound_directory/"ALL.XWS"))));
        bind_audio_services();
        std::cout<<"Native dry music enabled: original sequence/banks; reverb pending\n";
    }

    void bind_audio_services() {
        if(field)field->audio_service=[this](unsigned operation,int argument){
            if(operation==1 && !sound_directory.empty()){
                if(argument<0 || argument>9999)throw std::runtime_error("Sound set number outside import scope");
                wait.mode=15;wait.active=0;
                const auto path=sound_directory/("SET"+std::to_string(argument)+".XWS");
                auto bank=okage::read_stream_bank(readData(path));
                const auto count=bank.records.size();sound_banks.insert_or_assign(1,std::make_unique<okage::StreamPlayer>(std::move(bank)));
                // Native disk loading is synchronous. The actual owned bank must
                // be registered before the subsequent -1 completion wait can pass.
                wait.mode=5;wait.active=1;
                std::cout<<"Native sound set "<<argument<<": "<<count<<" RXWS records loaded\n";return true;
            }
            if(operation==2){sound_banks.erase(1);return true;}
            if((operation!=4 && operation!=5) || argument<0)return false;
            if(!music)return false;
            music->volume.request(operation==4 ? 128 : 80,unsigned(argument));return true;
        };
        if(field)field->music_service=[this](int id,bool fade){
            if(!music)return false;
            music->select(id,fade);return true;
        };
        if(field)field->music_channels=[this](int first,unsigned last,unsigned level){
            if(!music)return false;
            music->channels.request(first,int(last),level);return true;
        };
        if(field)field->sound_cue=[this](unsigned bank,int index,float speed,unsigned volume){
            auto found=sound_banks.find(bank);if(found==sound_banks.end())return false;
            try {found->second->play(index,speed,volume);}
            catch(const std::exception& e) {
                throw std::runtime_error("Sound cue frame "+std::to_string(frame)+", bank "+std::to_string(bank)+
                    ", record "+std::to_string(index)+": "+e.what());
            }
            sound_trace.insert(sound_trace.end(),{frame,bank,unsigned(index),__builtin_bit_cast(unsigned, (speed)),volume});return true;
        };
    }

    bool dialogue_active=true;
    std::vector<std::unique_ptr<okage::FieldActor>> actor_objects;
    std::vector<okage::ActorBinding> actor_bindings;
    std::unique_ptr<okage::MapObjects> map_objects;
    std::unique_ptr<okage::OpeningField> field;
    std::unique_ptr<okage::SceneRenderer> renderer;
    okage::Float4 projection{};
    float far_clip=1024;
    std::vector<std::uint32_t> scene_trace,dialogue_trace,camera_state_trace,movement_trace,interaction_trace;
    std::vector<std::uint32_t> effect_trace,map_effect_trace;
    bool effects_loaded=false;
    std::map<int,std::vector<std::uint32_t>> actor_traces;
    int message_id=10100;unsigned messages_started=0;
    std::optional<okage::DialogueOpening> opening;
    std::uint64_t service_request_count=0;
    Session(const fs::path& directory,const fs::path& pme,const std::string& name):
        fonts{okage::read_fnt(readData(directory/"IQ24.FNT")),okage::read_fnt(readData(directory/"BMUI.FNT")),okage::read_fnt(readData(directory/"BM.FNT"))},
        messages(okage::read_pme(readData(pme))),message_directory(pme.parent_path()),
        message_bank(pme.stem().string()),player_name(name),text({}) {
        if(player_name.empty() || player_name.size()>12 || std::any_of(player_name.begin(),player_name.end(),[](unsigned char c){return c<32 || c>=127 || c=='\\';}))
            throw std::runtime_error("Diagnostic player name requires 1..12 printable ASCII characters without text escapes");
        if(!messages.bind("############",player_name))throw std::runtime_error("Player-name tag missing from message bank");
        text=okage::DialogueText(messages,10100);
        text.fonts=fonts;text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        clock.glyph_flags=0x10;opacity.set(1,8);
        scene_fade.current=scene_fade.target=1; // Already lit diagnostic entry.
    }
    fs::path field_root,event_directory,effect_data,effect_textures;
    int current_scene=-1,current_entrance=0;
    std::vector<std::uint32_t> transition_trace;
    std::optional<okage::Texture> transition_image;
    unsigned transition_alpha=0,transition_target=0;
    void create_field(okage::Bytes script,bool diagnostic) {
        field=std::make_unique<okage::OpeningField>(script,[this](int id){
            if(dialogue_active && !closure.finished())throw std::runtime_error("Field replaces an unfinished message");
            const bool legacy_diagnostic=id==10026||id==10030||(id>=10100&&id<=10107);
            // Normal bank0/opcode4c uses configuration0x2293e0 for every message
            // ID. Its logical construction is independent of PME content;
            // unsupported text/window commands still fail in their handlers.
            message_id=id;++messages_started;
            text=okage::DialogueText(messages,id);text.fonts=fonts;
            text.state.flags=0x8041;text.state.font=2;text.state.period=1;
            clock={};clock.glyph_flags=0x10;controller={};closure={};opacity.set(1,8);
            opening.reset();
            if(!legacy_diagnostic){opening.emplace();opening->initialize(text,clock,controller,opacity,closure);}
            controller.get_variable=[this](unsigned index,unsigned bank){return okage::event_variable(variables,(bank<<16)|index);};
            controller.set_variable=[this](unsigned index,unsigned bank,std::int32_t value){okage::set_event_variable(variables,index,bank,value);};
            wait.countdown=wait.observed=wait.accumulated=0;
            dialogue_active=true;
        },[this](int request){
            if(!dialogue_active && request!=-1)throw std::runtime_error("Field wait before dialogue initialization");
            const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
            const bool released=okage::poll_dialogue_wait(wait,request,
                {true,__builtin_bit_cast(std::int32_t, (closure.message_flags)),layers,std::int32_t(text.state.progress)})==0;
            if(released && !release_frame)release_frame=frame;
            if(released && request==0 && !close_release_frame)close_release_frame=frame;
            return released;
        },actor_bindings,diagnostic);
        if(map_objects)field->bind_map_objects(map_objects->bindings());
        field->random_value=[this]{return random.next();};
        field->exterior_camera=&exterior_camera;
        field->collision_mask=&collision_mask;
        field->set_variable=[this](unsigned index,unsigned bank,std::int32_t value){okage::set_event_variable(variables,index,bank,value);};
        field->get_variable=[this](std::uint32_t selector){return okage::event_variable(variables,selector);};
        field->transition_service=[this](unsigned countdown){
            if(countdown==1)transition_image=surface();
            else if(countdown==0){transition_alpha=transition_target=128;}
        };
    }
    void load_scene(const fs::path& map,const fs::path& event,const std::vector<std::pair<int,fs::path>>& assets) {
        std::vector<okage::SceneActor> scene_actors;
        for(const auto& [alias,path]:assets) {
            auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            auto object=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),readData(motion),
                readActorTextures(xta));
            actor_bindings.push_back({alias,object.get()});scene_actors.push_back({object.get(),path});
            actor_objects.push_back(std::move(object));
        }
        map_objects=std::make_unique<okage::MapObjects>(map);
        renderer=std::make_unique<okage::SceneRenderer>(map,scene_actors,map_objects.get());
        auto stg=map;stg.replace_extension(".stg");projection=okage::field_projection(readData(stg),&far_clip);
        dialogue_active=false;
        create_field(readData(event),true);
        if(std::any_of(actor_bindings.begin(),actor_bindings.end(),[](const auto& a){return a.alias==-1;})) {
            auto room=map.parent_path().stem().string();
            std::transform(room.begin(),room.end(),room.begin(),[](unsigned char c){return char(std::tolower(c));});
            field_root=event.parent_path().parent_path().parent_path();event_directory=event.parent_path();
            const auto database=readData(field_root/"FLDSET.XDB");
            const auto definition=okage::read_field_definition(database,room);
            current_scene=definition.scene;
            field->load_interactions(database,room,variables);
            field->load_exploration_camera(readData(stg));
            auto xcl=map;xcl.replace_extension(".xcl");field->load_exploration_collision(readData(xcl),variables);
        }
        std::cout<<"Native room: "<<renderer->placements()<<" placements; "<<renderer->omitted_effects()
            <<" effects omitted; "<<renderer->actors()<<" native actors; shadows unavailable; diagnostic dialogue panel\n";
    }
    void load_effects(const fs::path& data,const fs::path& textures) {
        if(!field||!renderer)throw std::runtime_error("Effects require a loaded scene");
        renderer->load_effect_textures(textures);field->load_effects(readData(data));effects_loaded=true;
        effect_data=data;effect_textures=textures;
    }
    void load_destination(const okage::FieldTransition& request) {
        auto definition=okage::read_field_definition(readData(field_root/"FLDSET.XDB"),request.scene);
        auto upper=[](std::string name){
            if(name.empty()||name.front()=='/'||name.find("..")!=std::string::npos||name.find('\\')!=std::string::npos)
                throw std::runtime_error("Unsafe field asset path");
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});return name;
        };
        const auto bank_name=upper(definition.message);
        if(bank_name!=message_bank) {
            // 0x131d28 loads the field database's bank into slot0. Retire all
            // borrowers before replacement; a later dialogue constructs its
            // own path against the new bank, with the same player-name binding.
            auto replacement=okage::read_pme(readData(message_directory/(bank_name+".PME")));
            if(!replacement.bind("############",player_name))throw std::runtime_error("Player-name tag missing from destination message bank");
            if(dialogue_active&&!closure.finished())throw std::runtime_error("Destination replaces an unfinished dialogue bank");
            dialogue_active=false;text=okage::DialogueText({});text.fonts=fonts;
            opening.reset();choices={};narration={};controller={};clock={};
            messages=std::move(replacement);message_bank=bank_name;
            std::cout<<"Native message bank: "<<message_bank<<"; "<<messages.strings.size()<<" strings; "<<messages.nodes.size()<<" nodes\n";
        }
        upper(definition.event);
        const auto script=readData(event_directory/(definition.event+".xmt"));
        definition=okage::prepare_field_entry(script,variables,std::move(definition),request.entrance);
        if(definition.type>1)throw std::runtime_error("Destination field type is unrecovered");
        if(!definition.followers.empty())throw std::runtime_error("Destination follower construction is unrecovered");
        if(!music)throw std::runtime_error("Destination music service is not bound");
        music->enter(definition.music,definition.secondary_music,[this](int id){return read_music_resource(id);});
        std::cout<<"Native destination setup: scene "<<request.scene<<"; party "<<definition.party
                 <<"; followers "<<definition.followers.size()<<"; music "<<music->current_id()
                 <<"; slots "<<music->loaded_id(0)<<","<<music->loaded_id(1)<<"\n";
        const auto map=field_root/upper(definition.room_directory)/(upper(definition.room)+".XPF")/"a.xmp";
        std::vector<okage::SceneActor> scene_actors;
        renderer.reset();field.reset();map_objects.reset();actor_bindings.clear();actor_objects.clear();
        auto add=[&](int alias,const std::string& model){
            const auto path=field_root/"OBJ"/(upper(model)+".XPF")/"a.xmd";
            auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            auto object=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),readData(motion),
                readActorTextures(xta));
            actor_bindings.push_back({alias,object.get()});scene_actors.push_back({object.get(),path});actor_objects.push_back(std::move(object));
        };
        for(const auto& npc:definition.npcs)add(npc.alias,npc.model);
        add(-1,"hero");
        map_objects=std::make_unique<okage::MapObjects>(map);
        renderer=std::make_unique<okage::SceneRenderer>(map,scene_actors,map_objects.get());
        auto stg=map,xcl=map;stg.replace_extension(".stg");xcl.replace_extension(".xcl");
        const auto stage=readData(stg);projection=okage::field_projection(stage,&far_clip);
        create_field(script,false);bind_audio_services();
        if(effects_loaded){renderer->load_effect_textures(effect_textures);field->load_effects(readData(effect_data));}
        if(definition.type==0) {
            if(!effects_loaded)throw std::runtime_error("Exterior requires owned effect resources");
            field->load_map_effects(readData(map),renderer->effect_texture_sizes());
            renderer->bind_map_effects(&field->map_effects());
            renderer->set_effect_light_tint(okage::field_effect_tint(stage));
        }
        dialogue_active=false;choices={};narration={};
        field->enter(std::move(definition),stage,readData(xcl),request.entrance);
        current_scene=request.scene;current_entrance=int(request.entrance);transition_target=0;
        std::cout<<"Native destination: scene "<<current_scene<<" entrance "<<current_entrance<<"; "<<renderer->actors()<<" actors\n";
    }
    void tick(okage::DialogueButtons input) {
        ++frame;
        input.repeat=pad_repeat.update(input.held,input.pressed);
        // 0x114ac0 advances the captured-frame overlay by four alpha units.
        if(transition_alpha!=transition_target) {
            transition_alpha=transition_alpha>transition_target?transition_alpha-4:transition_alpha+4;
            if(!transition_alpha)transition_image.reset();
        }
        scene_fade.tick();
        const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
        if(field) {
            field->tick({input.held,128,128,input.pressed});
            if(field->encounter())throw std::runtime_error("Native battle dispatch is unrecovered (encounter column "+
                std::to_string(field->encounter()->column)+")");
            const auto request=field->transition();
            transition_trace.insert(transition_trace.end(),{frame,unsigned(current_scene),unsigned(current_entrance),
                request?unsigned(request->scene):0xffffffffu,request?request->entrance:0,
                request?request->countdown:0,request&&request->ready?1u:0u,field->pc()});
            if(request&&request->ready)load_destination(*request);
        }
        if(music){
            std::array<std::int16_t,1600> pcm{};music->render(pcm);
            std::array<std::int32_t,1600> stream{};
            for(auto& [bank,player]:sound_banks)player->mix(stream);
            for(unsigned i=0;i<pcm.size();++i)pcm[i]=std::clamp(int(pcm[i])+stream[i],-32768,32767);
            if(audio_output)audio_output->submit(pcm);
            if(record_audio){audio_capture.insert(audio_capture.end(),pcm.begin(),pcm.end());
                music_trace.insert(music_trace.end(),{frame,unsigned(music->volume.current),unsigned(music->volume.target),
                    unsigned(music->volume.step),music->player()?music->player()->peak_voices():0});}
        }
        if(!field && !release_frame && okage::poll_dialogue_wait(wait,2,{true,0,layers,std::int32_t(text.state.progress)})==0)release_frame=frame;
        if(dialogue_active && !closure.finished()) {
            opacity.tick();closure.decoration.tick();
            if(opening&&controller.stage==0)opening->bootstrap(text,clock);
            else text.frame(clock,opacity.visible());
        }
        const bool opening_tick=dialogue_active&&opening&&controller.stage<6&&
            opening->update(text,clock,controller,opacity,closure);
        if(dialogue_active && !closure.finished() && controller.stage>=6)closure.update(controller.stage);
        else if(dialogue_active && !opening_tick && controller.stage==5) {
            okage::DialogueInputEffects effects;
            try {effects=controller.update(text,input);}
            catch(const std::exception& e) {
                throw std::runtime_error("Dialogue ID="+std::to_string(message_id)+" native frame="+
                    std::to_string(frame)+" command="+text.state.window+": "+e.what());
            }
            if(effects.accepted_choice>=0)choices.accepted=effects.accepted_choice;
            if(effects.reset_text_clock){clock.revealed=0;clock.scroll=0;clock.scroll_target=0;clock.scroll_step=clock.scroll_base_step;}
            if(effects.scene_fade)scene_fade.request(*effects.scene_fade);
            if(effects.close)opacity.set(128,-4);
            if(effects.cue>=0 || effects.stop_voice || effects.stop_effect)++service_request_count;
        }
        if(closure.finished() && !close_frame)close_frame=frame;
        choices.update(dialogue_active && !closure.finished() ? text.state.replacement : "",text.choice_cursor,controller.choice_armed);
        choice_trace.insert(choice_trace.end(),{frame,unsigned(message_id),unsigned(text.position().fragment),unsigned(text.position().cursor),
            unsigned(text.choice_cursor),unsigned(text.choice_result),controller.choice_active?1u:0u,controller.choice_armed?1u:0u,
            input.held,input.pressed,input.repeat,choices.alpha});
        narration.update(dialogue_active && !closure.finished() ? text.state.description : "",fonts[2]);
        dialogue_trace.insert(dialogue_trace.end(),{frame,std::uint32_t(message_id),text.state.flags,text.state.phase,
            text.state.period,text.state.delay,text.state.progress,controller.advance_count,controller.layer_advance,
            controller.layer_flags,controller.stage,__builtin_bit_cast(std::uint32_t, (opacity.delta))});
        if(field) {
            interaction_trace.insert(interaction_trace.end(),{frame,input.pressed,
                std::uint32_t(field->interaction_alias()),std::uint32_t(field->interaction_event()),
                field->event_running()?1u:0u,field->player_movement().mask,
                std::uint32_t(okage::event_variable(variables,0x40006)),field->pc()});
            const auto& m=field->player_movement();
            movement_trace.insert(movement_trace.end(),{frame,input.held,m.mask,m.footstep_sound,
                std::uint32_t(m.motion),std::uint32_t(m.floor_contact),m.trigger_kind,m.trigger_index,
                m.camera_area,m.upper_material,m.effect_counter,m.run_delay,m.footstep_clock,
                m.footstep_period,m.dust_effect,m.contact_bubble});
            camera_state_trace.insert(camera_state_trace.end(),{frame,field->camera_mode(),
                field->event_running()?1u:0u,__builtin_bit_cast(std::uint32_t, (field->blend.phase)),
                __builtin_bit_cast(std::uint32_t, (field->blend.step))});
            scene_trace.insert(scene_trace.end(),{frame,field->pc(),field->pending_opcode,release_frame});
            for(const auto& values:{field->position,field->target,field->rotation})
                for(float v:values)scene_trace.push_back(__builtin_bit_cast(std::uint32_t, (v)));
            if(effects_loaded) {
                const auto& jobs=field->map_effects();unsigned jobs_count=0;for(const auto& e:jobs)if(e)++jobs_count;
                map_effect_trace.insert(map_effect_trace.end(),{frame,jobs_count});
                for(unsigned i=0;i<jobs.size();++i)if(const auto& e=jobs[i]) {
                    map_effect_trace.insert(map_effect_trace.end(),{i,e->id,e->flags,e->phase,e->duration,unsigned(e->elements.size())});
                    for(const auto& v:{e->position,e->rotation,e->scale,e->color})for(float f:v)map_effect_trace.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                    for(const auto& element:e->elements) {
                        map_effect_trace.insert(map_effect_trace.end(),{element.id,element.program,element.texture,element.render_flags});
                        for(const auto& v:element.rendered)for(float f:v)map_effect_trace.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                    }
                }
                const auto& all=field->effects();unsigned count=0;for(const auto& e:all)if(e)++count;
                effect_trace.insert(effect_trace.end(),{frame,field->pc(),count});
                for(unsigned i=0;i<all.size();++i)if(const auto& e=all[i]) {
                    effect_trace.insert(effect_trace.end(),{i,unsigned(e->actor_alias),e->animation.phase,e->animation.flags});
                    for(float f:e->position)effect_trace.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                    for(const auto& s:e->animation.sprites) {
                        for(const auto& v:{s.position,s.scale})for(float f:v)effect_trace.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                        effect_trace.push_back(s.texture);effect_trace.push_back(s.uv[0]|(std::uint32_t(s.uv[1])<<16));effect_trace.push_back(s.uv[2]|(std::uint32_t(s.uv[3])<<16));
                    }
                }
            }
        }
        for(const auto& binding:actor_bindings) {
            const auto* actor=binding.actor;auto& actor_trace=actor_traces[binding.alias];
            actor_trace.insert(actor_trace.end(),{frame,field->pc(),actor->animation().pc(),actor->animation().script_state(),
                actor->motion(),std::uint32_t(actor->channels().size()),std::uint32_t(actor->textures().images().size()),actor->visible ? 1u : 0u});
            for(const auto& channel:actor->channels())for(float v:channel)actor_trace.push_back(__builtin_bit_cast(std::uint32_t, (v)));
            for(auto image:actor->textures().images())actor_trace.push_back(__builtin_bit_cast(std::uint32_t, (image)));
        }
        if(field) {
            const auto& filter=field->screen_filter;
            filter_trace.insert(filter_trace.end(),{frame,filter.updating?1u:0u,filter.setting,filter.alpha});
            if(filter.alpha&&(!render_from||frame<render_from))
                throw std::runtime_error("Active feedback needs consecutive rendering; use --render-from before activation");
        }
        if(render_from&&frame>=render_from)render_frame();
    }
    void render_frame() {
        if(rendered_surface&&rendered_frame==frame)return;
        auto image=surface();
        feedback.retain(image,frame,224);
        rendered_surface=std::move(image);rendered_frame=frame;
    }
    okage::Texture surface() const {
        if(rendered_surface&&rendered_frame==frame)return *rendered_surface;
        auto text_image=okage::rasterize_text(fonts,text.glyphs,640,224);
        if(!renderer)return text_image;
        const auto& r=field->rotation;
        auto image=renderer->render(field->view(),projection,effects_loaded?&field->effects():nullptr,{-r[0],-r[1],-r[2],0},far_clip,field->position);
        feedback.apply(image,frame,field->screen_filter.alpha);
        const auto brightness=128-scene_fade.alpha();
        for(std::size_t i=0;i<image.rgba.size();i+=4)for(unsigned c=0;c<3;++c)image.rgba[i+c]=unsigned(image.rgba[i+c])*brightness/128;
        if(choices.highlighted){
            const int top=(336+27*choices.cursor)/2,bottom=(363+27*choices.cursor)/2;
            for(int y=top;y<bottom;++y)for(int x=162;x<599;++x)for(unsigned c=0;c<3;++c){
                auto& pixel=image.rgba[(y*640+x)*4+c];pixel=(255u*48+unsigned(pixel)*80)/128;
            }
        }
        auto glyphs=narration.glyphs(fonts[2]);const auto choiceGlyphs=choices.glyphs(fonts[2]);glyphs.insert(glyphs.end(),choiceGlyphs.begin(),choiceGlyphs.end());
        const auto overlay=okage::rasterize_text(fonts,glyphs,640,224);
        for(std::size_t i=0;i<overlay.rgba.size();i+=4){
            const unsigned alpha=overlay.rgba[i+3];
            for(unsigned c=0;c<3;++c)image.rgba[i+c]=(unsigned(overlay.rgba[i+c])*alpha+unsigned(image.rgba[i+c])*(255-alpha)+127)/255;
        }
        image.height=448;image.rgba.resize(640*448*4);
        for(std::size_t i=0;i<text_image.rgba.size();i+=4) {
            const float a=dialogue_active && controller.window_type!=2 ? (text_image.rgba[i+3]/255.f)*std::clamp(opacity.value/128.f,0.f,1.f) : 0;
            for(unsigned c=0;c<3;++c)image.rgba[640*224*4+i+c]=std::uint8_t(text_image.rgba[i+c]*a+16.f*(1-a));
            image.rgba[640*224*4+i+3]=255;
        }
        if(transition_alpha) {
            if(!transition_image||transition_image->rgba.size()!=image.rgba.size())throw std::runtime_error("Missing transition framebuffer");
            for(std::size_t i=0;i<image.rgba.size();i+=4)for(unsigned c=0;c<3;++c)
                image.rgba[i+c]=(unsigned(transition_image->rgba[i+c])*transition_alpha+unsigned(image.rgba[i+c])*(128-transition_alpha))/128;
        }
        return image;
    }
};
static NSBitmapImageRep* bitmap(const okage::Texture& image) {
    NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image.width pixelsHigh:image.height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:image.width*4 bitsPerPixel:32];
    if(!rep)throw std::runtime_error("Cannot allocate native text bitmap");
    std::memcpy(rep.bitmapData,image.rgba.data(),image.rgba.size());return rep;
}
static void capture(const Session& session,const fs::path& path) {
    auto statePath=fs::path(path.string()+".json");
    if(fs::exists(path)||fs::is_symlink(path)||fs::exists(statePath)||fs::is_symlink(statePath))throw std::runtime_error("Refusing existing native capture");
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());
    const auto data=[bitmap(session.surface()) representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    std::ofstream out(path,std::ios::binary);if(!data || !out.write(static_cast<const char*>(data.bytes),data.length))throw std::runtime_error("Cannot write native capture");
    std::ofstream state(statePath);
    if(session.effects_loaded) {
        std::ofstream map_effects(path.string()+".map-effects.bin",std::ios::binary);
        if(!map_effects.write(reinterpret_cast<const char*>(session.map_effect_trace.data()),session.map_effect_trace.size()*4))throw std::runtime_error("Cannot write map effect trace");
        std::ofstream effects(path.string()+".effects.bin",std::ios::binary);
        if(!effects.write(reinterpret_cast<const char*>(session.effect_trace.data()),session.effect_trace.size()*4))throw std::runtime_error("Cannot write effect trace");
    }
    state<<"{\n  \"frames\": "<<session.frame<<",\n  \"message_id\": "<<session.message_id
         <<",\n  \"messages_started\": "<<session.messages_started<<",\n  \"text_progress\": "<<session.text.state.progress
         <<",\n  \"advance_count\": "<<session.controller.advance_count<<",\n  \"wait_release_frame\": "<<session.release_frame
         <<",\n  \"controller_stage\": "<<session.controller.stage<<",\n  \"unimplemented_service_requests\": "<<session.service_request_count
         <<",\n  \"message_flags\": "<<session.closure.message_flags
         <<",\n  \"window_type\": "<<session.controller.window_type
         <<",\n  \"scene_brightness\": "<<session.scene_fade.current
         <<",\n  \"narration_cues\": "<<session.sound_trace.size()/5
         <<",\n  \"progression_flag_2\": "<<okage::event_variable(session.variables,2)
         <<",\n  \"progression_flag_3\": "<<okage::event_variable(session.variables,3)
         <<",\n  \"kitchen_flag_7\": "<<okage::event_variable(session.variables,0x10007)
         <<",\n  \"progression_flag_4\": "<<okage::event_variable(session.variables,4)
         <<",\n  \"progression_flag_5\": "<<okage::event_variable(session.variables,5)
         <<",\n  \"field_event_running\": "<<(session.field&&session.field->event_running()?"true":"false")
         <<",\n  \"field_camera_mode\": "<<(session.field?session.field->camera_mode():0)
         <<",\n  \"interaction_alias\": "<<(session.field?session.field->interaction_alias():0)
         <<",\n  \"interaction_event\": "<<(session.field?session.field->interaction_event():0)
         <<",\n  \"loaded_stream_banks\": "<<session.sound_banks.size()
         <<",\n  \"choice_cursor\": "<<session.text.choice_cursor
         <<",\n  \"choice_result\": "<<session.text.choice_result
         <<",\n  \"choice_armed\": "<<(session.controller.choice_armed ? "true" : "false")
         <<",\n  \"close_frame\": "<<session.close_frame<<",\n  \"close_wait_release_frame\": "<<session.close_release_frame<<"\n}\n";
    if(!state)throw std::runtime_error("Cannot write native capture state");
    for(const auto& [suffix,data]:std::array<std::pair<std::string,okage::Bytes>,3>{
        std::pair{".screen-filter.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.filter_trace.data()),session.filter_trace.size()*4)},
        std::pair{".transition.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.transition_trace.data()),session.transition_trace.size()*4)},
        std::pair{".variables.bin",okage::Bytes(session.variables)}}) {
        const auto output=fs::path(path.string()+suffix);
        if(fs::exists(output)||fs::is_symlink(output))throw std::runtime_error("Refusing existing field-state capture");
        std::ofstream stream(output,std::ios::binary);
        if(!stream.write(reinterpret_cast<const char*>(data.data()),data.size()))throw std::runtime_error("Cannot write field-state capture");
    }

    if(session.music){
        const auto pcmPath=path.string()+".audio.pcm",tracePath=path.string()+".music.bin",statsPath=path.string()+".audio.json",soundPath=path.string()+".sound.bin";
        for(const auto& p:{pcmPath,tracePath,statsPath,soundPath})if(fs::exists(p)||fs::is_symlink(p))throw std::runtime_error("Refusing existing audio capture");
        std::ofstream pcm(pcmPath,std::ios::binary),trace(tracePath,std::ios::binary),stats(statsPath),sound(soundPath,std::ios::binary);
        pcm.write(reinterpret_cast<const char*>(session.audio_capture.data()),session.audio_capture.size()*2);
        trace.write(reinterpret_cast<const char*>(session.music_trace.data()),session.music_trace.size()*4);
        sound.write(reinterpret_cast<const char*>(session.sound_trace.data()),session.sound_trace.size()*4);
        stats<<"{\n  \"dry_output\": true,\n  \"frames\": "<<session.audio_capture.size()/2<<",\n  \"rate\": 48000,\n  \"volume\": "<<session.music->volume.level()
             <<",\n  \"peak_voices\": "<<(session.music->player()?session.music->player()->peak_voices():0)<<",\n  \"device_frames_enqueued\": "<<(session.audio_output ? session.audio_output->delivered_frames() : 0)
             <<",\n  \"device_underrun_frames\": "<<(session.audio_output ? session.audio_output->underrun_frames() : 0)<<"\n}\n";
        if(!pcm || !trace || !stats || !sound)throw std::runtime_error("Cannot write native audio capture");
    }

    const auto choicePath=fs::path(path.string()+".choices.bin");
    if(fs::exists(choicePath)||fs::is_symlink(choicePath))throw std::runtime_error("Refusing existing choice trace");
    std::ofstream choiceOut(choicePath,std::ios::binary);
    if(!choiceOut.write(reinterpret_cast<const char*>(session.choice_trace.data()),session.choice_trace.size()*4))throw std::runtime_error("Cannot save choice trace");
    const auto dialoguePath=fs::path(path.string()+".dialogue.bin");
    if(fs::exists(dialoguePath)||fs::is_symlink(dialoguePath))throw std::runtime_error("Refusing existing dialogue trace");
    std::ofstream dialogueOut(dialoguePath,std::ios::binary);
    if(!dialogueOut.write(reinterpret_cast<const char*>(session.dialogue_trace.data()),session.dialogue_trace.size()*4))throw std::runtime_error("Cannot save dialogue trace");
    for(const auto& [alias,trace]:session.actor_traces) {
        const auto actorPath=fs::path(path.string()+(alias==-8 ? ".actor" : ".actor"+std::to_string(alias))+".bin");
        if(fs::exists(actorPath)||fs::is_symlink(actorPath))throw std::runtime_error("Refusing existing actor trace");
        std::ofstream out(actorPath,std::ios::binary);
        if(!out.write(reinterpret_cast<const char*>(trace.data()),trace.size()*4))throw std::runtime_error("Cannot save actor trace");
    }
    if(session.field) {
        const auto interactionPath=fs::path(path.string()+".interaction.bin");
        if(fs::exists(interactionPath)||fs::is_symlink(interactionPath))throw std::runtime_error("Refusing existing interaction trace");
        std::ofstream interaction(interactionPath,std::ios::binary);
        if(!interaction.write(reinterpret_cast<const char*>(session.interaction_trace.data()),session.interaction_trace.size()*4))
            throw std::runtime_error("Cannot save interaction trace");
        const auto tracePath=fs::path(path.string()+".camera.bin");
        if(fs::exists(tracePath)||fs::is_symlink(tracePath))throw std::runtime_error("Refusing existing camera trace");
        std::ofstream trace(tracePath,std::ios::binary);
        if(!trace.write(reinterpret_cast<const char*>(session.scene_trace.data()),session.scene_trace.size()*4))throw std::runtime_error("Cannot save native camera trace");
        const auto statePath=fs::path(path.string()+".camera-state.bin");
        if(fs::exists(statePath)||fs::is_symlink(statePath))throw std::runtime_error("Refusing existing camera-state trace");
        std::ofstream stateTrace(statePath,std::ios::binary);
        if(!stateTrace.write(reinterpret_cast<const char*>(session.camera_state_trace.data()),session.camera_state_trace.size()*4))
            throw std::runtime_error("Cannot save native camera-state trace");
        const auto movementPath=fs::path(path.string()+".movement.bin");
        if(fs::exists(movementPath)||fs::is_symlink(movementPath))throw std::runtime_error("Refusing existing movement trace");
        std::ofstream movementTrace(movementPath,std::ios::binary);
        if(!movementTrace.write(reinterpret_cast<const char*>(session.movement_trace.data()),session.movement_trace.size()*4))
            throw std::runtime_error("Cannot save native movement trace");
        std::cout<<"Field PC=0x"<<std::hex<<session.field->pc()<<" pending opcode=0x"<<session.field->pending_opcode<<std::dec
            <<" actor="<<session.field->pending_actor<<" animation="<<session.field->pending_animation<<'\n';
    }
}

@interface NativeTextView : NSView
@property(strong) NSImage* image;
@property float opacity;
@property unsigned short held;
@property unsigned short pressed;
@property unsigned injectedKeyDowns;
@end
@implementation NativeTextView
- (BOOL)acceptsFirstResponder {return YES;}
- (BOOL)isFlipped {return YES;}
- (void)keyDown:(NSEvent*)event {
    if(event.isARepeat)return;
    const unsigned mask=(event.keyCode==36 || event.keyCode==49)?0x40:event.keyCode==126?0x1000:event.keyCode==125?0x4000:event.keyCode==123?0x8000:event.keyCode==124?0x2000:0;
    if(mask){self.pressed|=mask;self.held|=mask;++self.injectedKeyDowns;}
    else [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    const unsigned mask=(event.keyCode==36 || event.keyCode==49)?0x40:event.keyCode==126?0x1000:event.keyCode==125?0x4000:event.keyCode==123?0x8000:event.keyCode==124?0x2000:0;
    if(mask)self.held&=~mask;
    else [super keyUp:event];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;[[NSColor colorWithCalibratedRed:.04 green:.06 blue:.09 alpha:1] setFill];NSRectFill(self.bounds);
    [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
    [self.image drawInRect:self.bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
        fraction:std::clamp(self.opacity/128.f,0.f,1.f) respectFlipped:YES hints:nil];
}
@end
@interface NativeApp : NSObject<NSApplicationDelegate,NSWindowDelegate> {
@public std::unique_ptr<Session> session;
@public unsigned testFrames;
@public int failureStatus;
@public std::vector<unsigned> testPresses;
@public DirectionReplay directions;
@public std::uint16_t replayDirections;
@public fs::path testCapture;
}
@property(strong) NSWindow* window;
@property(strong) NativeTextView* view;
@property(strong) NSTimer* timer;
@property double previousTime;
@property double accumulator;
@end
@implementation NativeApp
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,session->field ? 672 : 336)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    self.window.delegate=self;
    if(session->music)session->audio_output=std::make_unique<okage::AudioOutput>();
    self.view=[[NativeTextView alloc] initWithFrame:self.window.contentView.bounds];self.view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    self.window.contentView=self.view;[self.window center];[self.window makeKeyAndOrderFront:nil];[self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];self.previousTime=NSProcessInfo.processInfo.systemUptime;
    self.timer=[NSTimer scheduledTimerWithTimeInterval:1./120 target:self selector:@selector(update:) userInfo:nil repeats:YES];
}
- (void)update:(NSTimer*)timer {
    (void)timer;
    try {
        const double now=NSProcessInfo.processInfo.systemUptime;
        self.accumulator+=std::min(.25,now-self.previousTime);self.previousTime=now;
        bool changed=false;
        while(self.accumulator>=1./60) {
            if(testFrames){
                const auto next=directions.held(session->frame+1);
                for(const auto [mask,code]:{std::pair<unsigned,unsigned>{0x1000,126},{0x4000,125},{0x8000,123},{0x2000,124}}){
                    if((next^replayDirections)&mask){
                        const auto type=(next&mask)?NSEventTypeKeyDown:NSEventTypeKeyUp;
                        NSString* chars=[NSString stringWithFormat:@"%C",unichar(code==126?NSUpArrowFunctionKey:code==125?NSDownArrowFunctionKey:code==123?NSLeftArrowFunctionKey:NSRightArrowFunctionKey)];
                        NSEvent* event=[NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:now
                            windowNumber:self.window.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
                        [self.window sendEvent:event];
                    }
                }
                replayDirections=next;
            }
            const bool press=testFrames && std::find(testPresses.begin(),testPresses.end(),session->frame+1)!=testPresses.end();
            if(press) {
                NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:now
                    windowNumber:self.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
                [self.window sendEvent:event];
            }
            session->tick({self.view.held,self.view.pressed});self.view.pressed=0;
            if(press) {
                NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:now
                    windowNumber:self.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
                [self.window sendEvent:event];
            }
            self.accumulator-=1./60;changed=true;
            if(testFrames && session->frame==testFrames)break;
        }
        if(!changed)return;
        NSImage* image=[[NSImage alloc] initWithSize:NSMakeSize(640,session->field ? 448 : 224)];[image addRepresentation:bitmap(session->surface())];
        self.view.image=image;self.view.opacity=session->field ? 128 : session->opacity.value;self.view.needsDisplay=YES;
        self.window.title=[NSString stringWithFormat:@"Okage native field — Arrow keys, Return/Space — progress %u — event %@",
            session->text.state.progress,session->release_frame ? @"released" : @"waiting"];
        if(session->field && session->field->pending_opcode)
            self.window.title=[self.window.title stringByAppendingString:@" — service pending (partial scene)"];
        if(testFrames && session->frame==testFrames) {
            [self.timer invalidate];capture(*session,testCapture);
            [self.view display];
            NSBitmapImageRep* rep=[self.view bitmapImageRepForCachingDisplayInRect:self.view.bounds];
            [self.view cacheDisplayInRect:self.view.bounds toBitmapImageRep:rep];
            NSData* png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            const auto path=testCapture.string()+".view.png";
            if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing native view capture");
            std::ofstream out(path,std::ios::binary);
            if(!png || !out.write(static_cast<const char*>(png.bytes),png.length))throw std::runtime_error("Cannot save native view");
            std::cout<<"AppKit keyDown events="<<self.view.injectedKeyDowns<<" native progress="<<session->text.state.progress<<" wait_release="<<session->release_frame<<std::endl;
            [NSApp terminate:nil];
        }
    }catch(const std::exception& e) {
        [self.timer invalidate];self.window.title=[NSString stringWithUTF8String:e.what()];std::cerr<<e.what()<<'\n';
        if(testFrames){failureStatus=1;[NSApp stop:nil];}
    }
}
- (void)windowDidResignKey:(NSNotification*)notification {(void)notification;self.view.held=0;self.view.pressed=0;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {(void)app;return YES;}
@end
int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc<3)throw std::runtime_error("Usage: okage-native FONT_DIRECTORY FIELD.PME [--frames N | --ui-test-frames N] [--press FRAME,FRAME --directions INPUT --capture FILE.png --render-from FRAME] [--scene MAP.xmp --event EVENT.xmt --actor ACTOR.xmd --actor-bind ALIAS=MODEL.xmd --player-name NAME] [--music-dry SEQUENCE.xsq --music-bank BANK.xv --sound-dir SOUND_DIRECTORY] [--effect-data TES.EFD --effect-textures ENV_TEX_DIRECTORY]");
        unsigned frames=0,uiFrames=0,renderFrom=0;DirectionReplay directions;std::vector<unsigned> presses;fs::path output,map,event,musicSequence,musicBank,soundDirectory,effectData,effectTextures;std::vector<std::pair<int,fs::path>> actors;std::string playerName="Ari";
        for(int i=3;i<argc;++i) {
            const std::string arg=argv[i];if(++i==argc)throw std::runtime_error("Missing native runtime option value");
            if(arg=="--frames")frames=std::stoul(argv[i]);
            else if(arg=="--render-from")renderFrom=std::stoul(argv[i]);
            else if(arg=="--ui-test-frames")uiFrames=std::stoul(argv[i]);
            else if(arg=="--directions")directions.load(argv[i]);
            else if(arg=="--capture")output=argv[i];
            else if(arg=="--scene")map=argv[i];
            else if(arg=="--event")event=argv[i];
            else if(arg=="--music-dry")musicSequence=argv[i];
            else if(arg=="--music-bank")musicBank=argv[i];
            else if(arg=="--sound-dir")soundDirectory=argv[i];
            else if(arg=="--effect-data")effectData=argv[i];
            else if(arg=="--effect-textures")effectTextures=argv[i];
            else if(arg=="--actor")actors.push_back({-8,argv[i]});
            else if(arg=="--actor-bind") {
                const std::string value=argv[i];const auto eq=value.find('=');std::size_t consumed=0;
                if(eq==value.npos || eq+1==value.size())throw std::runtime_error("Actor binding requires ALIAS=MODEL.xmd");
                const auto alias=std::stoi(value.substr(0,eq),&consumed);
                if(consumed!=eq)throw std::runtime_error("Invalid actor alias");
                actors.push_back({alias,value.substr(eq+1)});
            }
            else if(arg=="--player-name")playerName=argv[i];
            else if(arg=="--press") {std::istringstream in(argv[i]);std::string n;while(std::getline(in,n,','))presses.push_back(std::stoul(n));}
            else throw std::runtime_error("Unknown native runtime option");
        }
        if(musicSequence.empty()!=musicBank.empty())throw std::runtime_error("Music requires both sequence and bank");
        if(!soundDirectory.empty() && musicSequence.empty())throw std::runtime_error("Sound directory requires native music setup");
        if(effectData.empty()!=effectTextures.empty()||(!effectData.empty()&&map.empty()))throw std::runtime_error("Effects require data, texture directory and scene");
        if(map.empty()!=event.empty() || (!actors.empty() && map.empty()))throw std::runtime_error("Scene requires both map and event assets");
        if(frames>36000 || uiFrames>36000 || (frames && uiFrames) || (uiFrames && output.empty()))throw std::runtime_error("Native diagnostic frame/capture bounds");
        if(renderFrom&&(!frames||renderFrom>frames))throw std::runtime_error("Render start requires a headless frame within the capture");
        if(frames) {
            Session session(argv[1],argv[2],playerName);session.render_from=renderFrom;if(!map.empty())session.load_scene(map,event,actors);
            if(!musicSequence.empty())session.load_music(musicSequence,musicBank,!output.empty(),soundDirectory);
            if(!effectData.empty())session.load_effects(effectData,effectTextures);
            std::uint16_t previous=0;
            for(unsigned i=1;i<=frames;++i) {
                const std::uint16_t mask=std::find(presses.begin(),presses.end(),i)!=presses.end()?0x40:0;
                const auto held=directions.held(i);session.tick({std::uint16_t(mask|held),std::uint16_t(mask|(held&~previous))});previous=held;
            }
            if(!output.empty())capture(session,output);
            std::cout<<"Native frames="<<session.frame<<" progress="<<session.text.state.progress<<" wait_release="<<session.release_frame<<'\n';return 0;
        }
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NativeApp* app=[NativeApp new];app->session=std::make_unique<Session>(argv[1],argv[2],playerName);if(!map.empty())app->session->load_scene(map,event,actors);
        if(!musicSequence.empty())app->session->load_music(musicSequence,musicBank,!output.empty(),soundDirectory);
        if(!effectData.empty())app->session->load_effects(effectData,effectTextures);
        app->session->render_from=1;app->testFrames=uiFrames;app->testPresses=presses;app->directions=directions;app->testCapture=output;
        [NSApp setDelegate:app];[NSApp run];return app->failureStatus;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
