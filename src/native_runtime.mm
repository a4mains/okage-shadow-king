#import <AppKit/AppKit.h>
#include "okage/input_log.hpp"
#include "okage/dialogue.hpp"
#include "okage/event.hpp"
#include "okage/game_state.hpp"
#include "okage/battle_ui.hpp"
#include "okage/game_mode.hpp"
#include "okage/scene_resource.hpp"
#include "battle_session.hpp"
#include "display_link.hpp"
#include "game_over_session.hpp"
#include "title_session.hpp"
#include "name_entry_session.hpp"
#include "restore_session.hpp"
#include "chapter_session.hpp"
#include "startup_session.hpp"
#include "okage/title_stream.hpp"
#include "results_renderer.hpp"
#include "okage/results_interface.hpp"
#include "okage/load_queue.hpp"
#include "okage/field.hpp"
#include "okage/music.hpp"
#include "okage/audio_output.hpp"
#include "okage/stream_bank.hpp"
#include "okage/scene_renderer.hpp"
#include "okage/map_objects.hpp"
#include "okage/live_performance.hpp"
#include "okage/native_run.hpp"
#include "okage/live_scheduler.hpp"
#include "okage/input_journal.hpp"
#include "okage/headless_control.hpp"
#include "okage/frame_composition.hpp"
#include "battle_capture_metal.hpp"
#include "battle_filter_metal.hpp"
#include "shadow_metal.hpp"
#include "ui_metal.hpp"
#include "okage/battle_entry.hpp"
#include "okage/startup_textures.hpp"
#include "okage/startup_inputs.hpp"
#include <bit>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <set>
#include <sstream>

namespace fs=std::filesystem;
static constexpr unsigned diagnosticFrameLimit=120000;
static std::vector<std::uint8_t> readData(const fs::path& path) {
    const auto n=fs::file_size(path);if(n>128*1024*1024)throw std::runtime_error("Runtime input limit");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read runtime input");
    okage::record_input_read(path,b);return b;
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
    void load(const fs::path& path,unsigned allowed_mask=0xf000u){
        std::ifstream in(path);if(!in)throw std::runtime_error("Cannot read direction replay");
        std::string line;unsigned previous=0;
        while(std::getline(in,line)){
            if(line.empty() || line[0]=='#')continue;
            std::istringstream row(line);unsigned frame,mask;std::string extra;
            if(!(row>>frame>>std::hex>>mask) || (row>>extra) || frame<=previous || frame>diagnosticFrameLimit || (mask&~allowed_mask))
                throw std::runtime_error("Input replay requires increasing FRAME and an allowed hexadecimal button mask");
            entries.push_back({frame,std::uint16_t(mask)});previous=frame;
        }
    }
    std::uint16_t held(unsigned frame) const {
        const auto found=std::upper_bound(entries.begin(),entries.end(),frame,[](unsigned f,const auto& entry){return f<entry.first;});
        return found==entries.begin()?0:std::prev(found)->second;
    }
};
struct FieldReturnLoad {
    explicit FieldReturnLoad(bool observe):jobs(observe?65536:0) {}
    unsigned phase=0x12,ui_stage=0,complete=0;
    std::atomic<unsigned> counter{0};
    bool resumed=false,world_queued=false,world_prepared=false;
    bool ordinary=false;
    unsigned entrance=0;
    std::optional<okage::ResultsUiData> definitions;
    std::optional<okage::ResultsTextData> text;
    std::optional<okage::BattleHudData> numbers;
    std::shared_ptr<okage::RuwResource> ruw;
    std::shared_ptr<const okage::Texture> atlas;
    std::unique_ptr<okage::ResultsRenderer> drawing;
    std::unique_ptr<okage::ResultsInterface> ui;
    std::optional<okage::UiDrawList> draw;
    okage::FieldDefinition definition;
    std::vector<std::uint8_t> script,stage,collision;
    fs::path map;
    std::vector<std::unique_ptr<okage::FieldActor>> actors;
    std::vector<okage::ActorBinding> bindings;
    std::unique_ptr<okage::MapObjects> objects;
    std::unique_ptr<okage::OpeningField> prepared_field;
    std::unique_ptr<okage::SceneRenderer> renderer;
    okage::StageSettings settings;
    const okage::Texture* rendered=nullptr;
    // Last: joins the worker before any captured resource above is destroyed.
    okage::NativeLoadQueue jobs;
};
struct Session {
    enum class LoadingPolicy {Continuous,LiveWindows,ReplayWindows};
    LoadingPolicy loading_policy=LoadingPolicy::Continuous;
    std::unique_ptr<okage::DisplayLink> display_link;
    std::optional<std::uint64_t> pending_display_sequence;
    struct DisplayGateObservation {unsigned frame;std::uint64_t baseline,completed;};
    std::vector<DisplayGateObservation> display_gates;
    unsigned replay_presentations=0,replay_delayed_presentations=0;
    bool deferred_battle_readback=false;
    struct PendingBattleFrame {
        std::shared_ptr<okage::BattleFrameCompletion> completion;
        std::shared_ptr<const okage::FrameComposition> composition;
    };
    std::optional<PendingBattleFrame> pending_battle;
    bool prepared_tick=false,prepared_render_tick=false;
    struct FrameEvent {unsigned frame;const char* kind;unsigned image;};
    std::vector<FrameEvent> frame_events;
    void frame_event(const char* kind,unsigned image=0) {
        if(load_observation_trace)frame_events.push_back({frame,kind,image});
    }
    std::array<okage::Font,3> fonts;
    // Immutable native font owner retained by queued UI packets across frames.
    std::shared_ptr<const std::vector<okage::Font>> ui_fonts;
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
    okage::UiPartySelection ui_party;
    okage::BattleUiShared ui_shared;
    okage::BattleUiPad battle_pad;
    std::uint16_t previous_held=0;
    okage::GameRandom random{1};
    okage::CollisionMask collision_mask;
    okage::ExteriorCamera exterior_camera;
    okage::DialogueWaitState wait{5,1};
    unsigned frame=0,release_frame=0,close_frame=0,close_release_frame=0;
    unsigned render_from=0,render_until=0,rendered_frame=0;
    std::vector<okage::NativeRenderRange> render_ranges;
    okage::FrameFeedback feedback;
    std::optional<okage::Texture> rendered_surface;
    std::vector<std::uint32_t> filter_trace;
    std::vector<std::uint32_t> wander_trace;
    std::unique_ptr<okage::MusicRouter> music;
    std::unique_ptr<okage::StartupTextureOwner> startup_textures;
    std::shared_ptr<const okage::StartupInputs> startup_inputs;
    std::shared_ptr<const okage::SceneRenderer::EffectTextureBytes> verified_effect_textures;
    std::string startup_textures_status="not initialized (no cold startup)";
    fs::path startup_texture_report,resource_report,retirement_report;
    unsigned startup_selector=0;
    unsigned title_updates=0;bool first_title=false;
    std::vector<std::string> title_update_log;
    std::optional<unsigned> next_first_title_update() const {
        return first_title&&title&&title->resource_live()?std::optional<unsigned>(title_updates):std::nullopt;
    }
    std::vector<std::pair<std::string,unsigned>> title_checkpoint_frames;
    void title_checkpoint(const char* name) {
        startup_textures->note_checkpoint(name);
        title_checkpoint_frames.emplace_back(name,frame);
        if(!retirement_report.empty())
            okage::write_title_checkpoint_report(*startup_textures,retirement_report/name,name,random.state);
    }
    bool title_modeled(okage::ResourceStage stage) const {return startup_textures&&startup_textures->stage()==stage;}
    void write_title_route() {
        if(retirement_report.empty())return;
        std::ofstream out(retirement_report/"route.json",std::ios::binary);
        const auto mode=startup_textures->successor_mode();
        out<<"{\"route\":\""<<(mode==10?"restore":"newgame")<<"\",\"elf_sha256\":\""<<okage::startup_input_elf_sha256()
           <<"\",\"checkpoints\":{";
        for(std::size_t i=0;i<title_checkpoint_frames.size();++i)
            out<<(i?",":"")<<"\""<<title_checkpoint_frames[i].first<<"\":"<<title_checkpoint_frames[i].second;
        out<<"}}\n";
        if(!out)throw std::runtime_error("Title route report write failed");
        std::ofstream updates(retirement_report/"title-updates.jsonl",std::ios::binary);
        for(const auto& line:title_update_log)updates<<line<<'\n';
        if(!updates)throw std::runtime_error("Title update log write failed");
        //Native-only observations at the stop (not part of the original comparison): pending tails.
        std::ofstream tails(retirement_report/"native-tails.json",std::ios::binary);
        const auto stream=title_stream.state();
        unsigned pending_effects=0;
        if(retiring_effects)for(auto f:retiring_effects->flags())pending_effects+=f!=0;
        tails<<"{\"session_rng_state\":"<<random.state<<",\"retiring_effect_slots\":"<<pending_effects
             <<",\"theme_registered\":"<<(stream.registered?"true":"false")<<",\"theme_stop_pending\":"
             <<(stream.stop_pending?"true":"false")<<",\"theme_stop_countdown\":"<<stream.stop_countdown<<"}\n";
        if(!tails)throw std::runtime_error("Title tails report write failed");
    }
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
        for(int candidate:{10,24,26,51})if(sequence.stem()==okage::music_resource_names(candidate).sequence &&
                                      bank.stem()==okage::music_resource_names(candidate).primary_bank)id=candidate;
        fs::path alternate;
        if(id>0) {
            const auto names=okage::music_resource_names(id);
            if(*names.alternate)alternate=music_directory/(std::string(names.alternate)+".XSQ");
        }
        music->install(0,id,read_music_resource(sequence,bank,alternate));music->select(id);
        for(int jingle:{20,21,22,23,33,34})
            music->install_jingle(*okage::music_jingle_index(jingle),read_music_resource(jingle));
        record_audio=record;sound_directory=sounds;
        if(!sound_directory.empty())sound_banks.emplace(0,std::make_unique<okage::StreamPlayer>(
            okage::read_stream_bank(readData(sound_directory/"ALL.XWS"))));
        bind_audio_services();
        std::cout<<"Native dry music enabled: original sequence/banks; reverb pending\n";
    }

    void bind_audio_services() {
        if(field)field->audio_service=[this](const okage::FieldAudioRequest& request){
            const auto operation=request.operation;
            const auto argument=request.argument;
            if(operation==0 && music){
                if(request.primary_music)music->select(*request.primary_music);
                if(argument==0)return true;
                wait.mode=15;wait.active=0;
                music->load_secondary(argument,[this](int id){return read_music_resource(id);});
                // Original0x12fec8 signals field+0x30 after both sample banks
                // load. Native file loading is synchronous; its completion is
                // not a fitted reproduction of the original I/O frame count.
                wait.mode=5;wait.active=1;
                std::cout<<"Native event music loaded: slot1 score "<<argument<<"\n";return true;
            }
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
            if((operation!=3 && operation!=4 && operation!=5) || argument<0)return false;
            if(!music)return false;
            //0x1308d8 mode3 requests0x12d778(0,B); the normal driver
            // callback ramps and stops the active score on reaching zero.
            music->volume.request(operation==3 ? 0 : operation==4 ? 128 : 80,unsigned(argument));return true;
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
            sound_trace.insert(sound_trace.end(),{frame,bank,unsigned(index),std::bit_cast<unsigned>(speed),volume});return true;
        };
        if(field)field->stop_sound_cues=[this] {
            if(title_stream.state().registered)
                throw std::runtime_error("Field global stream stop overlaps an unrecovered live title stream");
            unsigned active=0;
            for(auto& [bank,player]:sound_banks){(void)bank;active+=player->active();player->stop();}
            std::cout<<"Native streamed cues stopped frame="<<frame<<" voices="<<active
                     <<" registered_banks="<<sound_banks.size()<<'\n';
            return true;
        };
    }

    bool dialogue_active=true;
    std::vector<std::unique_ptr<okage::FieldActor>> actor_objects;
    std::vector<okage::ActorBinding> actor_bindings;
    std::array<int,128> instance_aliases{};
    int next_instance=0,instance_limit=640;
    std::unique_ptr<okage::MapObjects> map_objects;
    std::unique_ptr<okage::OpeningField> field;
    //Field resource ownership survives destruction of the temporary loader.
    std::shared_ptr<okage::RuwResource> field_ruw;
    std::shared_ptr<const okage::Texture> field_ui_atlas;
    okage::GamePlayTimeCounter playtime;
    std::unique_ptr<okage::SceneRenderer> renderer;
    okage::FieldProjection projection;
    okage::SceneClearColor scene_clear;
    std::optional<okage::RetiringSceneEffects> retiring_effects;
    std::unique_ptr<okage::Animation> reset_camera;
    std::vector<std::uint32_t> scene_trace,dialogue_trace,camera_state_trace,movement_trace,interaction_trace;
    std::vector<std::uint32_t> effect_trace,map_effect_trace,effect_submission_trace,shadow_trace,projection_trace;
    bool effects_loaded=false;
    std::shared_ptr<const okage::EffectTemplatePreparation> effect_preparation;
    std::vector<std::uint8_t> effect_bytes;
    std::map<int,std::vector<std::uint32_t>> actor_traces;
    int message_id=10100;unsigned messages_started=0;
    std::optional<okage::DialogueOpening> opening;
    std::uint64_t service_request_count=0;
    Session(const fs::path& directory,const fs::path& pme,const std::string& name,bool diagnostic=true):
        fonts{okage::read_fnt(readData(directory/"IQ24.FNT")),okage::read_fnt(readData(directory/"BMUI.FNT")),okage::read_fnt(readData(directory/"BM.FNT"))},
        message_directory(pme.parent_path()),
        message_bank(pme.stem().string()),player_name(name),text({}) {
        ui_fonts=std::make_shared<const std::vector<okage::Font>>(fonts.begin(),fonts.end());
        okage::initialize_game_variables(variables);
        if(!diagnostic) {
            text.fonts=fonts;dialogue_active=false;message_id=0;
            return;
        }
        messages=okage::read_pme(readData(pme));
        if(player_name.empty() || player_name.size()>12 || std::any_of(player_name.begin(),player_name.end(),[](unsigned char c){return c<32 || c>=127 || c=='\\';}))
            throw std::runtime_error("Diagnostic player name requires 1..12 printable ASCII characters without text escapes");
        if(!messages.bind("############",player_name))throw std::runtime_error("Player-name tag missing from message bank");
        text=okage::DialogueText(messages,10100);
        text.fonts=fonts;text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        clock.glyph_flags=0x10;opacity.set(1,8);
        scene_fade.current=scene_fade.target=1; // Already lit diagnostic entry.
    }
    fs::path field_root,event_directory,effect_data,effect_textures;
    std::optional<okage::GameDataTables> game_data;
    int current_scene=-1,current_entrance=0;
    std::vector<std::uint32_t> transition_trace;
    std::vector<std::uint32_t> battle_transition_trace;
    std::vector<std::uint32_t> battle_capture_trace;
    std::optional<okage::Texture> completed_game_image,battle_capture_image;
    unsigned completed_game_frame=0,battle_capture_frame=0;
    bool battle_followers_saved=false;
    unsigned battle_capture_alpha=0,battle_capture_target=0;
    std::optional<okage::BattleCaptureLayer> battle_capture_layer;
    std::optional<okage::BattleCaptureDraw> battle_capture_draw;
    std::shared_ptr<okage::BattleCaptureSurface> battle_capture_surface;
    okage::SineTable battle_capture_sine=okage::make_sine_table();
    std::optional<okage::FieldBattleReturnSnapshot> battle_return;
    okage::BattleEntryContext battle_entry;
    std::array<std::uint32_t,3> pending_game_command{};
    std::optional<okage::GameModeParent> game_mode;
    std::unique_ptr<okage::BattleSession> battle;
    std::unique_ptr<okage::DefeatFieldReturn> defeat_return;
    std::unique_ptr<okage::GameOverSession> game_over;
    std::unique_ptr<okage::TitleSession> title;
    std::unique_ptr<okage::StartupSession> startup;
    fs::path save_directory;
    bool startup_audio_initialized=false;
    okage::NameEntryShared name_entry_shared;
    std::unique_ptr<okage::NameEntrySession> name_entry;
    okage::RestoreShared restore_shared;
    std::unique_ptr<okage::RestoreSession> restore,field_save;
    std::optional<okage::Texture> save_background;
    std::unique_ptr<okage::BattleCaptureSurface> save_capture_surface;
    unsigned save_open_frame=0;
    std::unique_ptr<okage::ChapterSession> chapter;
    okage::TitleStreamOwner title_stream;
    okage::GameOverShared game_over_shared;
    okage::ResultsUiShared results_shared;
    std::unique_ptr<FieldReturnLoad> field_return;
    fs::path battle_input;
    fs::path battle_construction_report;
    fs::path battle_stage_report;
    fs::path battle_ui_update_report;
    std::ostream* battle_live_trace=nullptr;
    std::ostream* load_observation_trace=nullptr;
    okage::InputJournal* input_journal=nullptr;
    bool load_observation_written=false;
    void write_load_observation(const char* boundary) {
        if(!load_observation_trace||(load_observation_written&&std::string_view(boundary)=="constructed"))return;
        auto& out=*load_observation_trace;
        out<<"{\"frame\":"<<frame<<",\"boundary\":\""<<boundary
           <<"\",\"loading_policy\":\""<<(loading_policy==LoadingPolicy::Continuous?"accelerated_continuous":
               loading_policy==LoadingPolicy::LiveWindows?"live_host_windows":"replay_host_windows")
           <<"\",\"battle_readback\":\""<<(deferred_battle_readback?"deferred":"synchronous")
           <<"\",\"replay_presentations\":"<<replay_presentations
           <<",\"replay_delayed_presentations\":"<<replay_delayed_presentations
           <<",\"queue\":";
        if(battle)battle->write_load_observation(out);else out<<"null";
        out<<",\"display\":";
        if(display_link) {
            const auto sample=display_link->snapshot();
            out<<"{\"sequence\":"<<sample.sequence<<",\"display_id\":"<<sample.display_id
               <<",\"nominal_time_value\":"<<sample.nominal_time_value
               <<",\"nominal_time_scale\":"<<sample.nominal_time_scale
               <<",\"host_clock_frequency\":"<<sample.host_clock_frequency
               <<",\"output_host_time\":"<<sample.output.host_time
               <<",\"output_host_time_valid\":"<<(sample.output.host_time_valid?"true":"false")
               <<",\"pending_after\":";
            if(pending_display_sequence)out<<*pending_display_sequence;else out<<"null";
            out<<",\"gates\":[";
            bool comma=false;
            for(const auto& gate:display_gates) {
                if(comma)out<<',';comma=true;
                out<<"{\"frame\":"<<gate.frame<<",\"baseline\":"<<gate.baseline
                   <<",\"completed\":"<<gate.completed<<'}';
            }
            out<<"]}";
        } else out<<"null";
        out<<",\"field_return\":";
        if(field_return) {
            const auto& f=*field_return;
            out<<"{\"ordinary\":"<<(f.ordinary?"true":"false")<<",\"phase\":"<<f.phase<<",\"ui_stage\":"<<f.ui_stage
               <<",\"complete\":"<<f.complete<<",\"counter\":"<<f.counter
               <<",\"queue\":";
            okage::write_load_observation(out,f.jobs.observation());
            out<<'}';
        } else out<<"null";
        out<<",\"tick_pending\":"<<(prepared_tick?"true":"false")
           <<",\"composition_pending\":"<<(pending_battle?"true":"false")<<",\"frame_events\":[";
        bool comma=false;
        for(const auto& event:frame_events) {
            if(comma)out<<',';comma=true;
            out<<"{\"frame\":"<<event.frame<<",\"kind\":\""<<event.kind<<"\",\"image\":"<<event.image<<'}';
        }
        out<<"]}\n";out.flush();
        if(!out)throw std::runtime_error("Native loader observation write failed");
        load_observation_written=true;
    }
    std::vector<std::uint32_t> battle_loading_trace;
    std::vector<std::uint32_t> game_mode_trace,retiring_effect_trace,results_trace,title_trace,name_entry_trace,chapter_trace,ordinary_load_trace,startup_trace,restore_trace,save_trace;
    bool retain_field_music=false;
    std::optional<okage::Texture> transition_image;
    // AppKit presentation is separate from research captures and feedback.
    bool live_presentation_enabled=false;
    mutable std::optional<okage::Texture> live_image;
    std::optional<okage::Texture> transition_live_image;
    unsigned transition_alpha=0,transition_target=0;
    int bind_instance(int alias) {
        if(alias>=0||alias<=-int(instance_aliases.size())||alias==-2||next_instance>=instance_limit)
            throw std::runtime_error("Field instance/alias allocation is unrecovered");
        const int instance=next_instance++;
        instance_aliases[-alias]=instance;return instance;
    }
    void bind_scene_instances() {
        next_instance=map_objects->next_front_instance();instance_limit=map_objects->first_back_instance();
        //0x131d28 clears follower aliases4..6, leaving absent NPC aliases live.
        for(unsigned i=4;i<=6;++i)instance_aliases[i]=-1;
        for(auto& binding:actor_bindings)binding.instance=bind_instance(binding.alias);
    }
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
            controller.shadow_request=[this](std::uint32_t request){
                if(!field || !field->request_player_shadow(request))
                    throw std::runtime_error("Dialogue player shadow is unavailable");
            };
            controller.actor_motion_request=[this](int alias,int entry){
                //0x148240 handler21 skips only battle mode4. This controller
                // belongs to the field; battle dialogue dispatch is separate.
                if(!field||!field->request_actor_motion(alias,std::bit_cast<unsigned>(entry)))
                    throw std::runtime_error("Dialogue actor motion target is unavailable");
            };
            wait.countdown=wait.observed=wait.accumulated=0;
            dialogue_active=true;
        },[this](int request){
            if(!dialogue_active && request!=-1)throw std::runtime_error("Field wait before dialogue initialization");
            const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
            const bool released=okage::poll_dialogue_wait(wait,request,
                {true,std::bit_cast<std::int32_t>(closure.message_flags),layers,std::int32_t(text.state.progress)})==0;
            if(released && !release_frame)release_frame=frame;
            if(released && request==0 && !close_release_frame)close_release_frame=frame;
            return released;
        },actor_bindings,diagnostic);
        field->render_projection=&projection;
        field->resolve_instance_alias=[this](int alias)->std::optional<int> {
            if(alias>=0||alias<=-int(instance_aliases.size())||alias==-2)return {};
            return instance_aliases[-alias];
        };
        field->set_instance_alias=[this](int alias,okage::FieldActor* actor) {
            if(alias>=0||alias<=-int(instance_aliases.size())||alias==-2)
                throw std::runtime_error("Field alias assignment is unrecovered");
            for(const auto& binding:actor_bindings)if(binding.actor==actor) {
                instance_aliases[-alias]=binding.instance;return;
            }
            throw std::runtime_error("Field alias assignment has no instance");
        };
        field->create_hat_scene=[this] {
            const auto path=event_directory/"hat"/"a.xmd";
            auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            auto camera=readData(event_directory/"hatcam.xmt");
            auto object=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),
                readData(motion),readActorTextures(xta),true);
            object->visible=false;
            renderer->add_actor({object.get(),path,-7});
            auto* actor=object.get();actor_objects.push_back(std::move(object));
            actor_bindings.push_back({-7,actor,false,bind_instance(-7)});
            return okage::FieldEventActor{actor,std::move(camera)};
        };
        field->scene_fade=[this](unsigned percent,int mode){scene_fade.script_request(percent,mode);};
        field->dialogue_input=[this](bool enabled){
            if(!dialogue_active)throw std::runtime_error("Field input toggle before dialogue initialization");
            controller.layer_flags=(controller.layer_flags&~0x10u)|(enabled?0x10u:0u);
        };
        if(map_objects)field->bind_map_scene(*map_objects);
        field->random_value=[this]{return random.next();};
        field->resolve_encounter_formation=[this](unsigned row,unsigned column) {
            if(!game_data)throw std::runtime_error("Natural encounter requires owned BKDATA");
            return game_data->encounter_formation(row,column);
        };
        field->observe_wander=[this](int alias,bool after,const okage::WanderingNpc& npc,const okage::Animation& animation) {
            const auto channels=animation.channel_outputs();
            wander_trace.insert(wander_trace.end(),{frame,std::uint32_t(okage::event_variable(variables,0x40000)),
                std::bit_cast<std::uint32_t>(alias),unsigned(after),std::bit_cast<std::uint32_t>(npc.counter),random.state,unsigned(channels.size())});
            for(float value:npc.velocity)wander_trace.push_back(std::bit_cast<std::uint32_t>(value));
            for(const auto& channel:channels)for(float value:channel)wander_trace.push_back(std::bit_cast<std::uint32_t>(value));
        };
        field->exterior_camera=&exterior_camera;
        field->collision_mask=&collision_mask;
        field->set_variable=[this](unsigned index,unsigned bank,std::int32_t value){okage::set_event_variable(variables,index,bank,value);};
        field->get_variable=[this](std::uint32_t selector){return okage::event_variable(variables,selector);};
        field->transition_service=[this](unsigned countdown){
            if(countdown==1){
                transition_image=surface();
                if(live_presentation_enabled)transition_live_image=live_image;
            }
            else if(countdown==0){transition_alpha=transition_target=128;}
        };
        field->request_chapter=[this](unsigned chapter_id,std::uint32_t payload) {
            if(!game_mode)throw std::runtime_error("Chapter request requires global mode owner");
            pending_game_command={14,chapter_id,payload}; //1248d8; constructor consumes later.
        };
        field->set_save_caption=[this](std::string_view caption) {
            if(caption.size()>=0x14)throw std::runtime_error("Save location caption extent");
            std::copy(caption.begin(),caption.end(),variables.begin()+0x864);
            variables[0x864+caption.size()]=0; //strcpy preserves the following tail.
        };
        field->open_save_menu=[this] {
            if(field_save||restore||field_return||!field_ruw||!field_ui_atlas||save_directory.empty())
                throw std::runtime_error("Field Save requires exclusive menu and owned UI/storage resources");
            if(dialogue_active&&!closure.finished()&&opacity.value>0)
                throw std::runtime_error("Field Save opening requires recovered concurrent dialogue composition");
            if(field->persistent_map_index()!=-1)
                throw std::runtime_error("Field Save minimap bitset owner remains unrecovered");
            if(!completed_game_image||completed_game_frame+1!=frame)
                throw std::runtime_error("Save opening requires preceding completed native frame");
            field_save=std::make_unique<okage::RestoreSession>(scene_clear,scene_fade,ui_shared,restore_shared,
                okage::RestoreConstruction{field_root.parent_path()/"CMNDATA",battle_input,save_directory,ui_fonts,{},true,field_ruw,field_ui_atlas});
            save_capture_surface=std::make_unique<okage::BattleCaptureSurface>();
            scene_fade.immediate(std::bit_cast<float>(0x3f4ccccdu));
            save_background=*completed_game_image;save_open_frame=frame;
            pending_game_command={0,0,0};
            std::cout<<"Native field Save constructed frame="<<frame<<'\n';
        };
        field->recover_party=[this] {
            if(!game_data)throw std::runtime_error("Field Rest requires owned character and item tables");
            //126370 is126228 with all four refresh/reset bits enabled.
            okage::refresh_game_characters(variables,*game_data,15);
            std::cout<<"Native field Rest recovered party frame="<<frame<<'\n';
        };
        field->battle_transition_service=[this](unsigned phase,okage::FieldBattleRequest& request) {
            if(phase==11) {
                // 0x192bf0 copies the other framebuffer, completed last tick.
                // Never render this partially updated phase11 frame to get it.
                if(!completed_game_image||completed_game_frame+1!=frame)
                    throw std::runtime_error("Battle capture requires the preceding completed native game frame");
                if(transition_alpha||battle_capture_layer)
                    throw std::runtime_error("Battle capture layer already owned by a transition");
                if(dialogue_active&&controller.window_type!=2&&opacity.value>0)
                    throw std::runtime_error("Battle capture of a visible dialogue window needs shared game-surface UI");
                for(unsigned alias:{4u,5u,6u})if(instance_aliases[alias]!=-1)
                    throw std::runtime_error("Battle capture requires recovered live follower return state");
                battle_capture_image=*completed_game_image;battle_capture_frame=completed_game_frame;
                battle_followers_saved=true; // 0x131828 marks even an empty roster saved.
                battle_capture_surface=std::make_shared<okage::BattleCaptureSurface>();
                return;
            }
            if(phase==12) {
                if(!battle_capture_image||battle_capture_layer)
                    throw std::runtime_error("Battle capture cover without a fresh frozen frame");
                battle_capture_layer.emplace(); // 0x114dc8(2): mode2, RGBA80808080, progress0.
                battle_capture_alpha=battle_capture_target=128;
                return;
            }
            if(phase==13) {
                if(!battle_followers_saved||!battle_capture_layer||!music||!game_mode||game_mode->requested())
                    throw std::runtime_error("Battle return requires saved field state, music and mode owner");
                auto saved=field->battle_return_snapshot(); // 0x1301a8, before later callbacks.
                if(saved.battle_record_selection!=-1)
                    throw std::runtime_error("Battle return character-record selection is unrecovered");
                battle_return=std::move(saved);
                music->select(battle_return->secondary_music,false); // 0x12d768 -> 0x12d580.
                retain_field_music=battle_return->secondary_music!=28; // 0x12da78.
                pending_game_command={13,0,0}; // 0x1248d8; persists for field return.
                if(request.stage==-1)request.stage=field->default_battle_stage();
                battle_entry.reset(request.entry_parameter,unsigned(request.rear_attack),
                    okage::event_variable(variables,0x80010)==0x83,request.return_token);
                game_mode->request({3,request.formation,std::bit_cast<std::uint32_t>(request.stage)});
                return; // 0x124930 queues only; do not destroy the field here.
            }
            throw std::runtime_error("Native battle transition phase is unrecovered (field phase "+
                std::to_string(phase)+", formation "+std::to_string(request.formation)+
                ", stage "+std::to_string(request.stage)+")");
        };
    }
    fs::path scene_map;
    void initialize_cold_start(const fs::path& root,const fs::path& saves,bool record) {
        if(game_data||game_mode||field||effect_preparation||startup_textures)throw std::runtime_error("Cold startup ownership already exists");
        field_root=root/"FLDDATA";event_directory=field_root/"EVENT/EVDATA.XPF";
        message_directory=field_root/"MES";message_bank.clear();messages={};
        game_data.emplace(readData(root/"CMNDATA/XDB/BKDATA.XDB"),readData(root/"CMNDATA/XDB/ITEM.XDB"));
        //125130 also loads the actual field database before1251d8 refresh.
        (void)okage::read_field_definition(readData(field_root/"FLDSET.XDB"),72);
        okage::initialize_new_game_variables(variables,*game_data);
        music_directory=root/"SNDDATA/BGM";sound_directory=root/"SNDDATA/SE";
        music=std::make_unique<okage::MusicRouter>();record_audio=record;
        battle_input=root/"NATIVE_UI";save_directory=saves;reset_scene_camera();
        initialize_startup_textures(root);
        effects_loaded=true;
        game_mode.emplace(okage::GameModeParent::startup());
        //124940: the empty parent queues {8,3,0} (logged before the 0x100618 boundary).
        if(startup_textures)startup_textures->note_mode_request(8,3,0);
        sound_banks.emplace(0,std::make_unique<okage::StreamPlayer>(
            okage::read_stream_bank(readData(sound_directory/"ALL.XWS"))));
        std::cout<<"Native cold startup queued mode8 selector3\n";
    }
    void initialize_startup_textures(const fs::path& root) {
        if(startup_textures||effect_preparation)throw std::runtime_error("Startup textures already initialized");
        if(!startup_texture_report.empty()&&fs::exists(startup_texture_report))
            throw std::runtime_error("Startup texture report directory must be fresh");
        const auto owned_inputs=std::make_shared<const okage::StartupInputs>(okage::load_startup_inputs(root));
        const auto& owned=*owned_inputs;
        okage::StartupTextureInputs inputs;
        inputs.font_palette=owned.font_palette;inputs.fonts=owned.fonts;
        inputs.record_observations=!startup_texture_report.empty()||!resource_report.empty()||!retirement_report.empty();
        const auto& names=okage::startup_effect_names();
        inputs.read_effect=[&owned,&names](const std::string& name)->std::optional<std::vector<std::uint8_t>> {
            for(unsigned i=0;i<names.size();++i)if(name==names[i])return owned.effects[i];
            return std::nullopt;
        };
        okage::EffectPreparationTrace trace;
        std::shared_ptr<const okage::EffectTemplatePreparation> prepared;
        auto prepare=[&](okage::StartupRandomStream& stream) {
            //10bc78: sizes are the owned XIM dimensions (CPU decode, no allocation event).
            okage::EffectTextureSizes sizes;
            for(unsigned i=0;i<sizes.size();++i) {
                if(okage::effect_texture_names[i]!=std::string(names[i]))throw std::runtime_error("Effect name tables disagree");
                const auto image=okage::decode_xim(owned.effects[i]);sizes[i]={image.width,image.height};
            }
            trace.grid_complete=[&stream]{stream.enter(okage::StartupRngPhase::EffectTemplate);};
            prepared=std::make_shared<const okage::EffectTemplatePreparation>(okage::read_efd(owned.efd),sizes,
                okage::make_sine_table(),[&stream]{return stream.draw();},&trace);
        };
        auto owner=std::make_unique<okage::StartupTextureOwner>();
        owner->run_startup(inputs,random,prepare);
        effect_bytes=owned.efd;effect_preparation=std::move(prepared);
        effect_data=root/"CMNDATA/2D/TES.EFD";effect_textures=root/"CMNDATA/2D/ENV_TEX.XPF";
        startup_textures=std::move(owner);
        startup_inputs=owned_inputs;
        {
            okage::SceneRenderer::EffectTextureBytes bytes;
            for(unsigned i=0;i<bytes.size();++i) {
                const auto at=std::find(names.begin(),names.end(),std::string(okage::effect_texture_names[i]));
                if(at==names.end())throw std::runtime_error("Effect name tables disagree");
                bytes[i]=owned.effects[unsigned(at-names.begin())];
            }
            verified_effect_textures=std::make_shared<const okage::SceneRenderer::EffectTextureBytes>(std::move(bytes));
        }
        startup_textures_status="pre-scheduler startup textures and RNG established; later history unmodeled";
        std::cout<<"Native startup textures: "<<startup_textures_status<<" rng_draws="<<startup_textures->rng_draws()<<'\n';
        if(!startup_texture_report.empty()) {
            okage::write_startup_texture_report(*startup_textures,startup_texture_report);
            okage::write_effect_preparation_report(*effect_preparation,trace,startup_texture_report);
        }
    }
    void initialize_startup_audio_and_cache() {
        if(startup_audio_initialized||!music)throw std::runtime_error("Startup shared service lifecycle");
        //12cde8 loads JGL_BSC0.XV and the six shared jingle sequences.
        for(int jingle:{20,21,22,23,33,34})
            music->install_jingle(*okage::music_jingle_index(jingle),read_music_resource(jingle));
        //1bc538 registers archives for later lookup. The native data tree has
        //already decoded them; validate the actual backing inputs at this point.
        (void)readData(event_directory/"e_home1c.xmt");
        for(const auto* name:{"HERO","STUN","RKUMO","WKUMO","BKUMO"})
            (void)readData(field_root/"OBJ"/(std::string(name)+".XPF")/"a.xmd");
        startup_audio_initialized=true;
        std::cout<<"Native startup shared sound/archive service frame="<<frame<<'\n';
    }
    void load_synchronous_field_ui() {
        if(battle_input.empty())return;
        auto bytes=readData(field_root.parent_path()/"CMNDATA/2D/MAIN.XPF"/(variables[12]?"bm_ifj.xim":"bm_if.xim"));
        if(bytes.size()<4)throw std::runtime_error("Field UI atlas header");
        unsigned header=0;for(unsigned i=0;i<4;++i)header|=unsigned(bytes[i])<<(8*i);
        header=(header&0xfc0fffff)|(0x1bu<<20);for(unsigned i=0;i<4;++i)bytes[i]=header>>(8*i);
        field_ui_atlas=std::make_shared<const okage::Texture>(okage::decode_xim(bytes));
        field_ruw=std::make_shared<okage::RuwResource>(readData(battle_input/"FLD.RUW"));
        okage::initialize_scene_ui(ui_shared);
    }
    void load_scene(const fs::path& map,const fs::path& event,const std::vector<std::pair<int,fs::path>>& assets) {
        if(game_data)throw std::runtime_error("Initial game data already prepared");
        const auto data_root=message_directory.parent_path().parent_path()/"CMNDATA"/"XDB";
        game_data.emplace(readData(data_root/"BKDATA.XDB"),readData(data_root/"ITEM.XDB"));
        okage::initialize_new_game_variables(variables,*game_data);
        scene_map=map;
        std::vector<okage::SceneActor> scene_actors;
        auto add=[&](int alias,const fs::path& path) {
            auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            auto object=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),readData(motion),
                readActorTextures(xta));
            actor_bindings.push_back({alias,object.get(),alias!=-3});scene_actors.push_back({object.get(),path,alias});
            actor_objects.push_back(std::move(object));
        };
        for(const auto& [alias,path]:assets) {
            if(alias==-1)add(-3,path.parent_path().parent_path()/"STUN.XPF"/"a.xmd");
            add(alias,path);
        }
        map_objects=std::make_unique<okage::MapObjects>(map);
        bind_scene_instances();
        renderer=std::make_unique<okage::SceneRenderer>(map,scene_actors,map_objects.get());
        auto stg=map;stg.replace_extension(".stg");const auto stage=readData(stg);projection.load_stage(stage);
        scene_clear.rgb={0,0,0};scene_clear.load_stage(stage);renderer->set_clear_rgb(scene_clear.rgb);
        dialogue_active=false;
        create_field(readData(event),true);
        if(std::any_of(actor_bindings.begin(),actor_bindings.end(),[](const auto& a){return a.alias==-1;})) {
            auto room=map.parent_path().stem().string();
            std::transform(room.begin(),room.end(),room.begin(),[](unsigned char c){return char(std::tolower(c));});
            field_root=event.parent_path().parent_path().parent_path();event_directory=event.parent_path();
            load_synchronous_field_ui();
            const auto database=readData(field_root/"FLDSET.XDB");
            const auto definition=okage::read_field_definition(database,room);
            current_scene=definition.scene;
            field->load_interactions(database,room,variables);
            field->load_exploration_camera(readData(stg));
            auto xcl=map;xcl.replace_extension(".xcl");field->load_exploration_collision(readData(xcl),variables);
        }
        game_mode.emplace(okage::GameModeSelection{2,unsigned(current_scene),unsigned(current_entrance)});
        std::cout<<"Native room: "<<renderer->placements()<<" placements; "<<renderer->omitted_effects()
            <<" effects omitted; "<<renderer->actors()<<" native actors; native projected shadows; diagnostic dialogue panel\n";
    }
    void prepare_effects(const fs::path& data,const fs::path& textures) {
        if(effect_preparation||field)throw std::runtime_error("Effect templates must initialize once before field construction");
        effect_bytes=readData(data);okage::EffectTextureSizes sizes;
        for(unsigned i=0;i<sizes.size();++i) {
            const auto image=okage::decode_xim(readData(textures/okage::effect_texture_names[i]));
            sizes[i]={image.width,image.height};
        }
        effect_preparation=std::make_shared<const okage::EffectTemplatePreparation>(
            okage::read_efd(effect_bytes),sizes,okage::make_sine_table(),[this]{return random.next();});
        effect_data=data;effect_textures=textures;
    }
    void load_effects(const fs::path& data,const fs::path& textures) {
        if(!field||!renderer)throw std::runtime_error("Effects require a loaded scene");
        if(!effect_preparation||effect_data!=data||effect_textures!=textures)
            throw std::runtime_error("Effects require the session's prepared resources");
        renderer->load_effect_textures(textures);field->load_effects(effect_bytes,effect_preparation);effects_loaded=true;
        auto stage=scene_map;stage.replace_extension(".stg");
        field->load_map_effects(readData(scene_map),renderer->effect_texture_sizes());
        renderer->bind_map_effects(&field->map_effects());
        renderer->bind_effect_submissions(&field->effect_submissions());
        renderer->set_effect_light_tint(okage::field_effect_tint(readData(stage)));
        std::cout<<"Native effect placements initialized from owned map data\n";
    }
    void load_destination(const okage::FieldTransition& request,okage::ExplorationInput input) {
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
        load_synchronous_field_ui();
        auto add=[&](int alias,const std::string& model){
            const auto path=field_root/"OBJ"/(upper(model)+".XPF")/"a.xmd";
            auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            auto object=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),readData(motion),
                readActorTextures(xta));
            actor_bindings.push_back({alias,object.get(),alias!=-3});scene_actors.push_back({object.get(),path,alias});actor_objects.push_back(std::move(object));
        };
        for(const auto& npc:definition.npcs)add(npc.alias,npc.model);
        add(-3,"stun");add(-1,"hero");
        map_objects=std::make_unique<okage::MapObjects>(map);
        bind_scene_instances();
        renderer=std::make_unique<okage::SceneRenderer>(map,scene_actors,map_objects.get());
        auto stg=map,xcl=map;stg.replace_extension(".stg");xcl.replace_extension(".xcl");
        const auto stage=readData(stg);projection.load_stage(stage);
        scene_clear.rgb={0,0,0};scene_clear.load_stage(stage);renderer->set_clear_rgb(scene_clear.rgb);
        create_field(script,false);bind_audio_services();
        if(!effects_loaded)throw std::runtime_error("Destination requires owned effect resources");
        renderer->load_effect_textures(effect_textures);field->load_effects(effect_bytes,effect_preparation);
        field->load_map_effects(readData(map),renderer->effect_texture_sizes());
        renderer->bind_map_effects(&field->map_effects());
        renderer->bind_effect_submissions(&field->effect_submissions());
        renderer->set_effect_light_tint(okage::field_effect_tint(stage));
        scene_map=map;
        dialogue_active=false;choices={};narration={};
        // 0x132720 requests exactly1.0 through0x114cc0 before startup dispatch.
        // This is distinct from event4f's rounded percent multiplication.
        scene_fade.request(1.f);
        field->enter(std::move(definition),stage,readData(xcl),request.entrance,input);
        current_scene=request.scene;current_entrance=int(request.entrance);transition_target=0;
        game_mode.emplace(okage::GameModeSelection{2,unsigned(current_scene),unsigned(current_entrance)});
        std::cout<<"Native destination: scene "<<current_scene<<" entrance "<<current_entrance<<"; "<<renderer->actors()<<" actors\n";
    }
    static std::string field_asset_component(std::string name) {
        if(name.empty()||name.front()=='/'||name.find("..")!=std::string::npos||name.find('\\')!=std::string::npos)
            throw std::runtime_error("Unsafe return field asset path");
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});return name;
    }
    okage::ResultsUiContext results_context() {
        if(!field_return||!game_data)throw std::runtime_error("Results context ownership");
        auto& f=*field_return;
        return {variables,battle_entry.reward_storage(),*game_data,ui_party,*f.numbers,*f.text,
            [this](unsigned slot){
                if(!music||slot!=5)throw std::runtime_error("Results jingle slot unavailable");
                music->select(34);
            },2};
    }
    void begin_field_return() {
        if(!battle_return||field_return||pending_game_command[0]!=13||battle_input.empty())
            throw std::runtime_error("Victory return requires retained field snapshot and owned data");
        //133830 skips125ad0 for command13; all temporary game flags survive.
        scene_fade.immediate(0);scene_clear.rgb={0,0,0};scene_fade.color=0;
        okage::initialize_scene_ui(ui_shared);exterior_camera.snap_target=true;
        current_scene=battle_return->scene;current_entrance=-1;pending_game_command={};
        field_return=std::make_unique<FieldReturnLoad>(load_observation_trace!=nullptr);
        auto* f=field_return.get();const auto input=battle_input,decoded=field_root.parent_path();
        const auto owned_fonts=ui_fonts;const bool alternate=variables[12]!=0;
        f->jobs.enqueue([f,input,decoded,owned_fonts,alternate]{@autoreleasepool {
            if(!f->jobs.checkpoint())return;
            f->definitions.emplace(readData(input/"results-ui.bin"));
            f->text.emplace(readData(input/"results-text.bin"));
            f->numbers.emplace(readData(input/"battle-hud.bin"));
            if(!f->jobs.checkpoint())return;
            auto bytes=readData(decoded/"CMNDATA/2D/MAIN.XPF"/(alternate?"bm_ifj.xim":"bm_if.xim"));
            if(bytes.size()<4)throw std::runtime_error("Field UI atlas header");
            unsigned header=0;for(unsigned i=0;i<4;++i)header|=unsigned(bytes[i])<<(8*i);
            header=(header&0xfc0fffff)|(0x1bu<<20);for(unsigned i=0;i<4;++i)bytes[i]=header>>(8*i);
            f->atlas=std::make_shared<const okage::Texture>(okage::decode_xim(bytes));
            if(!f->jobs.checkpoint())return;
            f->ruw=std::make_shared<okage::RuwResource>(readData(input/"FLD.RUW"));
            if(!f->jobs.checkpoint())return;
            f->drawing=std::make_unique<okage::ResultsRenderer>(owned_fonts,f->atlas);
        }},{0x132688,0x131d28});
        std::cout<<"Native victory return queued scene="<<current_scene<<" frame="<<frame<<'\n';
    }
    void begin_ordinary_field(okage::GameModeSelection selection) {
        if(selection[0]!=2||selection[1]>0x7fffffff||selection[2]>0x7fffffff||
            (pending_game_command!=std::array<std::uint32_t,3>{}&&pending_game_command!=std::array<std::uint32_t,3>{3,0,0})||field_return||battle_input.empty())
            throw std::runtime_error("Ordinary field constructor requires explicit nonnegative destination and command0/3");
        std::fill(variables.begin()+0x10,variables.begin()+0x110,0);
        scene_fade.immediate(0);scene_clear.rgb={0,0,0};scene_fade.color=0;
        okage::initialize_scene_ui(ui_shared);exterior_camera.snap_target=true;
        current_scene=int(selection[1]);current_entrance=int(selection[2]);
        field_return=std::make_unique<FieldReturnLoad>(load_observation_trace!=nullptr);
        auto* f=field_return.get();f->ordinary=true;f->phase=7;f->entrance=selection[2];
        const auto input=battle_input,decoded=field_root.parent_path();
        const bool alternate=variables[12]!=0;
        f->jobs.enqueue([f,input,decoded,alternate]{@autoreleasepool {
            f->counter.store(0); //132688, before the first loader service.
            if(!f->jobs.checkpoint())return;
            auto bytes=readData(decoded/"CMNDATA/2D/MAIN.XPF"/(alternate?"bm_ifj.xim":"bm_if.xim"));
            if(bytes.size()<4)throw std::runtime_error("Ordinary field UI atlas header");
            unsigned header=0;for(unsigned i=0;i<4;++i)header|=unsigned(bytes[i])<<(8*i);
            header=(header&0xfc0fffff)|(0x1bu<<20);for(unsigned i=0;i<4;++i)bytes[i]=header>>(8*i);
            f->atlas=std::make_shared<const okage::Texture>(okage::decode_xim(bytes));
            if(!f->jobs.checkpoint())return;
            f->ruw=std::make_shared<okage::RuwResource>(readData(input/"FLD.RUW"));
        }},{0x132688,0x131d28});
        std::cout<<"Native ordinary field queued scene="<<current_scene<<" entrance="<<current_entrance<<" frame="<<frame<<'\n';
    }
    void service_field_return_loader() {
        if(!field_return)return;
        auto& f=*field_return;f.jobs.check();
        if(load_observation_trace)f.jobs.observe(okage::LoadEventKind::Poll,0,{},frame);
        if(!f.resumed){if(loading_policy==LoadingPolicy::Continuous)f.jobs.resume();f.resumed=true;return;}
        if(f.jobs.pending())return;
        if(!f.world_queued) {
            f.ui_stage=1;
            f.definition=okage::read_field_definition(readData(field_root/"FLDSET.XDB"),current_scene);
            field_asset_component(f.definition.event);
            f.script=readData(event_directory/(f.definition.event+".xmt"));
            f.definition=f.ordinary?okage::prepare_field_load(f.script,variables,std::move(f.definition),f.entrance):
                okage::prepare_field_return(f.script,variables,std::move(f.definition));
            if(f.definition.type>1||!f.definition.followers.empty())
                throw std::runtime_error("Return field type/followers unrecovered");
            const auto bank=field_asset_component(f.definition.message);
            auto replacement=okage::read_pme(readData(message_directory/(bank+".PME")));
            if(!replacement.bind("############",player_name))throw std::runtime_error("Return PME name binding missing");
            messages=std::move(replacement);message_bank=bank;
            if(f.ordinary) {
                if(!music)throw std::runtime_error("Ordinary field requires persistent music owner");
                music->enter(f.definition.music,f.definition.secondary_music,
                    [this](int id){return read_music_resource(id);});
            }
            f.map=field_root/field_asset_component(f.definition.room_directory)/(field_asset_component(f.definition.room)+".XPF")/"a.xmp";
            const auto root=field_root,textures=effect_textures;auto* target=&f;
            f.world_queued=true;
            f.jobs.enqueue([target,root,textures]{@autoreleasepool {
                auto& w=*target;
                if(!w.jobs.checkpoint())return;
                auto stage=w.map,collision=w.map;stage.replace_extension(".stg");collision.replace_extension(".xcl");
                w.stage=readData(stage);w.collision=readData(collision);
                w.objects=std::make_unique<okage::MapObjects>(w.map);
                if(!w.jobs.checkpoint())return;
                std::vector<okage::SceneActor> scene_actors;
                const auto add=[&](int alias,const std::string& model) {
                    const auto path=root/"OBJ"/(field_asset_component(model)+".XPF")/"a.xmd";
                    auto motion=path,texture=path;motion.replace_extension(".xmt");texture.replace_extension(".xta");
                    auto actor=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),readData(motion),readActorTextures(texture));
                    w.bindings.push_back({alias,actor.get(),alias!=-3});scene_actors.push_back({actor.get(),path,alias});w.actors.push_back(std::move(actor));
                    return w.jobs.checkpoint();
                };
                for(const auto& npc:w.definition.npcs)if(!add(npc.alias,npc.model))return;
                if(!add(-3,"stun")||!add(-1,"hero"))return;
                w.renderer=std::make_unique<okage::SceneRenderer>(w.map,scene_actors,w.objects.get());
                if(!w.jobs.checkpoint())return;
                w.renderer->load_effect_textures(textures);
            }},{0x132688,0x131d28});
            return;
        }
        if(f.world_prepared)return;
        if(!music||!effects_loaded)throw std::runtime_error("Return requires persistent music/effect owners");
        //Phase12 loads both score slots but defers primary playback until commit.
        if(!f.ordinary)music->enter(f.definition.music,f.definition.secondary_music,[this](int id){return read_music_resource(id);},true);
        map_objects=std::move(f.objects);actor_objects=std::move(f.actors);actor_bindings=std::move(f.bindings);
        renderer=std::move(f.renderer);bind_scene_instances();create_field(f.script,false);bind_audio_services();
        field->load_effects(effect_bytes,effect_preparation);
        field->load_map_effects(readData(f.map),renderer->effect_texture_sizes());
        renderer->bind_map_effects(&field->map_effects());renderer->bind_effect_submissions(&field->effect_submissions());
        renderer->set_effect_light_tint(okage::field_effect_tint(f.stage));
        field->prepare(f.definition,f.stage,f.collision);
        f.prepared_field=std::move(field);f.objects=std::move(map_objects);f.actors=std::move(actor_objects);
        f.bindings=std::move(actor_bindings);f.renderer=std::move(renderer);
        f.world_prepared=true;f.complete=1;
        std::cout<<"Native "<<(f.ordinary?"ordinary":"return")<<" loader complete frame="<<frame<<'\n';
    }
    bool tick_field_return(std::uint32_t pressed,okage::ExplorationInput input) {
        if(!field_return)return false;
        auto& f=*field_return;f.jobs.check();++f.counter;
        if(f.ordinary) {
            ordinary_load_trace.insert(ordinary_load_trace.end(),{frame,f.phase,f.ui_stage,f.complete,f.counter,
                unsigned(f.jobs.pending()),unsigned(current_scene),f.entrance,random.state});
            if(f.complete==1&&f.jobs.pending()==0) {
                renderer=std::move(f.renderer);field=std::move(f.prepared_field);map_objects=std::move(f.objects);
                actor_objects=std::move(f.actors);actor_bindings=std::move(f.bindings);scene_map=f.map;
                field_ruw=std::move(f.ruw);field_ui_atlas=std::move(f.atlas);
                projection.load_stage(f.stage);scene_clear.load_stage(f.stage);renderer->set_clear_rgb(scene_clear.rgb);
                f.phase=0;
                field->activation_ready=[this]{
                    scene_fade.request(1.f);
                    if(transition_alpha)transition_target=0;
                    if(battle_capture_layer)battle_capture_target=0;
                };
                field->activate_entry(f.entrance,input);
                battle_followers_saved=false;
                write_load_observation("ordinary_field_activated");
                field_return.reset();
                std::cout<<"Native ordinary field activated scene="<<current_scene<<" entrance="<<current_entrance
                         <<" frame="<<frame<<" pc="<<field->pc()<<'\n';
                return true;
            }
        }
        //133a98 phase12 creates UI before polling loader completion.
        if(!f.ordinary&&f.ui_stage==1) {
            f.ui_stage=2;
            f.ui=std::make_unique<okage::ResultsInterface>(ui_shared.registry,ui_shared.resource,
                ui_shared.resource_accumulators,results_shared,*f.definitions,f.ruw,f.atlas,0,results_context());
            std::cout<<"Native results constructed frame="<<frame<<'\n';
        }
        if(f.complete==1&&f.jobs.pending()==0&&f.ui_stage==2) {f.ui_stage=3;results_shared.loader_ready=1;}
        if(f.complete==1&&f.ui_stage==3&&f.ui&&f.ui->dead()) {
            //100c08/range0 then132720; retain GameModeParent and saved entrance.
            renderer=std::move(f.renderer);field=std::move(f.prepared_field);map_objects=std::move(f.objects);
            actor_objects=std::move(f.actors);actor_bindings=std::move(f.bindings);scene_map=f.map;
            field_ruw=f.ruw;field_ui_atlas=f.atlas;
            projection.load_stage(f.stage);scene_clear.load_stage(f.stage);renderer->set_clear_rgb(scene_clear.rgb);
            if(f.definition.music>0)music->select(f.definition.music);
            scene_fade.request(1.f);battle_capture_target=0;
            field->activate_return(*battle_return,input);
            battle_followers_saved=false; //131a80 consumes the saved empty roster.
            //The parent commits before visiting resource then dead manager.
            //Keep its zero-opacity window registered for that final resource
            //clock/layout pass, preserving persistent draw counters.
            const std::array<okage::UiResourceSlotBinding,1> bindings{{{f.ui->handle(),0}}};
            if(f.ui->window().opacity()!=0.f)throw std::runtime_error("Dead results window has nonzero opacity");
            (void)ui_shared.resource.advance_resource(ui_shared.registry,bindings,ui_shared.resource_accumulators,
                ui_fonts,battle_capture_sine,random,okage::initial_ui_projection());
            write_load_observation("field_return_retired");
            f.ui.reset();field_return.reset();
            std::cout<<"Native saved field restored scene="<<current_scene<<" frame="<<frame<<'\n';
            return true;
        }
        reset_camera->tick_channels();
        projection.submit_empty_resource_frame(!f.world_queued,
            f.settings.cached_depth_min,f.settings.cached_depth_max);
        if(retiring_effects&&(!f.world_queued))retiring_effects->tick();
        if(f.ui) {
            const std::array<okage::UiResourceSlotBinding,1> bindings{{{f.ui->handle(),0}}};
            f.draw=ui_shared.resource.advance_resource(ui_shared.registry,bindings,ui_shared.resource_accumulators,
                ui_fonts,battle_capture_sine,random,okage::initial_ui_projection());
            f.rendered=&f.drawing->render(f.draw,scene_fade);
            auto context=results_context();f.ui->first_update(pressed,context);f.ui->second_update(context);
        } else {
            f.draw=ui_shared.resource.advance_resource(ui_shared.registry,{},ui_shared.resource_accumulators,
                ui_fonts,battle_capture_sine,random,okage::initial_ui_projection());
            if(f.draw)throw std::runtime_error("Return resource has unexpected preloader UI");
        }
        if(!f.ordinary)results_trace.insert(results_trace.end(),{frame,f.phase,f.ui_stage,f.complete,f.counter,
            unsigned(f.jobs.pending()),f.ui?f.ui->words()[0xe4c/4]:0xffffffffu,
            f.ui?f.ui->words()[(0x40+0x68)/4]:0xffffffffu,
            results_shared.loader_ready,results_shared.progress.notice,results_shared.progress.notice_row,
            results_shared.progress.notice_wait,results_shared.reward_display,results_shared.money_display,
            std::bit_cast<unsigned>(okage::event_variable(variables,0x40003)),random.state});
        return false;
    }
    void retire_field() {
        const bool chapter_handoff=field&&field->transition()&&field->transition()->ready&&
            field->chapter_selector()>=0&&pending_game_command[0]==14&&
            pending_game_command[1]==unsigned(field->chapter_selector());
        if(!field||!renderer||(!chapter_handoff&&(!battle_return||!battle_capture_layer)))
            throw std::runtime_error("Field retirement requires a complete mode handoff");
        if(retiring_effects) {
            const auto flags=retiring_effects->flags();
            if(std::any_of(flags.begin(),flags.end(),[](auto f){return f!=0;}))
                throw std::runtime_error("Prior resource effects still occupy field retirement pool");
        }
        field->validate_retirement();
        renderer.reset();
        retiring_effects.emplace(field->retire_effects());
        dialogue_active=false;opening.reset();choices={};narration={};controller={};clock={};
        text=okage::DialogueText({});text.fonts=fonts;
        opacity={};closure={};
        actor_bindings.clear();field.reset();map_objects.reset();actor_objects.clear();
        field_ruw.reset();field_ui_atlas.reset();
        next_instance=0;instance_limit=640; // Numeric alias table survives.
        projection.reset_scene();
        reset_scene_camera();
        messages={};message_bank.clear();
        scene_fade.step=std::bit_cast<float>(0x3ca3d70au);
    }
    void reset_scene_camera() {
        if(!reset_camera)reset_camera=std::make_unique<okage::Animation>(3);
        okage::reset_scene_camera(*reset_camera);
    }
    void retire_battle() {
        if(!battle||field||renderer)throw std::runtime_error("Battle retirement mode ownership");
        if(retiring_effects) {
            const auto flags=retiring_effects->flags();
            if(std::any_of(flags.begin(),flags.end(),[](auto f){return f!=0;}))
                throw std::runtime_error("Prior resource effects still occupy battle retirement pool");
        }
        if(pending_battle)frame_event("retire_with_prior",pending_battle->completion->frame());
        retiring_effects.emplace(battle->retire_resource(ui_shared,projection));
        next_instance=0;instance_limit=640;
        reset_scene_camera();
        std::cout<<"Native battle resource destroyed frame="<<frame<<'\n';
    }
    void observe_game_mode(unsigned point) {
        if(!game_mode||(!game_mode->requested()&&!game_mode->has_dead_allocation()&&game_mode->current()[0]==2))return;
        const auto current=game_mode->current(),request=game_mode->queue_words();
        game_mode_trace.insert(game_mode_trace.end(),{frame,point,game_mode->phase(),current[0],current[1],current[2],
            request[0],request[1],request[2],unsigned(game_mode->has_child()),unsigned(game_mode->has_dead_allocation()),
            unsigned(bool(field)||bool(field_return)||bool(defeat_return)),pending_game_command[0],pending_game_command[1],pending_game_command[2],
            std::bit_cast<unsigned>(scene_fade.current),std::bit_cast<unsigned>(scene_fade.target),std::bit_cast<unsigned>(scene_fade.step),
            battle_capture_alpha,battle_capture_target,battle_capture_frame,completed_game_frame,
            scene_clear.rgb[0],scene_clear.rgb[1],scene_clear.rgb[2]});
        if(retiring_effects) {
            retiring_effect_trace.insert(retiring_effect_trace.end(),{frame,point});
            const auto flags=retiring_effects->flags();
            retiring_effect_trace.insert(retiring_effect_trace.end(),flags.begin(),flags.end());
        }
    }
    void close_loader_window() {
        if(loading_policy==LoadingPolicy::Continuous)return;
        if(battle)battle->pause_loader();
        if(field_return)field_return->jobs.pause_and_wait();
    }
    void open_loader_window() {
        if(loading_policy==LoadingPolicy::Continuous)return;
        // Resolve current owners now: tick can replace or retire either one.
        if(battle)battle->resume_loader();
        if(field_return&&field_return->jobs.pending())field_return->jobs.resume();
    }
    bool pending_battle_frame() const {return bool(pending_battle);}
    bool tick_pending() const {return prepared_tick;}
    void finish_battle_frame() {
        if(prepared_tick)finish_prepared_tick(true);
        if(pending_battle)complete_battle_frame();
    }
    bool tick(okage::DialogueButtons input,bool block=true) {
        if(prepared_tick)throw std::runtime_error("Scheduler advanced before prepared tick submission");
        if(field)finish_battle_frame();
        close_loader_window();
        if(pending_battle) {
            if(pending_battle->completion->frame()!=frame)
                throw std::runtime_error("Scheduler lost its previous submitted frame");
        }
        if(input_journal)input_journal->begin_tick(frame+1,input.held,input.pressed);
        ++frame;
        if(pending_battle)frame_event("scheduler_with_prior",pending_battle->completion->frame());
        const auto released=std::uint16_t(previous_held&~input.held);previous_held=input.held;
        input.repeat=pad_repeat.update(input.held,input.pressed);
        if(battle_capture_layer) {
            // 0x114ac0. The battle loader must request target0 when its real
            // resources become ready; there is no fabricated reveal timer.
            if(battle_capture_alpha!=battle_capture_target)
                battle_capture_alpha=battle_capture_alpha>battle_capture_target?battle_capture_alpha-4:battle_capture_alpha+4;
            if(!battle_capture_alpha) {battle_capture_layer.reset();battle_capture_draw.reset();}
            else battle_capture_layer->rgba=0x00808080u|(battle_capture_alpha<<24);
        }
        // 0x114ac0 advances the captured-frame overlay by four alpha units.
        if(transition_alpha!=transition_target) {
            transition_alpha=transition_alpha>transition_target?transition_alpha-4:transition_alpha+4;
            if(!transition_alpha){transition_image.reset();transition_live_image.reset();}
        }
        scene_fade.tick();
        //0x100618: the first scheduler traversal starts the per-frame clear/draw kicks
        //into the colour buffers and depth Z24 lanes (the title rendering contract).
        if(startup_textures&&startup_textures->stage()==okage::ResourceStage::PreScheduler)
            startup_textures->begin_scheduled_frames();
        if(game_mode) {
            observe_game_mode(0);
            game_mode->tick([this]{
                if(battle)retire_battle();
                else if(startup) {
                    const bool modeled=startup_textures&&startup_textures->stage()!=okage::ResourceStage::Unmodeled;
                    if(modeled)startup_textures->note_mode_resource_retire(startup_selector); //101058 entry
                    startup->retire(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared);
                    if(startup_textures&&startup_textures->stage()==okage::ResourceStage::LogosConstructed)
                        startup_textures->retire_image_list();
                    if(modeled)startup_textures->note_mode_destroy(startup_selector); //12c8d8
                }
                else if(chapter) {
                    chapter->retire(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared,scene_fade);
                    std::cout<<"Native chapter retired frame="<<frame<<'\n';
                }
                else if(restore) {
                    restore->retire(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared);
                    std::cout<<"Native Restore retired frame="<<frame<<'\n';
                }
                else if(name_entry) {
                    name_entry->retire(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared);
                    std::cout<<"Native name entry retired frame="<<frame<<'\n';
                }
                else if(title) {
                    retiring_effects.emplace(title->retire(*reset_camera,projection,ui_shared));
                    //N+1: 101058 resource destructor, then the mode-8 destructor 12c8d8 -> 12dd50(1).
                    if(title_modeled(okage::ResourceStage::TitleRemovalRequested)) {
                        startup_textures->retire_title_resources();
                        title_checkpoint("resource-retirement"); //entry of 12c8d8, after 101058 returned
                        startup_textures->destroy_title_mode();
                    } else if(startup_textures)startup_textures->mark_unmodeled("title retired outside the modeled order");
                    title_stream.request_stop(1);
                    std::cout<<"Native title resource/task destroyed frame="<<frame<<'\n';
                }
                else if(game_over) {
                    game_over->retire(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared);
                    std::cout<<"Native game-over resource/task destroyed frame="<<frame<<'\n';
                }
                else if(defeat_return) {
                    defeat_return->retire(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared,scene_fade);
                    std::cout<<"Native defeat return resource/field destroyed frame="<<frame<<'\n';
                } else retire_field();
            },[this]{
                if(battle) {
                    if(battle->resource_live())throw std::runtime_error("Battle wrapper freed before resource destructor");
                    write_load_observation("wrapper_free");
                    if(pending_battle)frame_event("free_with_prior",pending_battle->completion->frame());
                    battle.reset();
                    std::cout<<"Native battle wrapper freed frame="<<frame<<'\n';
                }
                if(defeat_return) {
                    if(defeat_return->resource().live())throw std::runtime_error("Defeat return field freed before destruction");
                    defeat_return.reset();
                    std::cout<<"Native defeat return field freed frame="<<frame<<'\n';
                }
                if(restore) {
                    if(restore->resource_live())throw std::runtime_error("Restore free before retirement");
                    restore.reset();
                }
                if(name_entry) {
                    if(name_entry->resource_live())throw std::runtime_error("Name entry free before retirement");
                    name_entry.reset();
                }
                if(chapter) {
                    if(chapter->resource_live())throw std::runtime_error("Chapter free before retirement");
                    chapter.reset();
                }
                if(startup) {
                    if(startup->resource_live())throw std::runtime_error("Startup freed before resource retirement");
                    startup.reset();
                    if(startup_textures&&startup_textures->stage()!=okage::ResourceStage::Unmodeled)
                        startup_textures->note_mode_wrapper_free(startup_selector);
                }
                if(title) {
                    if(title->resource_live())throw std::runtime_error("Title freed before resource retirement");
                    title.reset();first_title=false;
                    //N+2: resource node, then mode-8 node freed.
                    if(title_modeled(okage::ResourceStage::TitleResourcesRetired))startup_textures->free_title_wrappers();
                }
                if(game_over) {
                    if(game_over->resource_live())throw std::runtime_error("Game-over task freed before resource destructor");
                    game_over.reset();
                    std::cout<<"Native game-over task freed frame="<<frame<<'\n';
                }
                if(field||renderer||!actor_bindings.empty()||!actor_objects.empty()||map_objects)
                    throw std::runtime_error("Field borrowers survived mode retirement");
            },[this](okage::GameModeSelection selection){
                if(battle||field||field_return||defeat_return||game_over||title||name_entry||restore||chapter||startup||!reset_camera)
                    throw std::runtime_error("Native mode construction has surviving scene owners");
                if(title_modeled(okage::ResourceStage::TitleWrappersFreed)) {
                    if(selection==okage::GameModeSelection{startup_textures->successor_mode(),0,0}) {
                        startup_textures->enter_successor(selection[0]);
                        title_checkpoint("successor-entry");
                        write_title_route();
                        startup_textures->mark_unmodeled(selection[0]==10?"Restore constructor body (124548) is not modeled":
                            "New Game constructor body (124548) is not modeled");
                    } else startup_textures->mark_unmodeled("title successor differs from the requested mode");
                }
                if(selection[0]==2&&pending_game_command[0]==13) {begin_field_return();return;}
                if(selection[0]==2&&(pending_game_command[0]==0||pending_game_command[0]==3)) {begin_ordinary_field(selection);return;}
                if(selection[0]==2&&pending_game_command[0]==14) {
                    chapter=std::make_unique<okage::ChapterSession>(variables,pending_game_command,selection,
                        scene_clear,scene_fade,ui_shared,[this](unsigned id){
                            if(id<1||id>7)throw std::runtime_error("Unrecovered chapter image selection");
                            return readData(field_root.parent_path()/"CMNDATA/2D"/(
                                "CHP_0"+std::to_string(id)+".XIM"));
                        });
                    std::cout<<"Native chapter constructed frame="<<frame<<'\n';return;
                }
                if(selection[0]==2&&pending_game_command[0]==2) {
                    defeat_return=std::make_unique<okage::DefeatFieldReturn>(variables,pending_game_command,
                        scene_fade,scene_clear,ui_shared);
                    std::cout<<"Native defeat return field constructed frame="<<frame<<'\n';
                    return;
                }
                if(selection==okage::GameModeSelection{10,0,0}) {
                    restore=std::make_unique<okage::RestoreSession>(scene_clear,scene_fade,ui_shared,restore_shared,
                        okage::RestoreConstruction{field_root.parent_path()/"CMNDATA",battle_input,save_directory,ui_fonts,[this]{if(music)music->stop();}});
                    std::cout<<"Native Restore constructed frame="<<frame<<'\n';return;
                }
                if(selection==okage::GameModeSelection{9,0,0}) {
                    name_entry=std::make_unique<okage::NameEntrySession>(scene_clear,scene_fade,ui_shared,name_entry_shared,
                        okage::NameEntryConstruction{field_root.parent_path()/"CMNDATA",battle_input,ui_fonts,[this]{if(music)music->stop();}});
                    std::cout<<"Native name entry constructed frame="<<frame<<'\n';return;
                }
                if(selection==okage::GameModeSelection{8,3,0}||selection==okage::GameModeSelection{8,0,0}) {
                    if(selection[1]==0&&startup_textures) {
                        if(startup_textures->stage()==okage::ResourceStage::Scheduled)startup_textures->begin_logo_construction();
                        else startup_textures->mark_unmodeled("logo construction outside the cold-start order");
                    }
                    startup_selector=selection[1];
                    startup=std::make_unique<okage::StartupSession>(scene_clear,scene_fade,ui_shared,game_over_shared,
                        okage::StartupConstruction{selection[1],save_directory,[this]{if(music)music->stop();},
                        [this]{initialize_startup_audio_and_cache();},[this](unsigned index){
                            auto bytes=readData(field_root.parent_path()/"CMNDATA/2D/SCR.XPF"/(index==0?"scea.xim":"zener.xim"));
                            //1009e0 -> 1b2348: the same bytes become the source image-list entry.
                            if(startup_textures&&startup_textures->stage()==okage::ResourceStage::LogosConstructed)
                                startup_textures->append_image(bytes);
                            return bytes;
                        }});
                    std::cout<<"Native startup constructed selector="<<selection[1]<<" frame="<<frame<<'\n';return;
                }
                if(selection==okage::GameModeSelection{8,1,0}) {
                    std::shared_ptr<const std::array<std::vector<std::uint8_t>,8>> title_images;
                    std::shared_ptr<const std::vector<std::uint8_t>> title_model;
                    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> title_model_textures;
                    const auto night_v=field_root.parent_path()/"CMNDATA/2D/NIGHT_V.XPF";
                    //1c2e68 -> 0x1bc700 lower-cases the mounted-archive query IN PLACE
                    //(ASCII 'A'..'Z' via the ctype table); 1cc4e0 then names the handle with it.
                    const auto night_v_reader=[night_v](std::string& name)->std::optional<std::vector<std::uint8_t>> {
                        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){
                            return char(c>='A'&&c<='Z'?c+32:c);});
                        if(!fs::exists(night_v/name))return std::nullopt;
                        return readData(night_v/name);
                    };
                    if(startup_textures&&startup_textures->stage()!=okage::ResourceStage::LogosRetired)
                        startup_textures->mark_unmodeled("title constructed again after unmodeled history");
                    if(startup_textures&&startup_textures->stage()==okage::ResourceStage::LogosRetired) {
                        //12c350(1): eight SCR images through 1009e0/1b2348, then 186c48("a.xmd").
                        startup_textures->begin_title_construction();
                        auto images=std::make_shared<std::array<std::vector<std::uint8_t>,8>>();
                        const auto scr=field_root.parent_path()/"CMNDATA/2D/SCR.XPF";
                        for(unsigned i=0;i<images->size();++i) {
                            (*images)[i]=readData(scr/okage::title_image_names[i]);
                            startup_textures->append_image((*images)[i]);
                        }
                        title_images=images;
                        auto model_bytes=std::make_shared<const std::vector<std::uint8_t>>(readData(night_v/"a.xmd"));
                        title_model=model_bytes;
                        const auto model=okage::read_xmd(*model_bytes);
                        const auto loaded=startup_textures->load_and_bind_model("NIGHT_V/a.xmd",model.textures,night_v_reader);
                        if(loaded.first<0)throw std::runtime_error("Title NIGHT_V texture load failed; the model-less title is unrecovered");
                        title_model_textures=startup_textures->model_slot_bytes();
                    } else {
                        auto model_bytes=std::make_shared<const std::vector<std::uint8_t>>(readData(night_v/"a.xmd"));
                        title_model=model_bytes;
                        for(auto name:okage::read_xmd(*model_bytes).textures) {
                            name+=".xim";auto bytes=night_v_reader(name);
                            if(!bytes)throw std::runtime_error("Title model texture unavailable: "+name);
                            title_model_textures.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes)));
                        }
                    }
                    title=std::make_unique<okage::TitleSession>(scene_clear,scene_fade,ui_shared,game_over_shared,
                        okage::TitleConstruction{field_root.parent_path()/"CMNDATA",effect_textures,verified_effect_textures,title_images,title_model,title_model_textures,effect_bytes,
                        [this]{if(music)music->stop();},[this]{
                            if(music_directory.empty())throw std::runtime_error("Title theme needs imported SNDDATA directory");
                            const auto root=music_directory.parent_path()/"STR";
                            const auto header=readData(root/"THEMA_1.XWH"),body=readData(root/"THEMA_1.XWB"),table=readData(root/"PCM_GAIN_Q12.bin");
                            if(table.size()!=514)throw std::runtime_error("Title PCM gain table extent");
                            std::array<std::int16_t,257> gain;
                            for(unsigned i=0;i<gain.size();++i)gain[i]=std::bit_cast<std::int16_t>(std::uint16_t(table[i*2]|unsigned(table[i*2+1])<<8));
                            title_stream.start(1,header,body,gain);
                        }});
                    if(startup_textures&&startup_textures->stage()==okage::ResourceStage::LogosRetired) {
                        startup_textures->finish_title_construction();
                        //Observation stop: right after 12c350(1) returns, before the first 1296f8 update.
                        if(!resource_report.empty())okage::write_session_resource_report(*startup_textures,resource_report);
                    }
                    std::cout<<"Native title constructed frame="<<frame<<'\n';return;
                }
                if(selection==okage::GameModeSelection{8,4,0}) {
                    game_over=std::make_unique<okage::GameOverSession>(scene_clear,scene_fade,ui_shared,game_over_shared,
                        okage::GameOverConstruction{[this]{if(music)music->stop();},[this]{
                            return readData(field_root.parent_path()/"CMNDATA/2D/SCR.XPF/gameover.xim");
                        }});
                    std::cout<<"Native game-over task constructed frame="<<frame<<'\n';
                    return;
                }
                if(selection[0]!=3||battle_input.empty())
                    throw std::runtime_error("Native mode constructor unrecovered: mode="+std::to_string(selection[0])+
                        " arg="+std::to_string(selection[1])+" game_command="+std::to_string(pending_game_command[0])+
                        " frame="+std::to_string(frame));
                //149e60 clamps the signed stage and sets fade CURRENT/target0.
                const int stage=std::clamp(std::bit_cast<std::int32_t>(selection[2]),1,23);
                scene_fade.immediate(0);
                scene_clear.rgb={0,0,0};scene_fade.color=0; //100ef8 fresh resource.
                okage::initialize_scene_ui(ui_shared);
                battle=std::make_unique<okage::BattleSession>(selection[1],stage,battle_input,
                    field_root.parent_path(),variables,battle_entry,*reset_camera,effect_bytes,effect_textures,effect_preparation,
                    load_observation_trace?8192:0);
                load_observation_written=false;
            });
        }
        const bool returned_this_tick=tick_field_return(input.pressed,{input.held,128,128,input.pressed});
        if(startup&&startup->resource_live()) {
            startup->tick({input.repeat,input.pressed,{128,128,128,128}},game_over_shared,scene_fade,*reset_camera,projection,
                retiring_effects?&*retiring_effects:nullptr,ui_shared,ui_fonts,battle_capture_sine,random,
                [this](auto selection){game_mode->request(selection);
                    if(startup_textures&&startup_textures->stage()!=okage::ResourceStage::Unmodeled)
                        startup_textures->note_mode_request(selection[0],selection[1],selection[2]);
                    std::cout<<"Native startup requested selector="<<selection[1]<<" frame="<<frame<<'\n';});
            const auto& s=startup->state();const auto storage=startup->storage_observation();
            startup_trace.insert(startup_trace.end(),{frame,s.common.submode,s.common.tick_counter,
                s.common.generic_alpha_bits,s.common.generic_alpha_rate_bits,s.image_index,
                storage[0],storage[1],storage[2],storage[3],game_over_shared.image_frame_counter,random.state});
        }
        if(chapter&&chapter->resource_live()) {
            chapter->tick(pending_game_command,scene_fade,*reset_camera,projection,
                retiring_effects?&*retiring_effects:nullptr,ui_shared,ui_fonts,battle_capture_sine,random,
                game_over_shared.image_frame_counter,[this](auto selection){
                    game_mode->request(selection);
                    std::cout<<"Native chapter requests ordinary field frame="<<frame<<'\n';
                });
            const auto& s=chapter->state();
            chapter_trace.insert(chapter_trace.end(),{frame,s.counter,std::bit_cast<std::uint32_t>(scene_fade.current),
                std::bit_cast<std::uint32_t>(scene_fade.target),pending_game_command[0],game_over_shared.image_frame_counter});
        }
        if(defeat_return&&defeat_return->resource().live()) {
            defeat_return->tick([this](okage::GameModeSelection selection){
                game_mode->request(selection);
                std::cout<<"Native defeat return requested mode="<<selection[0]<<" arg="<<selection[1]<<" frame="<<frame<<'\n';
            });
            defeat_return->resource().tick(*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,
                ui_shared,ui_fonts,battle_capture_sine,random);
        }
        if(game_over&&game_over->resource_live()) {
            game_over->tick({input.repeat,input.pressed,{128,128,128,128}},game_over_shared,scene_fade,
                *reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared,
                ui_fonts,battle_capture_sine,random,[this](okage::GameModeSelection selection){
                    game_mode->request(selection);
                    std::cout<<"Native game-over requested mode="<<selection[0]<<" arg="<<selection[1]<<" frame="<<frame<<'\n';
                });
        }
        if(restore&&restore->resource_live()) {
            if(!game_data)throw std::runtime_error("Restore needs owned game tables");
            restore->tick({input.held,input.pressed,released,input.repeat,128,128},variables,*game_data,
                scene_fade,*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared,
                game_over_shared.image_frame_counter,battle_capture_sine,random,[this](unsigned id){
                    if(id!=3&&id!=4&&id!=7)throw std::runtime_error("Restore sound ID");
                    const auto bank=sound_banks.find(0);if(bank==sound_banks.end())throw std::runtime_error("Restore requires ALL bank");
                    bank->second->play(id,1.f,id==4?110:100);
                },[this](bool loaded){
                    pending_game_command={loaded?3u:4u,0,0};
                    if(loaded) {
                        const auto first=variables.begin()+0x844,end=std::find(first,first+8,0);
                        if(end==first+8)throw std::runtime_error("Restored player name lacks terminator");
                        player_name.assign(first,end);
                        const auto word=[this](unsigned at){return unsigned(variables[at])|unsigned(variables[at+1])<<8|unsigned(variables[at+2])<<16|unsigned(variables[at+3])<<24;};
                        game_mode->request({2,word(0x510),word(0x514)});
                    } else game_mode->request({8,1,0});
                    std::cout<<"Native Restore requested "<<(loaded?"saved field":"title")<<" frame="<<frame<<'\n';
                });
            const auto observation=restore->observation();restore_trace.push_back(frame);
            restore_trace.insert(restore_trace.end(),observation.begin(),observation.end());
        }
        if(name_entry&&name_entry->resource_live()) {
            if(!game_data)throw std::runtime_error("Name entry needs owned game tables");
            name_entry->tick(battle_pad,{input.held,input.pressed,released,input.repeat,128,128},variables,*game_data,
                scene_fade,*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,ui_shared,
                game_over_shared.image_frame_counter,battle_capture_sine,random,[this](unsigned id){
                    if(id!=0&&id!=3&&id!=4)throw std::runtime_error("Name entry sound ID");
                    const auto bank=sound_banks.find(0);if(bank==sound_banks.end())throw std::runtime_error("Name entry requires ALL bank");
                    bank->second->play(id,1.f,id==4?110:100);
                },[this]{
                    const auto first=variables.begin()+0x844;
                    const auto end=std::find(first,first+8,0);
                    if(end==first+8)throw std::runtime_error("Confirmed player name lacks terminator");
                    player_name.assign(first,end);
                    pending_game_command={14,1,0};game_mode->request({2,72,0});
                    std::cout<<"Native name entry requests opening field frame="<<frame<<'\n';
                });
            const auto observation=name_entry->observation();name_entry_trace.push_back(frame);
            name_entry_trace.insert(name_entry_trace.end(),observation.begin(),observation.end());
        }
        if(title&&title->resource_live()) {
            if(title_modeled(okage::ResourceStage::TitleConstructed)) {
                startup_textures->begin_title_updates();
                first_title=true;title_updates=0;
                title_checkpoint("first-update");
            }
            if(title_modeled(okage::ResourceStage::TitleActive)) {
                const auto& t=title->state();
                //Entry of the update that consumes Start in phase 10 (the original's stable-menu).
                if(t.common.mode_state==10&&(input.pressed&0x800)&&
                   std::none_of(title_checkpoint_frames.begin(),title_checkpoint_frames.end(),[](const auto& c){return c.first=="stable-menu";}))
                    title_checkpoint("stable-menu");
                char fade[11];std::snprintf(fade,sizeof fade,"0x%x",t.common.image_alpha_bits);
                title_update_log.push_back("{\"title_n\":"+std::to_string(title_updates)+",\"submode\":"+
                    std::to_string(t.common.mode_state)+",\"counter\":"+std::to_string(t.common.tick_counter)+
                    ",\"selection\":"+std::to_string(t.selection)+",\"pressed_word\":"+std::to_string(input.pressed)+
                    ",\"fade_b8\":\""+fade+"\"}");
            }
            if(first_title)++title_updates;
            title->tick({input.repeat,input.pressed,{128,128,128,128}},0x40,
                game_over_shared,scene_fade,*reset_camera,projection,retiring_effects?&*retiring_effects:nullptr,
                ui_shared,ui_fonts,battle_capture_sine,random,okage::TitleActions{
                    [this](auto selection){
                        //12a2d4 -> 124930: only Restore/New Game are modeled successors.
                        if(title_modeled(okage::ResourceStage::TitleActive)) {
                            if(selection==okage::GameModeSelection{10,0,0}||selection==okage::GameModeSelection{9,0,0})
                                startup_textures->request_title_successor(selection[0]);
                            else startup_textures->mark_unmodeled("title successor mode is not modeled");
                        }
                        game_mode->request(selection);},
                    [this](unsigned sound){
                        if(sound!=3&&sound!=7)throw std::runtime_error("Unrecovered title sound ID");
                        const auto bank=sound_banks.find(0);
                        if(bank==sound_banks.end())throw std::runtime_error("Title requires owned ALL sound bank");
                        bank->second->play(sound,1.f,sound==7?80:100);
                    },
                    [this]{okage::initialize_game_variables(variables);},
                    [this]{if(!game_data)throw std::runtime_error("Title reset requires game tables");
                        okage::refresh_game_characters(variables,*game_data,15);
                        okage::grant_and_equip_game_character(variables,*game_data,0,{131,174,240});},
                    [](unsigned){throw std::runtime_error("Native title saved-field selection is unrecovered");},
                    [](unsigned)->std::array<std::uint32_t,2>{throw std::runtime_error("Native title saved-field lookup is unrecovered");},
                    [this](const okage::TitleEffectEvent& e){
                        using Op=okage::TitleEffectEvent::Op;
                        if(e.op==Op::ReclaimArmed) {
                            if(title_modeled(okage::ResourceStage::TitleRemovalRequested))
                                startup_textures->note_effect_reclaim_armed(int(e.slot));
                            return;
                        }
                        if(!title_modeled(okage::ResourceStage::TitleActive))return;
                        if(e.op==Op::Create)startup_textures->note_effect_create(int(e.slot),e.definition);
                        //10c188: descriptor-only rebind of the title's image-list entry (hanten).
                        else if(e.op==Op::Bind)startup_textures->note_effect_bind(int(e.slot),e.element,
                            startup_textures->image_list().at(e.image_index));
                        else if(e.op==Op::RemovalRequest) {
                            title_checkpoint("removal-request");
                            startup_textures->note_effect_removal_request(int(e.slot));
                        } else startup_textures->note_effect_reclaim_armed(int(e.slot));
                    }
                });
        }
        if(battle&&battle->resource_live()) {
            try {battle->tick(retiring_effects?&*retiring_effects:nullptr,projection,scene_clear,
                {variables,*game_data,ui_party,ui_shared,battle_pad,
                 {input.held,input.pressed,released,input.repeat,128,128},scene_fade,
                 ui_fonts,battle_capture_sine,random,
                 [this]{if(battle_capture_layer)battle_capture_target=0;},battle_entry,
                 [this](bool alternate){
                     if(!retain_field_music)return;
                     throw std::runtime_error("Battle music12da90 enabled variant ownership is unrecovered: "+std::to_string(alternate));
                 },
                 [this](okage::BattleRosterSound cue){
                     auto found=sound_banks.find(0);
                     if(found==sound_banks.end())throw std::runtime_error("Battle roster sound requires the owned ALL bank");
                     const bool ready=cue==okage::BattleRosterSound::Ready;
                     found->second->play(ready?25:11,ready?.8f:1.f,ready?100:80);
                 },
                 [this](unsigned id){
                     if(id!=0&&id!=3&&id!=4)throw std::runtime_error("Battle menu sound ID outside recovered table scope");
                     auto found=sound_banks.find(0);
                     if(found==sound_banks.end())throw std::runtime_error("Battle menu sound requires the owned ALL bank");
                     //12df80: owned1fb3e0 speeds1; signed1fb388 volumes100/100/110.
                     found->second->play(id,1.f,id==4?110:100);
                 },battle_live_trace,[this](int id){
                     if(id!=18&&id!=40&&id!=58&&id!=65&&id!=66)
                         throw std::runtime_error("Battle strike sound outside recovered12df80 table scope: "+std::to_string(id));
                     auto found=sound_banks.find(0);
                     if(found==sound_banks.end())throw std::runtime_error("Battle strike sound requires the owned ALL bank");
                     //12df80 owned tables1fb3e0/1fb388: speeds1, volumes
                     //18=120,40=100,58=110,65=80,66=100.
                     found->second->play(unsigned(id),1.f,id==18?120:id==58?110:id==65?80:100);
                 },[this]{
                     if(!music)throw std::runtime_error("Victory requires live music owner");
                     music->stop();
                 },[this](unsigned slot){
                     if(!music||slot!=4)throw std::runtime_error("Victory jingle owner/slot unavailable");
                     music->select(33); //12e0b0(4), owned installed jingle slot4.
                 },[this](std::array<std::uint32_t,3> command){pending_game_command=command;},
                 [this](okage::GameModeSelection selection){
                     if(!game_mode)throw std::runtime_error("Battle mode-request owner missing");
                     game_mode->request(selection);
                     std::cout<<"Native battle wrapper requested mode="<<selection[0]<<" frame="<<frame<<'\n';
                 }},
                frame,battle_stage_report,battle_ui_update_report,deferred_battle_readback);
                pad_repeat.delay=battle_pad.controls[0].delay;
                pad_repeat.interval=battle_pad.controls[0].interval;
            }
            catch(const std::exception& error) {
                pad_repeat.delay=battle_pad.controls[0].delay;
                pad_repeat.interval=battle_pad.controls[0].interval;
                throw std::runtime_error("Native battle frame="+std::to_string(frame)+": "+error.what());
            }
            battle_loading_trace.push_back(frame);
            const auto observation=battle->observation();
            battle_loading_trace.insert(battle_loading_trace.end(),observation.begin(),observation.end());
        }
        const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
        if(field) {
            if(field_save&&field_save->dead()) {
                field_save.reset();save_background.reset();save_capture_surface.reset();field->close_save_menu();
                pending_game_command={0,0,0};scene_fade.request(1.f);
                std::cout<<"Native field Save retired frame="<<frame<<'\n';
            }
            try {if(!returned_this_tick)field->tick({input.held,128,128,input.pressed});}
            catch(const std::exception& e) {
                throw std::runtime_error("Native frame="+std::to_string(frame)+" field PC="+
                    std::to_string(field->pc())+": "+e.what());
            }
            if(const auto& encounter=field->encounter();encounter&&field->battle_request()&&
               field->battle_request()->phase==encounter->mode)
                std::cout<<"Native natural encounter frame="<<frame<<" row="<<encounter->row
                    <<" column="<<encounter->column<<" formation="<<encounter->formation
                    <<" phase="<<encounter->mode<<" rear="<<encounter->rear_attack<<'\n';
            if(const auto& battle=field->battle_request()) {
                const unsigned phase=battle->phase?battle->phase:(field->exploration_ready()?0u:5u);
                battle_transition_trace.insert(battle_transition_trace.end(),{frame,phase,battle->formation,
                    std::bit_cast<std::uint32_t>(battle->stage),battle->return_token,unsigned(battle->dispatched),
                    field->pc(),unsigned(field->event_running())});
            }
            const auto request=field->transition();
            transition_trace.insert(transition_trace.end(),{frame,unsigned(current_scene),unsigned(current_entrance),
                request?unsigned(request->scene):0xffffffffu,request?request->entrance:0,
                request?request->countdown:0,request&&request->ready?1u:0u,field->pc()});
            if(request&&request->ready) {
                try {
                    if(pending_game_command[0]==14) {
                        if(!game_mode||request->scene<0||field->persistent_map_index()!=-1)
                            throw std::runtime_error("Chapter transition requires recovered destination/minimap state");
                        game_mode->request({2,std::uint32_t(request->scene),request->entrance});
                    } else load_destination(*request,{input.held,128,128,input.pressed});
                }
                catch(const std::exception& e) {
                    throw std::runtime_error("Native frame="+std::to_string(frame)+" destination scene="+
                        std::to_string(request->scene)+" entrance="+std::to_string(request->entrance)+": "+e.what());
                }
            }
        }
        if(field_save) {
            if(!game_data||!save_background||!save_capture_surface)throw std::runtime_error("Save field owners missing");
            auto background=save_open_frame==frame?base_surface():*save_background;
            if(save_open_frame!=frame) {
                save_capture_surface->apply_frozen(background,*save_background,true);
            }
            field_save->tick_save({input.held,input.pressed,released,input.repeat,128,128},variables,*game_data,
                scene_fade,background,battle_capture_sine,random,[this](unsigned id){
                    const auto bank=sound_banks.find(0);if(bank==sound_banks.end())throw std::runtime_error("Save requires ALL sound bank");
                    bank->second->play(id,1.f,id==4?110:100);
                },[this](bool written){pending_game_command={written?3u:4u,0,0};});
            if(save_open_frame+1==frame) {
                if(!completed_game_image||completed_game_frame+1!=frame)
                    throw std::runtime_error("Save freeze requires consecutive framebuffer history");
                save_background=*completed_game_image;
            }
            save_trace.push_back(frame);const auto state=field_save->observation();save_trace.insert(save_trace.end(),state.begin(),state.end());
        }
        if(title&&title->resource_live()) {
            const auto& t=title->state();const auto& c=t.common;
            title_trace.insert(title_trace.end(),{frame,c.submode,c.mode_state,c.tick_counter,c.pressed_word,
                c.generic_alpha_bits,c.generic_alpha_rate_bits,c.image_alpha_bits,t.yaw,t.selection});
            for(const auto* values:{&t.x,&t.y,&t.alpha,&t.visible})title_trace.insert(title_trace.end(),values->begin(),values->end());
            title_trace.insert(title_trace.end(),{t.pulse,t.pulse_rising,t.pulse_channel,t.water,t.texture});
        }
        title_stream.advance_main_loop();
        if(music||title_stream.state().registered){
            std::array<std::int16_t,1600> pcm{};if(music)music->render(pcm);
            title_stream.mix(pcm);
            std::array<std::int32_t,1600> stream{};
            for(auto& [bank,player]:sound_banks)player->mix(stream);
            for(unsigned i=0;i<pcm.size();++i)pcm[i]=std::clamp(int(pcm[i])+stream[i],-32768,32767);
            if(audio_output)audio_output->submit(pcm);
            if(record_audio){audio_capture.insert(audio_capture.end(),pcm.begin(),pcm.end());
                music_trace.insert(music_trace.end(),{frame,music?unsigned(music->volume.current):0,music?unsigned(music->volume.target):0,
                    music?unsigned(music->volume.step):0,music&&music->player()?music->player()->peak_voices():0});}
        }
        if(!game_mode && !release_frame && okage::poll_dialogue_wait(wait,2,{true,0,layers,std::int32_t(text.state.progress)})==0)release_frame=frame;
        if(!game_mode||field) {
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
        choices.update(dialogue_active && !closure.finished() ? text.state.replacement : "",text.choice_cursor,controller.choice_armed,player_name);
        choice_trace.insert(choice_trace.end(),{frame,unsigned(message_id),unsigned(text.position().fragment),unsigned(text.position().cursor),
            unsigned(text.choice_cursor),unsigned(text.choice_result),controller.choice_active?1u:0u,controller.choice_armed?1u:0u,
            input.held,input.pressed,input.repeat,choices.alpha});
        narration.update(dialogue_active && !closure.finished() ? text.state.description : "",fonts[2],player_name);
        dialogue_trace.insert(dialogue_trace.end(),{frame,std::uint32_t(message_id),text.state.flags,text.state.phase,
            text.state.period,text.state.delay,text.state.progress,controller.advance_count,controller.layer_advance,
            controller.layer_flags,controller.stage,std::bit_cast<std::uint32_t>(opacity.delta)});
        }
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
                field->event_running()?1u:0u,std::bit_cast<std::uint32_t>(field->blend.phase),
                std::bit_cast<std::uint32_t>(field->blend.step)});
            scene_trace.insert(scene_trace.end(),{frame,field->pc(),field->pending_opcode,release_frame});
            for(const auto& values:{field->position,field->target,field->rotation})
                for(float v:values)scene_trace.push_back(std::bit_cast<std::uint32_t>(v));
            if(effects_loaded) {
                const auto& submitted=field->effect_submissions();
                effect_submission_trace.insert(effect_submission_trace.end(),{frame,unsigned(submitted.size())});
                for(const auto& draw:submitted) {
                    if(const auto* effect=std::get_if<okage::MapEffect>(&draw.effect))
                        effect_submission_trace.insert(effect_submission_trace.end(),{draw.handle,effect->id,effect->flags,effect->phase});
                    else {
                        const auto& bubble=std::get<okage::AttachedEffect>(draw.effect).animation;
                        effect_submission_trace.insert(effect_submission_trace.end(),{draw.handle,30,bubble.flags,bubble.phase});
                    }
                }
                const auto& jobs=field->map_effects();unsigned jobs_count=0;for(const auto& e:jobs)if(e)++jobs_count;
                map_effect_trace.insert(map_effect_trace.end(),{frame,jobs_count});
                for(unsigned i=0;i<jobs.size();++i)if(const auto& e=jobs[i]) {
                    map_effect_trace.insert(map_effect_trace.end(),{i,e->id,e->flags,e->phase,e->duration,unsigned(e->elements.size())});
                    for(const auto& v:{e->position,e->rotation,e->scale,e->color})for(float f:v)map_effect_trace.push_back(std::bit_cast<std::uint32_t>(f));
                    for(const auto& element:e->elements) {
                        map_effect_trace.insert(map_effect_trace.end(),{element.id,element.program,element.texture,element.render_flags});
                        for(const auto& v:element.rendered)for(float f:v)map_effect_trace.push_back(std::bit_cast<std::uint32_t>(f));
                    }
                }
                const auto& all=field->effects();unsigned count=0;for(const auto& e:all)if(e)++count;
                effect_trace.insert(effect_trace.end(),{frame,field->pc(),count});
                for(unsigned i=0;i<all.size();++i)if(const auto& e=all[i]) {
                    effect_trace.insert(effect_trace.end(),{i,unsigned(e->actor_alias),e->animation.phase,e->animation.flags});
                    for(float f:e->position)effect_trace.push_back(std::bit_cast<std::uint32_t>(f));
                    for(const auto& s:e->animation.sprites) {
                        for(const auto& v:{s.position,s.scale})for(float f:v)effect_trace.push_back(std::bit_cast<std::uint32_t>(f));
                        effect_trace.push_back(s.texture);effect_trace.push_back(s.uv[0]|(std::uint32_t(s.uv[1])<<16));effect_trace.push_back(s.uv[2]|(std::uint32_t(s.uv[3])<<16));
                    }
                }
            }
        }
        if(field&&field->player_shadow()) {
            const auto& shadow=*field->player_shadow();
            shadow_trace.insert(shadow_trace.end(),{frame,field->player_movement().mask,shadow.phase,shadow.locked,
                shadow.follow_disabled,std::bit_cast<std::uint32_t>(shadow.voice),std::bit_cast<std::uint32_t>(shadow.factor),
                shadow.hero_shadow,unsigned(shadow.stun_visible),std::bit_cast<std::uint32_t>(shadow.projection_factor)});
            for(const auto& v:{shadow.stun_rotation,shadow.stun_position,shadow.stun_scale})
                for(float f:v)shadow_trace.push_back(std::bit_cast<std::uint32_t>(f));
            const auto hero=std::find_if(actor_bindings.begin(),actor_bindings.end(),[](const auto& a){return a.alias==-1;})->actor;
            const auto hero_channels=hero->animation().values();
            for(unsigned ch:{0u,1u})for(float f:hero_channels.at(ch))shadow_trace.push_back(std::bit_cast<std::uint32_t>(f));
        }
        for(const auto& binding:actor_bindings) {
            const auto* actor=binding.actor;auto& actor_trace=actor_traces[binding.alias];
            actor_trace.insert(actor_trace.end(),{frame,field->pc(),actor->animation().pc(),actor->animation().script_state(),
                actor->motion(),std::uint32_t(actor->channels().size()),std::uint32_t(actor->textures().images().size()),actor->visible ? 1u : 0u});
            for(const auto& channel:actor->channels())for(float v:channel)actor_trace.push_back(std::bit_cast<std::uint32_t>(v));
            for(auto image:actor->textures().images())actor_trace.push_back(std::bit_cast<std::uint32_t>(image));
        }
        if(field) {
            const auto& p=projection.frame();
            projection_trace.insert(projection_trace.end(),{frame,unsigned(current_scene),unsigned(projection.frames()),
                std::bit_cast<std::uint32_t>(projection.effect_near())});
            for(const auto* s:{&p.shadow,&p.ordinary,&p.field_end,&p.effects,&p.deferred,&p.ending})
                for(float f:okage::field_projection_words(*s))projection_trace.push_back(std::bit_cast<std::uint32_t>(f));
        }
        observe_game_mode(1);
        if(battle_capture_layer) {
            battle_capture_draw=battle_capture_layer->draw(true,battle_capture_sine);
            const auto& draw=*battle_capture_draw;
            battle_capture_trace.insert(battle_capture_trace.end(),{frame,battle_capture_frame,
                completed_game_frame,1,draw.mode,draw.rgba,draw.scale,draw.count,
                battle_capture_layer->mode,battle_capture_layer->rgba,battle_capture_layer->progress});
        }
        const bool render_tick=bool(display_link)||field_save||(field&&field->save_requested())||(field&&field->battle_request())||battle_capture_layer||
            okage::native_render_tick(frame,render_from,render_until,render_ranges);
        if(field) {
            const auto& filter=field->screen_filter;
            filter_trace.insert(filter_trace.end(),{frame,filter.updating?1u:0u,filter.setting,filter.alpha});
            if(filter.alpha&&!render_tick)
                throw std::runtime_error("Active feedback needs consecutive rendering at native frame="+std::to_string(frame)+
                    " scene="+std::to_string(current_scene)+" event_pc="+std::to_string(field->pc())+
                    "; extend the headless render interval or --render-range");
        }
        prepared_tick=true;prepared_render_tick=render_tick;
        if(battle||pending_battle||display_link)frame_event("prepared",pending_battle?pending_battle->completion->frame():0);
        return finish_prepared_tick(block);
    }
    bool finish_prepared_tick(bool block) {
        if(!prepared_tick)return true;
        if(pending_battle) {
            if(pending_battle->completion->frame()+1!=frame)
                throw std::runtime_error("Prepared tick lost preceding GPU frame");
            if(!pending_battle->completion->ready()) {
                open_loader_window();
                if(!block) {frame_event("gpu_yield",pending_battle->completion->frame());return false;}
            }
            complete_battle_frame();
        }
        if(display_link) {
            if(!pending_display_sequence) {
                pending_display_sequence=display_link->sequence();
                frame_event("display_armed",0);
                open_loader_window();
            }
            if(!display_link->ready_after(*pending_display_sequence)) {
                if(!block) {frame_event("display_yield",0);return false;}
                display_link->wait_after(*pending_display_sequence);
            }
            if(load_observation_trace)
                display_gates.push_back({frame,*pending_display_sequence,display_link->sequence()});
            frame_event("display_passed",0);
            pending_display_sequence.reset();
        }
        // A resumed callback must park before current submission/continuations.
        close_loader_window();
        if(battle&&battle->frame_prepared()) {
            auto composition=deferred_battle_readback
                ? std::make_shared<const okage::FrameComposition>(capture_composition())
                : std::shared_ptr<const okage::FrameComposition>{};
            auto completion=battle->submit_frame(frame,deferred_battle_readback);
            if(composition)pending_battle.emplace(PendingBattleFrame{std::move(completion),std::move(composition)});
            frame_event("submitted",frame);
        }
        if(prepared_render_tick&&!pending_battle_frame())render_frame();
        service_field_return_loader();
        if(battle) {
            if(!game_data)throw std::runtime_error("Battle construction requires owned game tables");
            try {battle->service_loader_after_frame({variables,*game_data,battle_entry,random},frame,battle_construction_report,loading_policy!=LoadingPolicy::Continuous);}
            catch(const std::exception& error) {
                throw std::runtime_error("Native battle construction frame="+std::to_string(frame)+": "+error.what());
            }
            if(load_observation_trace&&!load_observation_written&&battle->observation()[2]==6)
                write_load_observation("constructed");
        }
        playtime.main_loop_pass(variables);
        if(input_journal)input_journal->complete_tick(frame);
        prepared_tick=false;
        if(battle||pending_battle||display_link)frame_event("tick_complete",frame);
        return true;
    }
    void publish_composed(okage::ComposedFrame result) {
        feedback.retain(result.diagnostic,result.frame,224);
        if(result.retain_game) {
            completed_game_image=result.diagnostic;completed_game_image->height=224;
            completed_game_image->rgba.resize(640*224*4);completed_game_frame=result.frame;
        }
        if(result.live)live_image=std::move(result.live);
        rendered_surface=std::move(result.diagnostic);rendered_frame=result.frame;
    }
    void complete_battle_frame() {
        if(!pending_battle)return;
        auto& pending=*pending_battle;
        const auto image_frame=pending.completion->frame();
        if(image_frame!=pending.composition->frame)
            throw std::runtime_error("Battle GPU/compositor frame mismatch");
        try {
            const auto& image=pending.completion->complete();
            close_loader_window();
            auto result=pending.composition->compose(image);
            publish_composed(std::move(result));
        } catch(const std::exception& e) {
            throw std::runtime_error("Native battle GPU/composition frame="+std::to_string(image_frame)+": "+e.what());
        }
        frame_event("published",image_frame);
        pending_battle.reset();
    }
    void render_frame() {
        if(pending_battle) {complete_battle_frame();return;}
        if(rendered_surface&&rendered_frame==frame)return;
        publish_composed(capture_composition().compose(base_surface()));
    }
    std::pair<okage::Texture,unsigned> presentation() {
        if((pending_battle||prepared_tick)&&!rendered_surface)finish_battle_frame();
        const bool pending=pending_battle||prepared_tick;
        const auto diagnostic=pending?*rendered_surface:surface();
        const auto displayed_frame=pending?rendered_frame:frame;
        if(pending&&(displayed_frame>frame||frame-displayed_frame>(prepared_tick?2u:1u)))
            throw std::runtime_error("Deferred presentation lost its completed frame history");
        return {game_mode&&live_image?*live_image:diagnostic,displayed_frame};
    }
    okage::FrameComposition capture_composition() const {
        okage::FrameComposition snapshot;
        snapshot.frame=frame;
        snapshot.game_view=bool(renderer)||bool(game_mode);
        snapshot.retain_game=bool(battle_capture_image)||field_save||(field&&(field->battle_request()||field->save_requested()));
        snapshot.dialogue=okage::rasterize_text(fonts,text.glyphs,640,224);
        if(!snapshot.game_view)return snapshot;
        snapshot.live_enabled=live_presentation_enabled;
        //Layer2 OT40e precedes results UI OT40f; results already applied it.
        snapshot.apply_fade=!(field_save&&field_save->rendered_image())&&!(field_return&&field_return->rendered)&&!(title&&title->rendered_image())&&!(name_entry&&name_entry->rendered_image())&&!(restore&&restore->rendered_image());
        snapshot.fade=scene_fade;
        if(choices.highlighted)snapshot.choice_cursor=choices.cursor;
        auto glyphs=narration.glyphs(fonts[2]);const auto choiceGlyphs=choices.glyphs(fonts[2]);
        glyphs.insert(glyphs.end(),choiceGlyphs.begin(),choiceGlyphs.end());
        snapshot.narration=okage::rasterize_text(fonts,glyphs,640,224);
        if(dialogue_active&&controller.window_type!=2) {
            snapshot.dialogue_alpha=std::clamp(opacity.value/128.f,0.f,1.f);
            if(snapshot.live_enabled) {
                auto live_glyphs=text.glyphs;
                for(auto& glyph:live_glyphs) {
                    if(glyph.x>INT32_MAX-25||glyph.y>INT32_MAX-(332-24))
                        throw std::runtime_error("Live dialogue placement overflow");
                    glyph.x+=25;glyph.y+=332-24;
                }
                snapshot.live_dialogue=okage::rasterize_text(fonts,live_glyphs,640,448);
            }
        }
        snapshot.transition_alpha=transition_alpha;
        if(transition_alpha) {
            snapshot.transition_diagnostic=transition_image;
            if(snapshot.live_enabled)snapshot.transition_live=transition_live_image;
        }
        if(battle_capture_draw) {
            if(!battle_capture_surface||!battle_capture_image||!completed_game_image||completed_game_frame+1!=frame)
                throw std::runtime_error("Battle capture lost consecutive native framebuffer history");
            snapshot.capture_pass=[owner=battle_capture_surface,capture=*battle_capture_image,
                                   history=*completed_game_image,draw=*battle_capture_draw](okage::Texture& image) {
                owner->apply(image,capture,history,draw);
            };
        }
        return snapshot;
    }
    okage::Texture surface() const {
        if(pending_battle_frame()||prepared_tick||(battle&&battle->frame_prepared()))
            throw std::runtime_error("Surface composition before battle frame completion");
        if(rendered_surface&&rendered_frame==frame)return *rendered_surface;
        auto result=capture_composition().compose(base_surface());
        if(result.live)live_image=std::move(result.live);
        return std::move(result.diagnostic);
    }
    okage::Texture base_surface() const {
        if(pending_battle_frame()||(battle&&battle->frame_prepared()))
            throw std::runtime_error("Scene readback before battle frame completion");
        if(!renderer&&!game_mode)return {};
        okage::Texture image{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
        if(field_save&&field_save->rendered_image()) {
            image=*field_save->rendered_image();
        } else if(renderer) {
        if(!field)throw std::runtime_error("Field renderer has no mode owner");
        const auto& r=field->rotation;
        const auto& screen=projection.frame().ordinary;
        image=renderer->render(field->view(),okage::field_clip_projection(screen),effects_loaded?&field->effects():nullptr,{-r[0],-r[1],-r[2],0},screen.clip_far,field->position,&projection.frame(),field->player_shadow()?field->player_shadow()->projection_factor:1);
        try {feedback.apply(image,frame,field->screen_filter.alpha);}
        catch(const std::exception& e) {
            throw std::runtime_error("Feedback at native frame="+std::to_string(frame)+" scene="+
                std::to_string(current_scene)+" event_pc="+std::to_string(field->pc())+": "+e.what());
        }
        } else if(field_return&&field_return->rendered) {
            image=*field_return->rendered;
        } else if(battle&&battle->rendered_image()) {
            image=*battle->rendered_image();
        } else if(defeat_return&&defeat_return->resource().rendered_image()) {
            image=*defeat_return->resource().rendered_image();
        } else if(restore&&restore->rendered_image()) {
            image=*restore->rendered_image();
        } else if(name_entry&&name_entry->rendered_image()) {
            image=*name_entry->rendered_image();
        } else if(chapter&&chapter->rendered_image()) {
            image=*chapter->rendered_image();
        } else if(startup&&startup->rendered_image()) {
            image=*startup->rendered_image();
        } else if(title&&title->rendered_image()) {
            image=*title->rendered_image();
        } else if(game_over&&game_over->rendered_image()) {
            image=*game_over->rendered_image();
        } else {
            for(std::size_t i=0;i<image.rgba.size();i+=4) {
                for(unsigned c=0;c<3;++c)image.rgba[i+c]=scene_clear.rgb[c];
                image.rgba[i+3]=(battle&&battle->resource_live())||field_return?64:0;
            }
        }
        return image;
    }
    std::string headless_status_json() const {
        std::ostringstream out;
        out<<std::setprecision(9)<<"{\"frame\":"<<frame<<",\"scene\":";
        if(field)out<<current_scene;else out<<"null";
        out<<",\"entrance\":";if(field)out<<current_entrance;else out<<"null";
        out<<",\"event_running\":"<<(field&&field->event_running()?"true":"false")
           <<",\"pc\":";if(field)out<<field->pc();else out<<"null";
        out<<",\"message\":"<<message_id<<",\"choice\":"<<text.choice_cursor
           <<",\"camera_yaw\":";
        const auto number=[&](float value){if(std::isfinite(value))out<<value;else out<<"null";};
        if(field)number(field->rotation[1]);else out<<"null";
        out<<",\"hero_position\":";
        const auto hero=std::find_if(actor_bindings.begin(),actor_bindings.end(),
            [](const auto& binding){return binding.alias==-1;});
        if(field&&hero!=actor_bindings.end()&&hero->actor&&hero->actor->channels().size()>1) {
            const auto& p=hero->actor->channels()[1];out<<'[';
            for(unsigned i=0;i<3;++i){if(i)out<<',';number(p[i]);}out<<']';
        } else out<<"null";
        out<<",\"guard_pending_opcode\":";
        if(field)out<<field->pending_opcode;else out<<"null";
        out<<'}';
        return out.str();
    }
    okage::Texture headless_live_view() const {
        if(prepared_tick||pending_battle_frame()||(battle&&battle->frame_prepared()))
            throw std::runtime_error("Cannot view an uncompleted native frame");
        if(live_image&&rendered_frame==frame) {
            if(live_image->width!=640||live_image->height!=448||
               live_image->rgba.size()!=std::size_t(640)*448*4)
                throw std::runtime_error("Published native live view has an invalid 640x448 extent");
            return *live_image;
        }
        auto composition=capture_composition();
        if(!composition.live_enabled)
            throw std::runtime_error("Headless live compositor was not enabled before the prelude");
        auto result=composition.compose(base_surface());
        if(!result.live||result.live->width!=640||result.live->height!=448||
           result.live->rgba.size()!=std::size_t(640)*448*4)
            throw std::runtime_error("Native live view has an invalid 640x448 extent");
        return std::move(*result.live);
    }
};
static NSBitmapImageRep* bitmap(const okage::Texture& image) {
    NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image.width pixelsHigh:image.height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:image.width*4 bitsPerPixel:32];
    if(!rep)throw std::runtime_error("Cannot allocate native text bitmap");
    std::memcpy(rep.bitmapData,image.rgba.data(),image.rgba.size());return rep;
}
static void captureImage(Session& session,const fs::path& path) {
    session.finish_battle_frame();
    if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing native frame image");
    const auto data=[bitmap(session.surface()) representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSError* error=nil;
    if(!data || ![data writeToFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataWritingWithoutOverwriting error:&error])
        throw std::runtime_error("Cannot write native frame image");
}
static void writeView(Session& session,const fs::path& path) {
    if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing native live view");
    const auto image=session.headless_live_view();
    const auto data=[bitmap(image) representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSError* error=nil;
    if(!data || ![data writeToFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataWritingWithoutOverwriting error:&error])
        throw std::runtime_error("Cannot write native live view");
}
static void reserve_observation_name(std::set<std::string>& names,const std::string& name) {
    if(!names.insert(name).second)
        throw std::runtime_error("Refusing reused headless observation name");
}
static void capture(Session& session,const fs::path& path) {
    session.finish_battle_frame();
    const auto battlePath=fs::path(path.string()+".battle-root.bin");
    if(session.battle&&(fs::exists(battlePath)||fs::is_symlink(battlePath)))
        throw std::runtime_error("Refusing existing battle root capture");
    auto statePath=fs::path(path.string()+".json");
    const auto actorsPath=fs::path(path.string()+".actors-state.json");
    const auto shadowsPath=fs::path(path.string()+".shadow-submissions.json");
    if(fs::exists(path)||fs::is_symlink(path)||fs::exists(statePath)||fs::is_symlink(statePath))throw std::runtime_error("Refusing existing native capture");
    if(fs::exists(actorsPath)||fs::is_symlink(actorsPath))throw std::runtime_error("Refusing existing actor-state capture");
    if(fs::exists(shadowsPath)||fs::is_symlink(shadowsPath))throw std::runtime_error("Refusing existing shadow-submission capture");
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());
    if(session.battle) {
        const auto battleActorsPath=fs::path(path.string()+".battle-actors.json");
        if(fs::exists(battleActorsPath)||fs::is_symlink(battleActorsPath))throw std::runtime_error("Refusing existing battle actor observation");
        std::ofstream battleActorsOut(battleActorsPath);
        session.battle->write_actor_observation(battleActorsOut,session.frame);
        const auto effectsPath=fs::path(path.string()+".battle-effects.json");
        if(fs::exists(effectsPath)||fs::is_symlink(effectsPath))throw std::runtime_error("Refusing existing battle effect observation");
        std::ofstream effectsOut(effectsPath);
        session.battle->write_effect_observation(effectsOut,session.frame);
        const auto historyPath=fs::path(path.string()+".battle-history.json");
        if(fs::exists(historyPath)||fs::is_symlink(historyPath))throw std::runtime_error("Refusing existing battle history observation");
        std::ofstream historyOut(historyPath);
        const auto& hud=session.ui_shared.hud;
        historyOut<<"{\"schema\":\"okage-native-battle-history-v1\",\"frame\":"<<session.frame
                  <<",\"cursor\":"<<hud.type3_history_cursor<<",\"animation\":"<<hud.type3_ready<<",\"slots\":[";
        for(unsigned i=0;i<hud.type3_slots.size();++i){if(i)historyOut<<',';historyOut<<hud.type3_slots[i];}
        historyOut<<"],\"current_number_bytes\":[";
        for(unsigned i=0;i<hud.type3_current_number.size();++i){if(i)historyOut<<',';historyOut<<unsigned(std::uint8_t(hud.type3_current_number[i]));}
        historyOut<<"]}\n";
        if(!historyOut)throw std::runtime_error("Cannot write battle history observation");
        const auto& bytes=session.battle->state().bytes;
        std::ofstream root(battlePath,std::ios::binary);
        if(!root.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()))
            throw std::runtime_error("Cannot write battle root capture");
    }
    const auto data=[bitmap(session.surface()) representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    std::ofstream out(path,std::ios::binary);if(!data || !out.write(static_cast<const char*>(data.bytes),data.length))throw std::runtime_error("Cannot write native capture");
    if(session.battle_capture_image) {
        const auto frozen=fs::path(path.string()+".battle-frozen.rgba");
        if(fs::exists(frozen)||fs::is_symlink(frozen))throw std::runtime_error("Refusing existing frozen-frame capture");
        std::ofstream pixels(frozen,std::ios::binary);
        const auto& rgba=session.battle_capture_image->rgba;
        if(!pixels.write(reinterpret_cast<const char*>(rgba.data()),rgba.size()))
            throw std::runtime_error("Cannot write native frozen-frame capture");
    }
    if(session.battle_return) {
        const auto output=fs::path(path.string()+".battle-return.bin");
        if(fs::exists(output)||fs::is_symlink(output))throw std::runtime_error("Refusing existing battle return capture");
        const auto& saved=*session.battle_return;
        std::vector<std::uint32_t> words={std::bit_cast<std::uint32_t>(saved.scene),
            std::bit_cast<std::uint32_t>(saved.secondary_music),unsigned(session.retain_field_music)};
        words.insert(words.end(),session.pending_game_command.begin(),session.pending_game_command.end());
        if(!session.game_mode)throw std::runtime_error("Battle return capture has no mode parent");
        const auto selection=session.game_mode->requested().value_or(session.game_mode->current());
        words.insert(words.end(),selection.begin(),selection.end());
        for(const auto& v:{saved.player_position,saved.player_rotation,saved.raw_camera_position})
            for(float f:v)words.push_back(std::bit_cast<std::uint32_t>(f));
        std::ofstream return_state(output,std::ios::binary);
        const auto& context=session.battle_entry.bytes();
        if(!return_state.write(reinterpret_cast<const char*>(words.data()),words.size()*4)||
           !return_state.write(reinterpret_cast<const char*>(context.data()),context.size()))
            throw std::runtime_error("Cannot write battle return state");
    }
    std::ofstream state(statePath);
    if(!session.wander_trace.empty()) {
        const auto output=fs::path(path.string()+".wander.bin");
        if(fs::exists(output)||fs::is_symlink(output))throw std::runtime_error("Refusing existing wander capture");
        std::ofstream wander(output,std::ios::binary);
        if(!wander.write(reinterpret_cast<const char*>(session.wander_trace.data()),session.wander_trace.size()*4))
            throw std::runtime_error("Cannot write wander capture");
    }
    if(!session.projection_trace.empty()) {
        std::ofstream projection(path.string()+".projection.bin",std::ios::binary);
        if(!projection.write(reinterpret_cast<const char*>(session.projection_trace.data()),session.projection_trace.size()*4))
            throw std::runtime_error("Cannot write field projection trace");
    }
    if(!session.shadow_trace.empty()) {
        std::ofstream shadow(path.string()+".shadow.bin",std::ios::binary);
        if(!shadow.write(reinterpret_cast<const char*>(session.shadow_trace.data()),session.shadow_trace.size()*4))
            throw std::runtime_error("Cannot write player shadow trace");
    }
    if(session.effects_loaded) {
        std::ofstream submissions(path.string()+".effect-submissions.bin",std::ios::binary);
        if(!submissions.write(reinterpret_cast<const char*>(session.effect_submission_trace.data()),session.effect_submission_trace.size()*4))throw std::runtime_error("Cannot write effect submission trace");
        std::ofstream map_effects(path.string()+".map-effects.bin",std::ios::binary);
        if(!map_effects.write(reinterpret_cast<const char*>(session.map_effect_trace.data()),session.map_effect_trace.size()*4))throw std::runtime_error("Cannot write map effect trace");
        std::ofstream effects(path.string()+".effects.bin",std::ios::binary);
        if(!effects.write(reinterpret_cast<const char*>(session.effect_trace.data()),session.effect_trace.size()*4))throw std::runtime_error("Cannot write effect trace");
    }
    state<<"{\n  \"frames\": "<<session.frame<<",\n  \"message_id\": "<<session.message_id
         <<",\n  \"rng_state\": "<<session.random.state
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
         <<",\n  \"field_present\": "<<(session.field?"true":"false")
         <<",\n  \"game_mode\": "<<(session.game_mode?session.game_mode->current()[0]:0)
         <<",\n  \"game_mode_parent_phase\": "<<(session.game_mode?session.game_mode->phase():0)
         <<",\n  \"game_mode_dead_allocation\": "<<(session.game_mode&&session.game_mode->has_dead_allocation()?"true":"false")
         <<",\n  \"game_over_resource_live\": "<<(session.game_over&&session.game_over->resource_live()?"true":"false")
         <<",\n  \"game_over_state\": "<<(session.game_over?session.game_over->state().mode_state:0)
         <<",\n  \"game_over_ticks\": "<<(session.game_over?session.game_over->state().tick_counter:0)
         <<",\n  \"game_over_image_alpha_bits\": "<<(session.game_over?session.game_over->state().image_alpha_bits:0)
         <<",\n  \"field_camera_mode\": "<<(session.field?session.field->camera_mode():0)
         <<",\n  \"interaction_alias\": "<<(session.field?session.field->interaction_alias():0)
         <<",\n  \"interaction_event\": "<<(session.field?session.field->interaction_event():0)
         <<",\n  \"loaded_stream_banks\": "<<session.sound_banks.size()
         <<",\n  \"choice_cursor\": "<<session.text.choice_cursor
         <<",\n  \"choice_result\": "<<session.text.choice_result
         <<",\n  \"choice_armed\": "<<(session.controller.choice_armed ? "true" : "false")
         <<",\n  \"battle_capture_frame\": "<<session.battle_capture_frame
         <<",\n  \"battle_followers_saved\": "<<(session.battle_followers_saved?"true":"false")
         <<",\n  \"battle_capture_alpha\": "<<session.battle_capture_alpha
         <<",\n  \"battle_capture_target\": "<<session.battle_capture_target
         <<",\n  \"close_frame\": "<<session.close_frame<<",\n  \"close_wait_release_frame\": "<<session.close_release_frame<<"\n}\n";
    if(!state)throw std::runtime_error("Cannot write native capture state");
    std::ofstream actors(actorsPath);
    actors<<"{\n  \"version\": 1,\n  \"frame\": "<<session.frame<<",\n  \"actors\": [";
    bool firstActor=true;
    for(const auto& binding:session.actor_bindings) {
        const auto& actor=*binding.actor;
        actors<<(firstActor ? "\n" : ",\n")<<"    {\"alias\": "<<binding.alias
              <<", \"instance\": "<<binding.instance
              <<", \"visible\": "<<(actor.visible ? "true" : "false")
              <<", \"render_flags\": "<<actor.render_flags<<", \"shadow_mode_override\": ";
        if(actor.shadow_mode_override)actors<<*actor.shadow_mode_override;else actors<<"null";
        actors<<", \"raw_channel_words\": [";
        bool firstWord=true;
        for(const auto& channel:actor.animation().channel_outputs())for(float value:channel) {
            if(!firstWord)actors<<',';
            actors<<std::bit_cast<std::uint32_t>(value);firstWord=false;
        }
        actors<<"]}";firstActor=false;
    }
    actors<<"\n  ],\n  \"map_channel_groups\": [";
    bool firstGroup=true;
    if(session.map_objects)for(int instance=0;instance<640;++instance) {
        const auto* group=session.map_objects->channel_group(instance);
        if(!group)continue;
        actors<<(firstGroup?"\n":",\n")<<"    {\"instance\": "<<instance
              <<", \"base\": "<<group->channel_base()<<", \"count\": "<<group->channel_count()
              <<", \"output_words\": [";
        bool first=true;
        for(const auto& channel:group->channel_outputs())for(float value:channel) {
            if(!first)actors<<',';
            actors<<std::bit_cast<std::uint32_t>(value);first=false;
        }
        actors<<"]}";firstGroup=false;
    }
    actors<<"\n  ],\n  \"camera_channel_group\": ";
    if(session.field)actors<<"{\"base\": "<<session.field->camera_channels().channel_base()
        <<", \"count\": "<<session.field->camera_channels().channel_count()<<"}";
    else actors<<"null";
    actors<<",\n  \"next_front_instance\": "<<session.next_instance
          <<",\n  \"first_back_instance\": "<<session.instance_limit
          <<",\n  \"instance_aliases\": [";
    for(unsigned i=0;i<session.instance_aliases.size();++i) {
        if(i)actors<<',';
        if(i==2)actors<<"null";else actors<<session.instance_aliases[i];
    }
    actors<<"]\n}\n";
    if(!actors)throw std::runtime_error("Cannot write native actor-state capture");
    if(session.renderer){
        std::ofstream shadows(shadowsPath);
        shadows<<"{\n  \"version\": 1,\n  \"frame\": "<<session.frame<<",\n  \"gpu_order\": [";
        bool first=true;
        for(const auto& s:session.renderer->shadow_submissions()){
            shadows<<(first?"\n":",\n")<<"    {\"alias\": "<<s.alias<<", \"selector\": "<<s.selector
                <<", \"depth_bucket\": "<<s.bucket<<", \"blocks\": "<<s.blocks
                <<", \"triangles\": "<<s.triangles<<", \"emitted_prisms\": "<<s.emitted<<"}";
            first=false;
        }
        shadows<<"\n  ]\n}\n";
        if(!shadows)throw std::runtime_error("Cannot write shadow-submission capture");
    }
    for(const auto& [suffix,data]:std::array{
        std::pair{".screen-filter.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.filter_trace.data()),session.filter_trace.size()*4)},
        std::pair{".transition.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.transition_trace.data()),session.transition_trace.size()*4)},
        std::pair{".battle-transition.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.battle_transition_trace.data()),session.battle_transition_trace.size()*4)},
        std::pair{".battle-capture.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.battle_capture_trace.data()),session.battle_capture_trace.size()*4)},
        std::pair{".game-mode.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.game_mode_trace.data()),session.game_mode_trace.size()*4)},
        std::pair{".save-state.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.save_trace.data()),session.save_trace.size()*4)},
        std::pair{".restore-state.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.restore_trace.data()),session.restore_trace.size()*4)},
        std::pair{".name-entry-state.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.name_entry_trace.data()),session.name_entry_trace.size()*4)},
        std::pair{".chapter-state.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.chapter_trace.data()),session.chapter_trace.size()*4)},
        std::pair{".startup.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.startup_trace.data()),session.startup_trace.size()*4)},
        std::pair{".ordinary-load.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.ordinary_load_trace.data()),session.ordinary_load_trace.size()*4)},
        std::pair{".title-state.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.title_trace.data()),session.title_trace.size()*4)},
        std::pair{".results.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.results_trace.data()),session.results_trace.size()*4)},
        std::pair{".battle-loading.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.battle_loading_trace.data()),session.battle_loading_trace.size()*4)},
        std::pair{".retiring-effects.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(session.retiring_effect_trace.data()),session.retiring_effect_trace.size()*4)},
        std::pair{".variables.bin",okage::Bytes(session.variables)}}) {
        const auto output=fs::path(path.string()+suffix);
        if(fs::exists(output)||fs::is_symlink(output))throw std::runtime_error("Refusing existing field-state capture");
        std::ofstream stream(output,std::ios::binary);
        if(!stream.write(reinterpret_cast<const char*>(data.data()),data.size()))throw std::runtime_error("Cannot write field-state capture");
    }
    {
        const auto output=fs::path(path.string()+".ui-party.bin");
        if(fs::exists(output)||fs::is_symlink(output))throw std::runtime_error("Refusing existing UI party capture");
        std::ofstream stream(output,std::ios::binary);
        const auto write=[&](std::uint32_t value) {
            for(unsigned i=0;i<4;++i)stream.put(char((value>>(i*8))&255));
        };
        for(auto count:session.ui_party.counts())write(count);
        for(const auto& row:session.ui_party.rows())for(auto value:row)write(std::uint32_t(value));
        if(!stream)throw std::runtime_error("Cannot write UI party capture");
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
    if(!session.scene_trace.empty()) {
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
        if(session.field)std::cout<<"Field PC=0x"<<std::hex<<session.field->pc()<<" pending opcode=0x"<<session.field->pending_opcode<<std::dec
            <<" actor="<<session.field->pending_actor<<" animation="<<session.field->pending_animation<<'\n';
    }
}

@interface NativeTextView : NSView {
@public okage::LivePerformance* performance;
}
@property(strong) NSImage* image;
@property float opacity;
@property unsigned short held;
@property unsigned short pressed;
@property unsigned injectedKeyDowns;
@property BOOL pauseRequested;
@property BOOL fieldPresentation;
@end
@implementation NativeTextView
- (BOOL)acceptsFirstResponder {return YES;}
- (BOOL)isFlipped {return YES;}
- (void)keyDown:(NSEvent*)event {
    if(event.isARepeat)return;
    if(event.keyCode==35){self.pauseRequested=YES;return;} // P: diagnostic pause, no game input.
    const unsigned mask=(event.keyCode==36 || event.keyCode==49)?0x40:event.keyCode==126?0x1000:event.keyCode==125?0x4000:event.keyCode==123?0x8000:event.keyCode==124?0x2000:event.keyCode==1?0x800:event.keyCode==53?0x20:0;
    if(mask){self.pressed|=mask;self.held|=mask;++self.injectedKeyDowns;}
    else [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    if(event.keyCode==35)return;
    const unsigned mask=(event.keyCode==36 || event.keyCode==49)?0x40:event.keyCode==126?0x1000:event.keyCode==125?0x4000:event.keyCode==123?0x8000:event.keyCode==124?0x2000:event.keyCode==1?0x800:event.keyCode==53?0x20:0;
    if(mask)self.held&=~mask;
    else [super keyUp:event];
}
- (void)drawRect:(NSRect)dirty {
    if(performance&&performance->interval.active())performance->interval.draw(NSProcessInfo.processInfo.systemUptime);
    (void)dirty;[[NSColor colorWithCalibratedRed:.04 green:.06 blue:.09 alpha:1] setFill];NSRectFill(self.bounds);
    [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
    NSRect destination=self.bounds;
    if(self.fieldPresentation) {
        const CGFloat width=std::min(destination.size.width,destination.size.height*4/3);
        const CGFloat height=width*3/4;
        destination=NSMakeRect((self.bounds.size.width-width)/2,(self.bounds.size.height-height)/2,width,height);
    }
    [self.image drawInRect:destination fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
        fraction:std::clamp(self.opacity/128.f,0.f,1.f) respectFlipped:YES hints:nil];
}
@end
static void requireFreshViewCapture(const fs::path& path) {
    if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing native view capture");
}
static void captureView(NSView* view,const fs::path& path) {
    requireFreshViewCapture(path);
    [view display];
    NSBitmapImageRep* rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if(!rep)throw std::runtime_error("Cannot allocate native view capture");
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    NSData* png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSError* error=nil;
    if(!png||![png writeToFile:[NSString stringWithUTF8String:path.c_str()]
            options:NSDataWritingWithoutOverwriting error:&error])
        throw std::runtime_error("Cannot save native view");
}
@interface NativeApp : NSObject<NSApplicationDelegate,NSWindowDelegate> {
@public std::unique_ptr<Session> session;
@public unsigned testFrames;
@public int failureStatus;
@public bool paused;
@public std::vector<unsigned> testPresses;
@public DirectionReplay directions;
@public std::uint16_t replayDirections;
@public fs::path testCapture,pausedViewCapture;
@public fs::path pauseCaptureDirectory;
@public unsigned pauseCaptureCount;
@public std::unique_ptr<okage::LivePerformance> performance;
@public okage::LiveTickClock tickClock;
// One admitted scheduler invocation may span multiple host callbacks.
@public bool admittedTick,admittedTickMeasured;
@public double admittedTickStarted;
}
@property(strong) NSWindow* window;
@property(strong) NativeTextView* view;
@property(strong) NSTimer* timer;
@property double previousTime;
@end
@implementation NativeApp
- (okage::InputJournalContext)journalContext {
    return {session->current_scene,session->field?std::optional<unsigned>(session->field->pc()):std::nullopt};
}
- (void)capturePausedSession {
    if(pauseCaptureDirectory.empty())return;
    const auto path=pauseCaptureDirectory/("pause-"+std::to_string(++pauseCaptureCount)+
        "-frame-"+std::to_string(session->frame)+".png");
    capture(*session,path);
    captureView(self.view,path.string()+".view.png");
    std::cout<<"Paused native diagnostic capture frame="<<session->frame<<" path="<<path<<std::endl;
}
- (void)finishInputJournal {
    if(session->input_journal&&!session->input_journal->terminal())
        session->input_journal->finish(session->frame,[self journalContext]);
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,session->game_mode ? 720 : 336)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    self.window.delegate=self;
    if(session->music&&!paused)session->audio_output=std::make_unique<okage::AudioOutput>();
    self.view=[[NativeTextView alloc] initWithFrame:self.window.contentView.bounds];self.view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    self.view->performance=performance.get();
    self.view.held=replayDirections;
    self.view.fieldPresentation=bool(session->game_mode);
    self.window.contentView=self.view;[self.window center];[self.window makeKeyAndOrderFront:nil];[self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];self.previousTime=NSProcessInfo.processInfo.systemUptime;
    if(paused) {
        try {
            [self refreshImage];
            if(session->input_journal)session->input_journal->pause(session->frame,[self journalContext]);
            if(!pausedViewCapture.empty()) {
                captureView(self.view,pausedViewCapture);
                std::cout<<"Paused AppKit view capture at native frame="<<session->frame
                    <<" path="<<pausedViewCapture<<std::endl;
            }
            [self capturePausedSession];
        }catch(const std::exception& e) {
            if(session->input_journal&&!session->input_journal->terminal()) {
                try {session->input_journal->error(session->input_journal->frame(),e.what(),[self journalContext]);}
                catch(const std::exception& journalError){std::cerr<<journalError.what()<<'\n';}
            }
            failureStatus=1;std::cerr<<e.what()<<'\n';[NSApp stop:nil];return;
        }
    }
    else if(performance)performance->interval.begin(self.previousTime,session->frame);
    self.timer=[NSTimer scheduledTimerWithTimeInterval:1./120 target:self selector:@selector(update:) userInfo:nil repeats:YES];
}
- (void)refreshImage {
    const bool measured=performance&&performance->interval.active();
    const double started=measured?NSProcessInfo.processInfo.systemUptime:0;
    const auto [displayed,displayedFrame]=session->presentation();
    NSImage* image=[[NSImage alloc] initWithSize:NSMakeSize(displayed.width,displayed.height)];[image addRepresentation:bitmap(displayed)];
    self.view.image=image;self.view.opacity=session->game_mode ? 128 : session->opacity.value;self.view.needsDisplay=YES;
    self.window.title=[NSString stringWithFormat:@"Okage native — frame %u — %@ — P: pause/resume — Arrows, Return/Space, S: Start",
        displayedFrame,paused ? @"paused" : @"running"];
    if(session->field && session->field->pending_opcode)
        self.window.title=[self.window.title stringByAppendingString:@" — service pending (partial scene)"];
    if(measured)performance->interval.refresh(started,NSProcessInfo.processInfo.systemUptime);
}
- (void)completeAdmittedTickAt:(double)finished {
    if(!admittedTick)return;
    if(session->tick_pending())throw std::runtime_error("Completing an unfinished native tick");
    if(admittedTickMeasured&&performance&&performance->interval.active())
        performance->interval.deferred_tick(admittedTickStarted,finished,
            session->rendered_surface&&session->rendered_frame==session->frame);
    tickClock.complete_tick();
    if(performance&&performance->interval.active())
        performance->interval.scheduler_pending(tickClock.pending());
    admittedTick=false;admittedTickMeasured=false;
}
- (void)update:(NSTimer*)timer {
    (void)timer;
    bool measured=false;
    try {
        const double now=NSProcessInfo.processInfo.systemUptime;
        const double elapsed=now-self.previousTime;
        const double admitted=tickClock.admit(elapsed);self.previousTime=now;
        if(performance&&performance->interval.active())
            performance->interval.scheduler_admit(admitted,tickClock.pending());
        if(performance&&performance->interval.active()&&!(self.view.pauseRequested&&!paused)) {
            performance->interval.update_begin(now,std::max(0.,elapsed-.25));measured=true;
        }
        if(self.view.pauseRequested) {
            self.view.pauseRequested=NO;paused=!paused;
            self.view.held=self.view.pressed=0;
            if(performance) {
                if(paused) {
                    performance->interval.discard_elapsed(std::max(0.,elapsed-.25));
                }
                else {
                    performance->interval.begin(now,session->frame);
                    performance->interval.update_begin(now,0);measured=true;
                }
            }
            if(paused) {
                session->finish_battle_frame();
                [self completeAdmittedTickAt:now];
                session->close_loader_window();
                if(performance&&performance->interval.active()&&session->audio_output)performance->interval.audio_sample(session->audio_output->stats());
                session->audio_output.reset();
            }
            // Finish/debit an admitted tick before pause discards remaining debt.
            tickClock.reset();
            if(!paused&&session->music&&!session->audio_output)
                session->audio_output=std::make_unique<okage::AudioOutput>();
            if(paused&&performance)performance->finish(now,session->frame,okage::PerformanceEnd::pause);
            if(session->input_journal) {
                if(paused)session->input_journal->pause(session->frame,[self journalContext]);
                else session->input_journal->resume(session->frame,[self journalContext]);
            }
            [self refreshImage];
            if(paused)[self capturePausedSession];
        }
        if(paused){tickClock.reset();self.view.held=self.view.pressed=0;return;}
        bool changed=false;
        if(session->tick_pending()) {
            if(!admittedTick)throw std::runtime_error("Native tick has no AppKit admission");
            if(session->finish_prepared_tick(false)) {
                [self completeAdmittedTickAt:NSProcessInfo.processInfo.systemUptime];
                changed=true;
            } else {
                if(measured) {
                    performance->interval.scheduler_pending(tickClock.pending());
                    if(session->audio_output)performance->interval.audio_sample(session->audio_output->stats());
                    performance->interval.update_end(NSProcessInfo.processInfo.systemUptime);measured=false;
                }
                return;
            }
        }
        // No scheduler, sound generation, or test-key reinjection while waiting.
        if(!changed&&tickClock.tick_due()) {
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
            if(admittedTick)throw std::runtime_error("Native tick admitted twice");
            admittedTick=true;admittedTickMeasured=measured;
            admittedTickStarted=measured?NSProcessInfo.processInfo.systemUptime:0;
            const auto input=okage::DialogueButtons{self.view.held,self.view.pressed};
            self.view.pressed=0; // Consume only this scheduler's latch.
            const bool complete=session->tick(input,false);
            if(press) {
                NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:now
                    windowNumber:self.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
                [self.window sendEvent:event];
            }
            if(complete) {
                [self completeAdmittedTickAt:NSProcessInfo.processInfo.systemUptime];
                changed=true;
            }
        }
        if(measured) {
            performance->interval.scheduler_pending(tickClock.pending());
            if(session->audio_output)performance->interval.audio_sample(session->audio_output->stats());
        }
        if(!changed) {
            if(measured){performance->interval.update_end(NSProcessInfo.processInfo.systemUptime);measured=false;}
            return;
        }
        if(testFrames&&session->frame==testFrames)session->finish_battle_frame();
        [self refreshImage];
        if(measured){performance->interval.update_end(NSProcessInfo.processInfo.systemUptime);measured=false;}
        if(testFrames && session->frame==testFrames) {
            [self.timer invalidate];
            if(performance) {
                if(session->audio_output)performance->interval.audio_sample(session->audio_output->stats());
                performance->finish(NSProcessInfo.processInfo.systemUptime,session->frame,okage::PerformanceEnd::test_complete);
            }
            capture(*session,testCapture);
            captureView(self.view,testCapture.string()+".view.png");
            [self finishInputJournal];
            std::cout<<"AppKit keyDown events="<<self.view.injectedKeyDowns<<" native progress="<<session->text.state.progress<<" wait_release="<<session->release_frame<<std::endl;
            [NSApp terminate:nil];
        }
        else session->open_loader_window();
    }catch(const std::exception& e) {
        failureStatus=1;
        // Failed scheduler work must not later be counted as a completed tick.
        admittedTick=false;admittedTickMeasured=false;
        try {session->close_loader_window();}
        catch(const std::exception& loadingError){std::cerr<<loadingError.what()<<'\n';}
        if(session->input_journal&&!session->input_journal->terminal()) {
            try {session->input_journal->error(session->frame,e.what(),[self journalContext]);}
            catch(const std::exception& journalError){std::cerr<<journalError.what()<<'\n';}
        }
        const double failedAt=performance?NSProcessInfo.processInfo.systemUptime:0;
        if(performance&&performance->interval.active()) {
            performance->interval.scheduler_pending(tickClock.pending());
            if(session->audio_output)performance->interval.audio_sample(session->audio_output->stats());
        }
        session->audio_output.reset();
        if(performance) {
            failureStatus=1;
            try {
                if(measured)performance->interval.update_end(failedAt);
                performance->finish(failedAt,session->frame,okage::PerformanceEnd::runtime_error);
            } catch(const std::exception& reportError) {std::cerr<<reportError.what()<<'\n';}
        }
        [self.timer invalidate];self.window.title=[NSString stringWithUTF8String:e.what()];std::cerr<<e.what()<<'\n';
        if(testFrames){failureStatus=1;[NSApp stop:nil];}
    }
}
- (void)finishPerformance:(okage::PerformanceEnd)reason {
    if(!performance)return;
    try {
        if(performance->interval.active()&&session->audio_output)
            performance->interval.audio_sample(session->audio_output->stats());
        performance->finish(NSProcessInfo.processInfo.systemUptime,session->frame,reason);
    }
    catch(const std::exception& e) {failureStatus=1;std::cerr<<e.what()<<'\n';}
}
- (void)finishSession {
    std::string failure;
    try {
        session->finish_battle_frame();
        [self completeAdmittedTickAt:NSProcessInfo.processInfo.systemUptime];
    }
    catch(const std::exception& e){failure=e.what();}
    // A failed GPU ticket must not skip parking the background worker.
    try {session->close_loader_window();}
    catch(const std::exception& e){if(failure.empty())failure=e.what();else std::cerr<<e.what()<<'\n';}
    try {
        if(failure.empty())[self finishInputJournal];
        else if(session->input_journal&&!session->input_journal->terminal())
            session->input_journal->error(session->frame,failure,[self journalContext]);
    }catch(const std::exception& e){if(failure.empty())failure=e.what();else std::cerr<<e.what()<<'\n';}
    if(!failure.empty()){failureStatus=1;std::cerr<<failure<<'\n';}
}
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;[self finishSession];
    [self finishPerformance:okage::PerformanceEnd::window_close];
}
- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;[self finishSession];
    [self finishPerformance:okage::PerformanceEnd::application_terminate];
}
- (void)windowDidResignKey:(NSNotification*)notification {(void)notification;self.view.held=0;self.view.pressed=0;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {(void)app;return YES;}
@end
int main(int argc,const char** argv) {
    @autoreleasepool {
    std::unique_ptr<okage::InputJournal> inputJournal;
    try {
        if(argc<3)throw std::runtime_error("Usage: okage-native --game-data DIRECTORY [--save-directory DIRECTORY] OR okage-native FONT_DIRECTORY FIELD.PME [--frames N | --ui-test-frames M | --preview-at N [--ui-test-frames M>N]] [--press FRAME,FRAME --directions INPUT --buttons INPUT (held non-directional masks; bounded replay only) --capture FILE.png --capture-frames N,N,... --render-from FRAME --render-until FRAME --render-range FIRST:LAST (repeatable, inclusive, replay only)] [--scene MAP.xmp --event EVENT.xmt --actor ACTOR.xmd --actor-bind ALIAS=MODEL.xmd --player-name NAME] [--music-dry SEQUENCE.xsq --music-bank BANK.xv --sound-dir SOUND_DIRECTORY] [--effect-data TES.EFD --effect-textures ENV_TEX_DIRECTORY] [--battle-input OWNED_BATTLE_DIRECTORY] [--battle-live FRESH.jsonl (bounded replay with owned battle data)] [--load-observation FRESH.jsonl (bounded replay; host scheduling diagnostics)] [--battle-readback synchronous|deferred (replay policy; live always deferred)] [--replay-loading continuous|cooperative (cooperative requires deferred readback)] [--replay-display-from N (bounded cooperative replay; real host display callbacks and full rendering from N)] [--performance-report FRESH.jsonl (interactive/preview/UI test)] [--input-journal FRESH.jsonl] [--input-log FRESH.jsonl (path/sha256 of every asset read)] [--title-retirement-report FRESH_DIRECTORY (Retire title checkpoints)] [--pause-captures FRESH_DIRECTORY (interactive diagnostics, not saved games)] [--headless-control FRESH_DIRECTORY (stdin controller steps after --frames; no window)]");
        const bool coldStart=std::string_view(argv[1])=="--game-data";
        fs::path gameDataRoot,saveDirectory,startupTextureReport,titleResourceReport,titleRetirementReport;
        fs::path fontDirectory=coldStart?fs::path{}:fs::path(argv[1]),messagePath=coldStart?fs::path{}:fs::path(argv[2]);
        unsigned frames=0,uiFrames=0,previewAt=0,renderFrom=0,renderUntil=0;DirectionReplay directions,buttons,titleScript;bool hasButtons=false;std::vector<unsigned> presses,captureFrames;fs::path output,map,event,musicSequence,musicBank,soundDirectory,effectData,effectTextures,battleInput;std::vector<std::pair<int,fs::path>> actors;std::string playerName="Ari";
        std::optional<fs::path> performancePath,inputJournalPath,pauseCapturePath,headlessControlPath;
        fs::path uiUpdateReport,battleLivePath,loadObservationPath;
        bool deferredBattleReadback=false;
        std::optional<bool> cooperativeReplay;
        std::optional<unsigned> replayDisplayFrom;
        std::ofstream battleLiveTrace,loadObservationTrace;
        std::vector<okage::NativeRenderRange> renderRanges;
        for(int i=coldStart?1:3;i<argc;++i) {
            const std::string arg=argv[i];if(++i==argc)throw std::runtime_error("Missing native runtime option value");
            if(arg=="--game-data") {
                if(!coldStart||!gameDataRoot.empty())throw std::runtime_error("Game data requires one standalone cold-start root");
                gameDataRoot=argv[i];
            }
            else if(arg=="--save-directory")saveDirectory=argv[i];
            else if(arg=="--startup-texture-report")startupTextureReport=argv[i];
            else if(arg=="--title-resource-report")titleResourceReport=argv[i];
            else if(arg=="--title-retirement-report")titleRetirementReport=argv[i];
            else if(arg=="--title-pad-script") {
                if(!titleScript.entries.empty())throw std::runtime_error("Duplicate title pad script");
                titleScript.load(argv[i],0xffffu);
                std::vector<std::pair<unsigned,std::uint16_t>> pulses;
                for(const auto& [n,mask]:titleScript.entries){pulses.push_back({n,mask});pulses.push_back({n+1,0});}
                titleScript.entries.clear();
                for(const auto& e:pulses){
                    if(!titleScript.entries.empty()&&titleScript.entries.back().first>=e.first)
                        throw std::runtime_error("Title pad script presses must be at least two updates apart");
                    titleScript.entries.push_back(e);
                }
            }
            else if(arg=="--input-log") {
                if(okage::input_log_enabled()||fs::exists(argv[i]))throw std::runtime_error("Input log requires one fresh file");
                okage::enable_input_log(argv[i]);
            }
            else if(arg=="--frames")frames=std::stoul(argv[i]);
            else if(arg=="--headless-control") {
                if(headlessControlPath||!argv[i][0])throw std::runtime_error("Headless control requires one fresh directory");
                headlessControlPath=argv[i];
            }
            else if(arg=="--preview-at")previewAt=std::stoul(argv[i]);
            else if(arg=="--battle-readback") {
                const std::string policy=argv[i];
                if(policy!="synchronous"&&policy!="deferred")throw std::runtime_error("Battle readback must be synchronous or deferred");
                deferredBattleReadback=policy=="deferred";
            }
            else if(arg=="--replay-loading") {
                if(cooperativeReplay)throw std::runtime_error("Duplicate replay loading policy");
                const std::string policy=argv[i];
                if(policy!="continuous"&&policy!="cooperative")throw std::runtime_error("Replay loading must be continuous or cooperative");
                cooperativeReplay=policy=="cooperative";
            }
            else if(arg=="--replay-display-from") {
                unsigned value=0;const std::string_view text=argv[i];
                const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
                if(replayDisplayFrom||error!=std::errc{}||end!=text.data()+text.size()||!value)
                    throw std::runtime_error("Replay display start requires one positive frame");
                replayDisplayFrom=value;
            }
            else if(arg=="--render-from")renderFrom=std::stoul(argv[i]);
            else if(arg=="--render-until")renderUntil=std::stoul(argv[i]);
            else if(arg=="--render-range")renderRanges.push_back(okage::native_render_range(argv[i]));
            else if(arg=="--ui-test-frames")uiFrames=std::stoul(argv[i]);
            else if(arg=="--directions")directions.load(argv[i]);
            else if(arg=="--capture")output=argv[i];
            else if(arg=="--performance-report") {
                if(performancePath||!argv[i][0])throw std::runtime_error("Performance report requires one nonempty path");
                performancePath=argv[i];
            }
            else if(arg=="--input-journal") {
                if(inputJournalPath||!argv[i][0])throw std::runtime_error("Input journal requires one nonempty fresh path");
                inputJournalPath=argv[i];
            }
            else if(arg=="--pause-captures") {
                if(pauseCapturePath||!argv[i][0])throw std::runtime_error("Pause captures require one nonempty fresh directory");
                pauseCapturePath=argv[i];
            }
            else if(arg=="--capture-frames") {
                if(!captureFrames.empty())throw std::runtime_error("Duplicate native frame selection");
                std::string_view input=argv[i];
                for(;;){
                    const auto comma=input.find(',');const auto token=input.substr(0,comma);unsigned n=0;
                    const auto [end,error]=std::from_chars(token.data(),token.data()+token.size(),n);
                    if(error!=std::errc{}||end!=token.data()+token.size()||!n||captureFrames.size()>=64||(!captureFrames.empty()&&n<=captureFrames.back()))
                        throw std::runtime_error("Native capture frames require 1..64 increasing positive ticks");
                    captureFrames.push_back(n);if(comma==input.npos)break;input.remove_prefix(comma+1);
                }
            }
            else if(arg=="--scene")map=argv[i];
            else if(arg=="--event")event=argv[i];
            else if(arg=="--music-dry")musicSequence=argv[i];
            else if(arg=="--music-bank")musicBank=argv[i];
            else if(arg=="--sound-dir")soundDirectory=argv[i];
            else if(arg=="--effect-data")effectData=argv[i];
            else if(arg=="--effect-textures")effectTextures=argv[i];
            else if(arg=="--battle-input")battleInput=argv[i];
            else if(arg=="--battle-live") {
                if(!battleLivePath.empty()||!argv[i][0])throw std::runtime_error("Battle-live requires one nonempty fresh path");
                battleLivePath=argv[i];
            }
            else if(arg=="--load-observation") {
                if(!loadObservationPath.empty()||!argv[i][0])throw std::runtime_error("Loader observation requires one nonempty fresh path");
                loadObservationPath=argv[i];
            }
            else if(arg=="--ui-update-diagnostic") {
                if(!uiUpdateReport.empty()||!argv[i][0])throw std::runtime_error("UI-update diagnostic requires one nonempty fresh path");
                uiUpdateReport=argv[i];
            }
            else if(arg=="--actor")actors.push_back({-8,argv[i]});
            else if(arg=="--actor-bind") {
                const std::string value=argv[i];const auto eq=value.find('=');std::size_t consumed=0;
                if(eq==value.npos || eq+1==value.size())throw std::runtime_error("Actor binding requires ALIAS=MODEL.xmd");
                const auto alias=std::stoi(value.substr(0,eq),&consumed);
                if(consumed!=eq)throw std::runtime_error("Invalid actor alias");
                actors.push_back({alias,value.substr(eq+1)});
            }
            else if(arg=="--player-name")playerName=argv[i];
            else if(arg=="--buttons") {
                if(hasButtons)throw std::runtime_error("Duplicate native button replay");
                buttons.load(argv[i],0x0fffu);hasButtons=true;
            }
            else if(arg=="--press") {std::istringstream in(argv[i]);std::string n;while(std::getline(in,n,','))presses.push_back(std::stoul(n));}
            else throw std::runtime_error("Unknown native runtime option");
        }
        if(coldStart) {
            if(gameDataRoot.empty()||!map.empty()||!event.empty()||!actors.empty()||!musicSequence.empty()||
               !musicBank.empty()||!effectData.empty()||!effectTextures.empty()||!battleInput.empty()||!soundDirectory.empty()||playerName!="Ari")
                throw std::runtime_error("Cold startup uses the owned game-data root without diagnostic scene/audio/name overrides");
            fontDirectory=gameDataRoot/"CMNDATA/FONT";messagePath=gameDataRoot/"FLDDATA/MES/FIELD.PME";
            battleInput=gameDataRoot/"NATIVE_UI";
            if(saveDirectory.empty()) {
                const auto support=NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES);
                if(!support.count)throw std::runtime_error("Native Application Support directory unavailable");
                saveDirectory=fs::path(support[0].UTF8String)/"Okage Shadow King/Saves";
            }
        }
        if(cooperativeReplay&&(!frames&&!previewAt))
            throw std::runtime_error("Replay loading policy requires --frames or --preview-at");
        if(cooperativeReplay.value_or(false)&&!deferredBattleReadback)
            throw std::runtime_error("Cooperative replay requires --battle-readback deferred");
        if(headlessControlPath&&(!frames||previewAt||uiFrames||cooperativeReplay.value_or(false)||replayDisplayFrom))
            throw std::runtime_error("Headless control requires --frames with continuous replay loading and no UI/display mode");
        if(replayDisplayFrom&&(!frames||previewAt||*replayDisplayFrom>frames||!cooperativeReplay.value_or(false)))
            throw std::runtime_error("Replay display start requires a frame within a bounded cooperative replay");
        if(hasButtons&&((!frames&&!previewAt)||uiFrames))
            throw std::runtime_error("Button replay requires --frames or --preview-at without --ui-test-frames");
        if(musicSequence.empty()!=musicBank.empty())throw std::runtime_error("Music requires both sequence and bank");
        if(!soundDirectory.empty() && musicSequence.empty())throw std::runtime_error("Sound directory requires native music setup");
        if(effectData.empty()!=effectTextures.empty()||(!effectData.empty()&&map.empty()))throw std::runtime_error("Effects require data, texture directory and scene");
        if(map.empty()!=event.empty() || (!actors.empty() && map.empty()))throw std::runtime_error("Scene requires both map and event assets");
        // Check aliases before either trace reserves its output file.
        if(inputJournalPath) {
            for(const auto& path:{uiUpdateReport,battleLivePath,loadObservationPath,performancePath.value_or(fs::path{})})
                if(!path.empty()&&fs::weakly_canonical(path)==fs::weakly_canonical(*inputJournalPath))
                    throw std::runtime_error("Input journal overlaps another report");
        }
        if(!loadObservationPath.empty()) {
            if(!frames||previewAt||uiFrames||battleInput.empty())
                throw std::runtime_error("Loader observation requires bounded --frames and owned --battle-input");
            if(fs::exists(loadObservationPath)||fs::is_symlink(loadObservationPath)||
                (!output.empty()&&okage::native_capture_family_contains(output,loadObservationPath))||
                (!uiUpdateReport.empty()&&fs::weakly_canonical(uiUpdateReport)==fs::weakly_canonical(loadObservationPath))||
                (!battleLivePath.empty()&&fs::weakly_canonical(battleLivePath)==fs::weakly_canonical(loadObservationPath))||
                (performancePath&&fs::weakly_canonical(*performancePath)==fs::weakly_canonical(loadObservationPath)))
                throw std::runtime_error("Loader observation refuses an existing or aliased output");
            loadObservationTrace.open(loadObservationPath);
            loadObservationTrace<<"{\"schema\":\"okage-native-loader-observation-v1\",\"capacity_per_battle\":8192,\"capacity_per_field_return\":65536,\"scope\":\"opt-in native host events; may perturb scheduling; worker events have no guessed simulation tick; no original timing equivalence\"}\n";
            if(!loadObservationTrace)throw std::runtime_error("Cannot create native loader observation");
        }
        if(!battleLivePath.empty()) {
            if(!frames||battleInput.empty())throw std::runtime_error("Native battle-live requires bounded --frames and owned --battle-input");
            if(fs::exists(battleLivePath)||fs::is_symlink(battleLivePath)||
                (!output.empty()&&okage::native_capture_family_contains(output,battleLivePath))||
                (!uiUpdateReport.empty()&&fs::weakly_canonical(uiUpdateReport)==fs::weakly_canonical(battleLivePath)))
                throw std::runtime_error("Battle-live refuses an existing or aliased output");
            battleLiveTrace.open(battleLivePath);
            battleLiveTrace<<"{\"schema\":\"okage-native-battle-live-v1\",\"frame_convention\":\"native tick; callback entry/return; no shifted anchors\",\"scope\":\"root and roster callbacks after native construction; raw pointer slots normalized by typed owners; channels are stored outputs\"}\n";
            if(!battleLiveTrace)throw std::runtime_error("Cannot create native battle-live trace");
        }
        const auto runPlan=okage::native_run_plan(frames,previewAt,uiFrames,output,diagnosticFrameLimit);
        const auto replayFrames=runPlan.replay_frames;
        okage::validate_native_capture_selection(runPlan,captureFrames);
        okage::validate_native_render_ranges(renderRanges,replayFrames);
        if(renderFrom&&(!replayFrames||renderFrom>replayFrames))throw std::runtime_error("Render start requires a replay frame within the capture");
        if(renderUntil&&(!renderFrom||renderUntil<renderFrom||renderUntil>replayFrames))
            throw std::runtime_error("Render end requires a bounded headless render interval");
        if(performancePath&&frames)throw std::runtime_error("Performance report requires interactive, preview-at or UI-test mode");
        if(pauseCapturePath&&frames)throw std::runtime_error("Pause captures require an interactive or preview session");
        if(runPlan.bounded_handoff) {
            okage::require_fresh_native_capture_family(output);
            if(performancePath&&okage::native_capture_family_contains(output,*performancePath))
                throw std::runtime_error("Performance report overlaps the combined capture output family");
        }
        const auto pausedViewCapture=runPlan.starts_paused&&!output.empty()
            ?fs::path(output.string()+".view.png"):fs::path{};
        if(!pausedViewCapture.empty()) {
            requireFreshViewCapture(pausedViewCapture);
            if(performancePath&&fs::weakly_canonical(fs::absolute(*performancePath))==
                    fs::weakly_canonical(fs::absolute(pausedViewCapture)))
                throw std::runtime_error("Performance report overlaps the paused view capture");
        }
        // Reserve the fresh report before loading assets or preparing a preview.
        if(inputJournalPath) {
            for(const auto& path:{output,runPlan.replay_capture,runPlan.live_capture})
                if(!path.empty()&&okage::native_capture_family_contains(path,*inputJournalPath))
                    throw std::runtime_error("Input journal overlaps native capture output");
        }
        if(pauseCapturePath) {
            const auto directory=fs::weakly_canonical(fs::absolute(*pauseCapturePath));
            for(const auto& path:{output,runPlan.replay_capture,runPlan.live_capture})
                if(!path.empty()&&okage::native_capture_family_contains(path,directory))
                    throw std::runtime_error("Pause capture directory overlaps a native capture family");
            for(const auto& path:{output,uiUpdateReport,battleLivePath,loadObservationPath,
                                performancePath.value_or(fs::path{}),inputJournalPath.value_or(fs::path{})}) {
                if(path.empty())continue;
                const auto relative=fs::weakly_canonical(fs::absolute(path)).lexically_relative(directory);
                if(!relative.empty()&&*relative.begin()!="..")
                    throw std::runtime_error("Pause capture directory overlaps another output");
            }
            if(fs::exists(directory)||fs::is_symlink(directory)||!fs::create_directory(directory))
                throw std::runtime_error("Pause captures require a fresh directory");
            pauseCapturePath=directory;
        }
        if(headlessControlPath) {
            const auto directory=fs::weakly_canonical(fs::absolute(*headlessControlPath));
            for(const auto& path:{output,uiUpdateReport,battleLivePath,loadObservationPath,
                                inputJournalPath.value_or(fs::path{})}) {
                if(path.empty())continue;
                const auto absolute=fs::weakly_canonical(fs::absolute(path));
                const auto relative=absolute.lexically_relative(directory);
                if((!relative.empty()&&*relative.begin()!="..")||okage::native_capture_family_contains(path,directory))
                    throw std::runtime_error("Headless control directory overlaps another output");
            }
            if(fs::exists(*headlessControlPath)||fs::is_symlink(*headlessControlPath)||!fs::create_directory(directory))
                throw std::runtime_error("Headless control requires a fresh directory");
            headlessControlPath=directory;
            if(!inputJournalPath)inputJournalPath=directory/"inputs.jsonl";
        }
        if(inputJournalPath)inputJournal=std::make_unique<okage::InputJournal>(*inputJournalPath);
        std::unique_ptr<okage::LivePerformance> performance;
        if(performancePath)performance=std::make_unique<okage::LivePerformance>(*performancePath,
            uiFrames?std::optional<okage::PerformanceTestRange>({replayFrames,uiFrames}):std::nullopt);
        okage::SceneRenderer::prepare_backend();
        const auto backendDevice=MTLCreateSystemDefaultDevice();
        okage::BattleFilterMetal::prepare_backend(backendDevice);
        okage::ShadowMetal::prepare_backend(backendDevice);
        okage::UiMetal::prepare_backend(backendDevice);
        std::unique_ptr<Session> prepared;
        if(replayFrames) {
            const auto frameDirectory=fs::path(output.string()+".frames");std::ofstream frameManifest;
            if(!captureFrames.empty()) {
                if(fs::exists(frameDirectory)||fs::is_symlink(frameDirectory)||!fs::create_directory(frameDirectory))
                    throw std::runtime_error("Native selected-frame output must be a fresh directory");
                frameManifest.open(frameDirectory/"manifest.json");
                frameManifest<<"{\n  \"boundary\": \"after completed native Session::tick, before the next input\",\n  \"captures\": [\n";
                if(!frameManifest)throw std::runtime_error("Cannot open native frame manifest");
            }
            prepared=std::make_unique<Session>(fontDirectory,messagePath,playerName,!coldStart);
            auto& session=*prepared;session.render_from=renderFrom;session.render_until=renderUntil;session.render_ranges=std::move(renderRanges);
            session.input_journal=inputJournal.get();
            session.deferred_battle_readback=deferredBattleReadback;
            if(cooperativeReplay.value_or(false))session.loading_policy=Session::LoadingPolicy::ReplayWindows;
            session.battle_input=battleInput;
            session.battle_ui_update_report=uiUpdateReport;
            session.battle_live_trace=battleLivePath.empty()?nullptr:&battleLiveTrace;
            session.load_observation_trace=loadObservationPath.empty()?nullptr:&loadObservationTrace;
            if(!output.empty())session.battle_construction_report=output.string()+".battle-construction.json";
            if(!output.empty())session.battle_stage_report=output.string()+".battle-stage.json";
            session.live_presentation_enabled=previewAt!=0||cooperativeReplay.value_or(false)||bool(headlessControlPath);
            session.save_directory=saveDirectory;
            session.startup_texture_report=startupTextureReport;session.resource_report=titleResourceReport;
            session.retirement_report=titleRetirementReport;
            if(!titleRetirementReport.empty()&&fs::exists(titleRetirementReport))throw std::runtime_error("Title retirement report directory must be fresh");
            if(!titleResourceReport.empty()&&fs::exists(titleResourceReport))throw std::runtime_error("Title resource report directory must be fresh");
            if(coldStart)session.initialize_cold_start(gameDataRoot,saveDirectory,!output.empty());
            if(!effectData.empty())session.prepare_effects(effectData,effectTextures);
            if(!map.empty())session.load_scene(map,event,actors);
            if(!musicSequence.empty())session.load_music(musicSequence,musicBank,!output.empty(),soundDirectory);
            if(!effectData.empty())session.load_effects(effectData,effectTextures);
            std::uint16_t previous=0;std::size_t selected=0;
            try {for(unsigned i=1;i<=replayFrames;++i) {
                if(replayDisplayFrom&&i==*replayDisplayFrom)
                    session.display_link=std::make_unique<okage::DisplayLink>();
                const std::uint16_t mask=std::find(presses.begin(),presses.end(),i)!=presses.end()?0x40:0;
                auto held=std::uint16_t(directions.held(i)|buttons.held(i));
                if(const auto n=session.next_first_title_update())held|=titleScript.held(*n);
                const bool completed=session.tick({std::uint16_t(mask|held),std::uint16_t(mask|(held&~previous))},
                    !cooperativeReplay.value_or(false));
                previous=held;
                if(!completed) {
                    if(session.rendered_surface&&session.rendered_frame<=session.frame&&
                        session.frame-session.rendered_frame<=2) {
                        const auto shown=session.presentation();
                        ++session.replay_presentations;
                        if(shown.second<session.frame)++session.replay_delayed_presentations;
                    }
                    session.finish_prepared_tick(true);
                }
                if(selected<captureFrames.size()&&i==captureFrames[selected]) {
                    const auto filename="tick-"+std::to_string(i)+".png";captureImage(session,frameDirectory/filename);
                    frameManifest<<(selected ? ",\n" : "")<<"    {\"tick\": "<<i<<", \"image\": \""<<filename
                        <<"\", \"scene\": "<<session.current_scene<<", \"event_pc\": "<<(session.field?session.field->pc():0);
                    if(session.record_audio&&session.music)
                        frameManifest<<", \"audio_sample_frame_offset\": "<<session.audio_capture.size()/2;
                    frameManifest<<"}";
                    ++selected;
                }
                if(cooperativeReplay.value_or(false)) {
                    if(session.battle&&session.battle->resource_live()) {
                        const auto shown=session.presentation();
                        ++session.replay_presentations;
                        if(shown.second<session.frame)++session.replay_delayed_presentations;
                    }
                    if(i<replayFrames)session.open_loader_window();
                }
            }
            session.finish_battle_frame();
            session.close_loader_window();
            session.write_load_observation("replay_end");
            } catch(...) {
                const auto failure=std::current_exception();
                // Closing remains mandatory after a failed drain or capture.
                try {session.close_loader_window();}
                catch(const std::exception& error){std::cerr<<error.what()<<'\n';}
                try {session.write_load_observation("replay_failure");}
                catch(const std::exception& error){std::cerr<<error.what()<<'\n';}
                std::rethrow_exception(failure);
            }
            if(!captureFrames.empty()) {
                frameManifest<<"\n  ],\n  \"complete\": true\n}\n";frameManifest.close();
                if(!frameManifest||selected!=captureFrames.size())throw std::runtime_error("Incomplete native frame capture");
            }
            if(!runPlan.replay_capture.empty())capture(session,runPlan.replay_capture);
            std::cout<<"Native frames="<<session.frame<<" progress="<<session.text.state.progress<<" wait_release="<<session.release_frame<<std::endl;
            if(headlessControlPath) {
                session.render_from=1;session.render_until=0;session.render_ranges.clear();
                std::ofstream commands(*headlessControlPath/"commands.txt");
                if(!commands)throw std::runtime_error("Cannot record headless commands");
                commands<<"# Native digital controller steps; continuous asynchronous loaders can finish\n"
                        <<"# while simulation waits for input. No AppKit timing or deterministic readiness claim.\n";
                auto park=[&] {
                    std::exception_ptr failure;
                    try {session.finish_battle_frame();}catch(...){failure=std::current_exception();}
                    try {session.close_loader_window();}catch(...){if(!failure)failure=std::current_exception();}
                    if(failure)std::rethrow_exception(failure);
                };
                auto status=[&] {
                    std::cout<<"Headless ready frame="<<session.frame<<" scene="<<session.current_scene
                             <<" pc="<<(session.field?session.field->pc():0)<<" message="<<session.message_id
                             <<std::endl;
                };
                std::set<std::string> observation_names;
                try {
                    park();inputJournal->pause(session.frame);status();
                    std::string line;
                    while(std::getline(std::cin,line)) {
                        commands<<session.frame<<' '<<line<<'\n';commands.flush();
                        if(!commands)throw std::runtime_error("Cannot record headless command");
                        const auto command=okage::parse_headless_command(line,diagnosticFrameLimit-session.frame);
                        if(command.kind==okage::HeadlessCommand::Kind::Quit)break;
                        if(command.kind==okage::HeadlessCommand::Kind::Capture) {
                            const auto path=*headlessControlPath/(command.name+".png");
                            okage::require_fresh_native_capture_family(path);
                            reserve_observation_name(observation_names,command.name);capture(session,path);
                        } else if(command.kind==okage::HeadlessCommand::Kind::Status) {
                            std::cout<<session.headless_status_json()<<std::endl;
                            continue;
                        } else if(command.kind==okage::HeadlessCommand::Kind::View) {
                            const auto path=*headlessControlPath/(command.name+".view.png");
                            reserve_observation_name(observation_names,command.name);
                            okage::require_fresh_native_capture_family(path);writeView(session,path);
                        } else {
                            inputJournal->resume(session.frame);
                            for(unsigned n=0;n<command.ticks;++n) {
                                const auto pressed=std::uint16_t(command.held&~session.previous_held);
                                if(!session.tick({command.held,pressed},true))session.finish_prepared_tick(true);
                            }
                            park();inputJournal->pause(session.frame);
                        }
                        status();
                    }
                    if(std::cin.bad())throw std::runtime_error("Headless command input failed");
                    park();const auto final=*headlessControlPath/"final.png";
                    okage::require_fresh_native_capture_family(final);capture(session,final);
                    session.write_load_observation("control_end");
                } catch(...) {
                    const auto failure=std::current_exception();
                    try {park();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
                    try {session.write_load_observation("control_failure");}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
                    std::rethrow_exception(failure);
                }
            }
            if(frames) {
                if(!loadObservationPath.empty()) {
                    loadObservationTrace<<"{\"event\":\"finish\",\"frame\":"<<session.frame<<"}\n";loadObservationTrace.flush();
                    if(!loadObservationTrace)throw std::runtime_error("Native loader observation finish write failed");
                }
                if(!battleLivePath.empty()) {
                    battleLiveTrace<<"{\"event\":\"finish\",\"frame\":"<<session.frame<<"}\n";battleLiveTrace.flush();
                    if(!battleLiveTrace)throw std::runtime_error("Native battle-live finish write failed");
                }
                if(inputJournal)inputJournal->finish(session.frame,
                    {session.current_scene,session.field?std::optional<unsigned>(session.field->pc()):std::nullopt});
                return 0;
            }
            session.render_frame();
        }
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NativeApp* app=[NativeApp new];app->paused=runPlan.starts_paused;app->session=std::move(prepared);
        app->performance=std::move(performance);app->pausedViewCapture=pausedViewCapture;
        app->pauseCaptureDirectory=pauseCapturePath.value_or(fs::path{});
        if(!app->session) {
        app->session=std::make_unique<Session>(fontDirectory,messagePath,playerName,!coldStart);
        app->session->input_journal=inputJournal.get();
        app->session->battle_input=battleInput;
        app->session->battle_ui_update_report=uiUpdateReport;
        if(!output.empty())app->session->battle_construction_report=output.string()+".battle-construction.json";
        if(!output.empty())app->session->battle_stage_report=output.string()+".battle-stage.json";
        app->session->live_presentation_enabled=true;
        app->session->save_directory=saveDirectory;app->session->startup_texture_report=startupTextureReport;app->session->resource_report=titleResourceReport;app->session->retirement_report=titleRetirementReport;
        for(const auto& report:{titleResourceReport,titleRetirementReport})
            if(!report.empty()&&fs::exists(report))throw std::runtime_error("Title report directory must be fresh");
        if(coldStart)app->session->initialize_cold_start(gameDataRoot,saveDirectory,!output.empty());
        if(!effectData.empty())app->session->prepare_effects(effectData,effectTextures);
        if(!map.empty())app->session->load_scene(map,event,actors);
        if(!musicSequence.empty())app->session->load_music(musicSequence,musicBank,!output.empty(),soundDirectory);
        if(!effectData.empty())app->session->load_effects(effectData,effectTextures);
        }
        app->session->render_from=1;app->session->render_until=0;app->session->render_ranges.clear();
        app->session->loading_policy=Session::LoadingPolicy::LiveWindows;
        app->session->deferred_battle_readback=true;
        app->session->display_link=std::make_unique<okage::DisplayLink>();
        app->session->close_loader_window(); // Also closes the replay-to-live handoff.
        app->testFrames=uiFrames;app->testPresses=presses;app->directions=directions;app->testCapture=runPlan.live_capture;
        if(runPlan.bounded_handoff)app->replayDirections=directions.held(replayFrames);
        [NSApp setDelegate:app];[NSApp run];return app->failureStatus;
    }catch(const std::exception& e){
        if(inputJournal&&!inputJournal->terminal()) {
            try {inputJournal->error(inputJournal->frame(),e.what());}
            catch(const std::exception& journalError){std::cerr<<journalError.what()<<'\n';}
        }
        std::cerr<<e.what()<<'\n';return 1;
    }}
}
