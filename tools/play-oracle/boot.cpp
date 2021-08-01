#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "AppConfig.h"
#include "gs/GSH_Null.h"
#include "reference_graphics.hpp"
#include "reference_input.hpp"
#include "reference_audio.hpp"
#include "reference_scene.hpp"
#include "reference_events.hpp"
#include "reference_dialogue.hpp"
#include "reference_text.hpp"
#include "reference_controller.hpp"
#include "reference_movement.hpp"
#include "reference_interaction.hpp"
#include "reference_transition.hpp"
#include "reference_dust.hpp"
#include "reference_camera.hpp"
#include "reference_camera_schedule.hpp"
#include "reference_screen_filter.hpp"
#include "reference_actor.hpp"
#include "reference_npc.hpp"
#include "reference_lifecycle.hpp"
#include "reference_order.hpp"
#include "reference_music.hpp"
#include "iop/IopBios.h"
#include "iop/ioman/OpticalMediaDevice.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <CommonCrypto/CommonDigest.h>
#include <iomanip>
#include <sstream>

fs::path CAppConfig::GetBasePath() const {
    auto path=std::filesystem::current_path()/"local/play-boot-config";
    std::filesystem::create_directories(path); return path.string();
}
using Traffic=ResearchTraffic;
class ResearchVM final : public CPS2VM {
public:
    void wait_for_observer_command(){m_mailBox.WaitForCall(100);}
};
class TraceGS final : public CGSH_Null {
    Traffic& traffic;
public:
    explicit TraceGS(Traffic& value):traffic(value) {}
    void ProcessHostToLocalTransfer() override {++traffic.uploads; CGSH_Null::ProcessHostToLocalTransfer();}
protected:
    void WriteRegisterImpl(uint8 reg,uint64 value) override {
        ++traffic.registers;
        if (reg==GS_REG_XYZ2 || reg==GS_REG_XYZF2) ++traffic.vertices;
        CGSHandler::WriteRegisterImpl(reg,value);
    }
};
class TraceDisc final : public Iop::Ioman::COpticalMediaDevice {
    std::atomic<unsigned>& frames;
public:
    TraceDisc(OpticalMediaPtr& media,std::atomic<unsigned>& counter):COpticalMediaDevice(media),frames(counter) {}
    Framework::CStream* GetFile(uint32 flags,const char* path) override {
        auto* result=COpticalMediaDevice::GetFile(flags,path);
        std::cout<<"disc_open_vblank="<<frames<<" found="<<(result!=nullptr)<<" path="<<path<<std::endl;
        return result;
    }
};
int main(int argc,char** argv) {
    try {
        if (argc<3) throw std::runtime_error("Usage: okage-boot-oracle DISC.cue VBLANKS [STATE.zip [--load]] [--save STATE] [--capture PREFIX] [--pad INPUT | --walk-waypoints ROUTE --walk-record INPUT] [--audio WAV] [--trace-files] [--trace-loaders] [--scene DIRECTORY] [--events DIRECTORY] [--dialogue FILE] [--text FILE] [--controller FILE] [--camera FILE] [--camera-schedule FILE] [--screen-filter FILE] [--movement FILE] [--interaction FILE] [--interaction-wide FILE] [--transition FILE] [--dust FILE] [--actor FILE --actor-alias -1|-8|-9|0..2047] [--npc FILE] [--lifecycle FILE] [--order FILE] [--music FILE] [--deadline-seconds 1..7200]");
        unsigned deadlineSeconds=0;
        std::string walkWaypoints,walkRecord;
        int actorAlias=-8;bool load=false,traceFiles=false,traceLoaders=false,musicField=false,musicJingle=false,musicNight=false,interactionWide=false;std::string state,capture,pad,audio,saveState,scene,events,dialogue,text,controller,camera,actor,lifecycle,order,cameraSchedule,music,movement,interaction,transition,npc,dust,screenFilter;
        for (int i=3;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--load") load=true;
            else if (arg=="--trace-files") traceFiles=true;
            else if (arg=="--trace-loaders") traceLoaders=true;
            else if (arg=="--capture" && i+1<argc) capture=argv[++i];
            else if (arg=="--pad" && i+1<argc) pad=argv[++i];
            else if (arg=="--walk-waypoints" && i+1<argc) walkWaypoints=argv[++i];
            else if (arg=="--walk-record" && i+1<argc) walkRecord=argv[++i];
            else if (arg=="--audio" && i+1<argc) audio=argv[++i];
            else if (arg=="--save" && i+1<argc) saveState=argv[++i];
            else if (arg=="--scene" && i+1<argc) scene=argv[++i];
            else if (arg=="--events" && i+1<argc) events=argv[++i];
            else if (arg=="--dialogue" && i+1<argc) dialogue=argv[++i];
            else if (arg=="--text" && i+1<argc) text=argv[++i];
            else if (arg=="--npc" && i+1<argc) npc=argv[++i];
            else if (arg=="--actor" && i+1<argc) actor=argv[++i];
            else if (arg=="--actor-alias" && i+1<argc) actorAlias=std::stoi(argv[++i]);
            else if (arg=="--camera-schedule" && i+1<argc) cameraSchedule=argv[++i];
            else if (arg=="--lifecycle" && i+1<argc) lifecycle=argv[++i];
            else if (arg=="--order" && i+1<argc) order=argv[++i];
            else if (arg=="--music" && i+1<argc) music=argv[++i];
            else if (arg=="--music-field") musicField=true;
            else if (arg=="--music-jingle") musicJingle=true;
            else if (arg=="--music-night") musicNight=true;
            else if (arg=="--screen-filter" && i+1<argc) screenFilter=argv[++i];
            else if (arg=="--camera" && i+1<argc) camera=argv[++i];
            else if (arg=="--controller" && i+1<argc) controller=argv[++i];
            else if (arg=="--dust" && i+1<argc) dust=argv[++i];
            else if (arg=="--transition" && i+1<argc) transition=argv[++i];
            else if (arg=="--interaction-wide" && i+1<argc) {interaction=argv[++i];interactionWide=true;}
            else if (arg=="--interaction" && i+1<argc) interaction=argv[++i];
            else if (arg=="--movement" && i+1<argc) movement=argv[++i];
            else if (arg=="--deadline-seconds" && i+1<argc) {
                const std::string_view input=argv[++i];unsigned value=0;
                const auto [end,error]=std::from_chars(input.data(),input.data()+input.size(),value);
                if(error!=std::errc{} || end!=input.data()+input.size() || !value || value>7200)
                    throw std::runtime_error("Research deadline requires 1..7200 seconds");
                deadlineSeconds=value;
            }
            else if (state.empty() && !arg.starts_with("--")) state=arg;
            else throw std::runtime_error("Unknown boot argument");
        }
        if (load && state.empty()) throw std::runtime_error("--load requires a state path");
        if(!music.empty() && !load)throw std::runtime_error("Music tracing requires a state with RXSNDDRV already loaded");
        if(musicField&&music.empty())throw std::runtime_error("--music-field requires --music");
        if(musicJingle&&(music.empty()||musicField))throw std::runtime_error("--music-jingle requires --music and excludes --music-field");
        if(musicNight&&(music.empty()||musicField||musicJingle))throw std::runtime_error("--music-night requires --music and excludes other music corpora");
        if (!load && !state.empty()) {
            if (!saveState.empty()) throw std::runtime_error("Duplicate state output");
            saveState=state;
        }
#ifndef OKAGE_REFERENCE_VULKAN
        if (!capture.empty()) throw std::runtime_error("Configure OKAGE_REFERENCE_VULKAN for captures");
#endif
        const auto requested=std::stoul(argv[2]);
        if (!requested || requested>36000) throw std::runtime_error("Boot experiment requires 1..36000 vblanks");
        const auto disc=std::filesystem::absolute(argv[1]);
        if (!std::filesystem::is_regular_file(disc)) throw std::runtime_error("Disc input missing");
        if (!saveState.empty() && (std::filesystem::exists(saveState) || std::filesystem::is_symlink(saveState)))
            throw std::runtime_error("Refusing existing state output");
        if (!audio.empty() && (std::filesystem::exists(audio) || std::filesystem::is_symlink(audio)))
            throw std::runtime_error("Refusing existing WAV output");
        if (load && !std::filesystem::is_regular_file(state)) throw std::runtime_error("State input missing");
        auto recordedPad=std::make_unique<RecordedPad>(pad,requested);
        if(!walkWaypoints.empty()||!walkRecord.empty()) {
            if(walkWaypoints.empty()||walkRecord.empty()||!pad.empty()||!load||transition.empty()||camera.empty())
                throw std::runtime_error("Walking requires --load, --transition, --camera, --walk-waypoints and --walk-record, without --pad");
            recordedPad->navigate(walkWaypoints,walkRecord);
        }
        auto sceneTrace=scene.empty() ? nullptr : std::make_unique<SceneTrace>(scene);
        auto eventTrace=events.empty() ? nullptr : std::make_unique<EventTrace>(events);
        auto dialogueTrace=dialogue.empty() ? nullptr : std::make_unique<DialogueTrace>(dialogue);
        auto textTrace=text.empty() ? nullptr : std::make_unique<TextTrace>(text);
        auto controllerTrace=controller.empty() ? nullptr : std::make_unique<ControllerTrace>(controller);
        auto screenFilterTrace=screenFilter.empty() ? nullptr : std::make_unique<ScreenFilterTrace>(screenFilter);
        auto cameraTrace=camera.empty() ? nullptr : std::make_unique<CameraTrace>(camera);
        auto npcTrace=npc.empty() ? nullptr : std::make_unique<NpcTrace>(npc);
        auto actorTrace=actor.empty() ? nullptr : std::make_unique<ActorTrace>(actor,actorAlias);
        auto dustTrace=dust.empty() ? nullptr : std::make_unique<DustTrace>(dust);
        auto transitionTrace=transition.empty() ? nullptr : std::make_unique<TransitionTrace>(transition);
        auto interactionTrace=interaction.empty() ? nullptr : std::make_unique<InteractionTrace>(interaction,interactionWide);
        auto movementTrace=movement.empty() ? nullptr : std::make_unique<MovementTrace>(movement);
        auto cameraScheduleTrace=cameraSchedule.empty() ? nullptr : std::make_unique<CameraScheduleTrace>(cameraSchedule);
        auto lifecycleTrace=lifecycle.empty() ? nullptr : std::make_unique<LifecycleTrace>(lifecycle);
        auto orderTrace=order.empty() ? nullptr : std::make_unique<OrderTrace>(order);
        const bool tracing=traceLoaders || bool(cameraScheduleTrace) || bool(screenFilterTrace) || bool(dustTrace) || bool(npcTrace) || bool(sceneTrace) || bool(eventTrace) || bool(dialogueTrace) || bool(textTrace) || bool(controllerTrace) || bool(cameraTrace) || bool(actorTrace) || bool(movementTrace) || bool(interactionTrace) || bool(transitionTrace) || bool(lifecycleTrace) || bool(orderTrace) || !music.empty();
        RecordedAudio* recordedAudio=nullptr;
        Traffic traffic; std::atomic<unsigned> frames{0}; std::atomic<bool> exited{false};
        ResearchVM vm;
        CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH,disc.string());
        vm.Initialize();
        struct Cleanup { CPS2VM& vm; ~Cleanup(){vm.Pause();vm.DestroySoundHandler();vm.DestroyPadHandler();vm.DestroyGSHandler();vm.Destroy();} } cleanup{vm};
#ifdef OKAGE_REFERENCE_VULKAN
        if (!capture.empty()) vm.CreateGSHandler(vulkanReference(traffic));
        else
#endif
        vm.CreateGSHandler([&](){return new TraceGS(traffic);});
        vm.CreatePadHandler([&](){return recordedPad.release();});
        if (!audio.empty()) vm.CreateSoundHandler([&](){recordedAudio=new RecordedAudio();return recordedAudio;});
        if (!vm.m_cdrom0) throw std::runtime_error("Reference VM did not mount the disc");
        if (traceFiles) {
            auto* bios=dynamic_cast<CIopBios*>(vm.m_iop->m_bios.get());
            if (!bios) throw std::runtime_error("Reference IOP BIOS missing");
            auto traced=std::make_shared<TraceDisc>(vm.m_cdrom0,frames);
            for (const auto* name:{"cdrom","cdrom0","cdrom1"}) bios->GetIoman()->RegisterDevice(name,traced);
        }
        auto onFrame=vm.OnNewFrame.Connect([&](){
            const auto n=++frames;
            if (n==1 || n%60==0) {
                std::cout<<"vblank="<<n<<" ee_pc=0x"<<std::hex<<vm.m_ee->m_EE.m_State.nPC
                         <<" iop_pc=0x"<<vm.m_iop->m_cpu.m_State.nPC<<std::dec
                         <<" gs_registers="<<traffic.registers<<" gs_vertices="<<traffic.vertices
                         <<" uploads="<<traffic.uploads<<std::endl;
            }
            if (n==requested) vm.PauseAsync();
        });
        auto onExit=vm.m_ee->m_os->OnRequestExit.Connect([&](){exited=true;});
        vm.m_ee->m_os->BootFromCDROM();
        if (load) {
            if (!vm.LoadState(state).get()) throw std::runtime_error("Could not load reference state");
            std::cout<<"Loaded reference state "<<state<<std::endl;
        }
        auto musicTrace=music.empty() ? nullptr : std::make_unique<MusicTrace>(music,*vm.m_iop,musicNight?3:(musicJingle?2:unsigned(musicField)));
        if(musicTrace)for(auto at:MusicTrace::offsets)vm.m_iop->m_cpu.ToggleBreakpoint(musicTrace->base+at);
        std::vector<uint32> loaders;
        if(traceLoaders)loaders={0x1bccf0,0x1babf8,0x1c2e68};
        const std::array<uint32,5> sceneBreakpoints={0x1856f8,0x185fb0,0x184bd8,0x18579c,0x186058};
        if(sceneTrace)for(auto at:sceneBreakpoints)loaders.push_back(at);
        if(eventTrace) {loaders.push_back(0x1b5888);loaders.push_back(0x1b58ac);}
        if(dialogueTrace) {loaders.push_back(0x130458);loaders.push_back(0x1b59f0);}
        if(textTrace)for(auto at:TextTrace::breakpoints)loaders.push_back(at);
        if(controllerTrace)for(auto at:ControllerTrace::breakpoints)loaders.push_back(at);
        if(screenFilterTrace)for(auto at:ScreenFilterTrace::breakpoints)loaders.push_back(at);
        if(cameraTrace)for(auto at:CameraTrace::breakpoints)loaders.push_back(at);
        if(actorTrace)for(auto at:actorTrace->breakpoints())loaders.push_back(at);
        if(npcTrace)for(auto at:NpcTrace::breakpoints)loaders.push_back(at);
        if(dustTrace)for(auto at:DustTrace::breakpoints)loaders.push_back(at);
        if(transitionTrace)for(auto at:TransitionTrace::breakpoints)loaders.push_back(at);
        if(interactionTrace)for(auto at:InteractionTrace::breakpoints)loaders.push_back(at);
        if(movementTrace)for(auto at:MovementTrace::breakpoints)loaders.push_back(at);
        if(lifecycleTrace)for(auto at:LifecycleTrace::breakpoints)loaders.push_back(at);
        if(cameraScheduleTrace)for(auto at:CameraScheduleTrace::breakpoints)loaders.push_back(at);
        if(orderTrace)for(auto at:OrderTrace::breakpoints)loaders.push_back(at);
        std::sort(loaders.begin(),loaders.end());loaders.erase(std::unique(loaders.begin(),loaders.end()),loaders.end());
        if (tracing) {
            Iop::Ioman::COpticalMediaDevice reader(vm.m_cdrom0);
            std::unique_ptr<Framework::CStream> stream(reader.GetFile(1,"\\SCUS_971.29;1"));
            if (!stream || stream->GetLength()!=1374220) throw std::runtime_error("Loader tracing needs the pinned USA ELF");
            std::vector<uint8> bytes(1374220);
            if (stream->Read(bytes.data(),bytes.size())!=bytes.size()) throw std::runtime_error("Short ELF hash read");
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),bytes.size(),digest);
            std::ostringstream hash;for (auto byte:digest) hash<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
            if (hash.str()!="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")
                throw std::runtime_error("Loader tracing ELF hash mismatch");
            for (auto address:loaders) vm.m_ee->m_EE.ToggleBreakpoint(address);
        }
        std::atomic<bool> stateChanged{false};
        std::mutex stateMutex;
        std::condition_variable stateWake;
        auto onState=vm.OnMachineStateChange.Connect([&](){
            {std::lock_guard lock(stateMutex);stateChanged=true;}
            stateWake.notify_one();
            if(tracing && frames<requested && vm.GetStatus()==CVirtualMachine::PAUSED)
                vm.wait_for_observer_command();
        });
        std::cout<<"Booting "<<vm.m_ee->m_os->GetExecutableName()<<" with research HLE services and "<<(capture.empty() ? "null" : "offscreen Vulkan")<<" GS"<<std::endl;
        vm.Resume();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(deadlineSeconds ? deadlineSeconds : std::max<unsigned long>(90,tracing ? requested*2+30 : requested/30+30));
        unsigned tracedCalls=0;
        while (frames<requested && !exited && std::chrono::steady_clock::now()<deadline) {
            if (tracing && stateChanged.exchange(false) && vm.GetStatus()==CVirtualMachine::PAUSED && frames<requested) {
                if(musicTrace) {
                    const auto at=vm.m_iop->m_cpu.m_State.nPC-musicTrace->base;
                    if(std::find(MusicTrace::offsets.begin(),MusicTrace::offsets.end(),at)!=MusicTrace::offsets.end()) {
                        musicTrace->observe(*vm.m_iop,frames);vm.Resume();continue;
                    }
                }
                const auto& cpu=vm.m_ee->m_EE.m_State;
                const auto pc=cpu.nPC;
                if (std::find(loaders.begin(),loaders.end(),pc)==loaders.end()) {
                    std::ostringstream error;error<<"Unexpected reference breakpoint at EE PC 0x"<<std::hex<<pc
                        <<" IOP PC 0x"<<vm.m_iop->m_cpu.m_State.nPC;
                    throw std::runtime_error(error.str());
                }
                if (++tracedCalls>(eventTrace ? 200000u : 10000u)) throw std::runtime_error("Reference trace limit exceeded");
                const bool observingCameraSchedule=cameraScheduleTrace&&std::find(CameraScheduleTrace::breakpoints.begin(),CameraScheduleTrace::breakpoints.end(),pc)!=CameraScheduleTrace::breakpoints.end();
                if(observingCameraSchedule)cameraScheduleTrace->observe(*vm.m_ee,frames);
                const bool observingNpc=npcTrace&&std::find(NpcTrace::breakpoints.begin(),NpcTrace::breakpoints.end(),pc)!=NpcTrace::breakpoints.end();
                if(observingNpc)npcTrace->observe(*vm.m_ee,frames);
                const bool observingActor=actorTrace&&std::find(actorTrace->breakpoints().begin(),actorTrace->breakpoints().end(),pc)!=actorTrace->breakpoints().end();
                // Observe before another subscriber resumes a shared field BP.
                if(observingActor)actorTrace->observe(*vm.m_ee,frames);
                const bool observingDust=dustTrace&&std::find(DustTrace::breakpoints.begin(),DustTrace::breakpoints.end(),pc)!=DustTrace::breakpoints.end();
                if(observingDust)dustTrace->observe(*vm.m_ee,frames);
                if(transitionTrace && std::find(TransitionTrace::breakpoints.begin(),TransitionTrace::breakpoints.end(),pc)!=TransitionTrace::breakpoints.end()) {
                    transitionTrace->observe(*vm.m_ee,frames);
                    if(!(interactionTrace && std::find(InteractionTrace::breakpoints.begin(),InteractionTrace::breakpoints.end(),pc)!=InteractionTrace::breakpoints.end())) {vm.Resume();continue;}
                }
                if(interactionTrace && std::find(InteractionTrace::breakpoints.begin(),InteractionTrace::breakpoints.end(),pc)!=InteractionTrace::breakpoints.end()) {
                    interactionTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(movementTrace && std::find(MovementTrace::breakpoints.begin(),MovementTrace::breakpoints.end(),pc)!=MovementTrace::breakpoints.end()) {
                    movementTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(lifecycleTrace && std::find(LifecycleTrace::breakpoints.begin(),LifecycleTrace::breakpoints.end(),pc)!=LifecycleTrace::breakpoints.end()) {
                    lifecycleTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(orderTrace && std::find(OrderTrace::breakpoints.begin(),OrderTrace::breakpoints.end(),pc)!=OrderTrace::breakpoints.end()) {
                    orderTrace->observe(*vm.m_ee,frames);
                    const bool shared=(textTrace && std::find(TextTrace::breakpoints.begin(),TextTrace::breakpoints.end(),pc)!=TextTrace::breakpoints.end()) ||
                        (controllerTrace && std::find(ControllerTrace::breakpoints.begin(),ControllerTrace::breakpoints.end(),pc)!=ControllerTrace::breakpoints.end());
                    if(!shared){vm.Resume();continue;}
                }
                if(observingNpc){vm.Resume();continue;}
                // Actor and scene observers share a model-render boundary.
                if(observingActor) {
                    if(!sceneTrace||std::find(sceneBreakpoints.begin(),sceneBreakpoints.end(),pc)==sceneBreakpoints.end()){vm.Resume();continue;}
                }
                if(screenFilterTrace && std::find(ScreenFilterTrace::breakpoints.begin(),ScreenFilterTrace::breakpoints.end(),pc)!=ScreenFilterTrace::breakpoints.end()) {
                    screenFilterTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(cameraTrace && std::find(CameraTrace::breakpoints.begin(),CameraTrace::breakpoints.end(),pc)!=CameraTrace::breakpoints.end()) {
                    cameraTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(observingCameraSchedule){vm.Resume();continue;}
                if(controllerTrace && std::find(ControllerTrace::breakpoints.begin(),ControllerTrace::breakpoints.end(),pc)!=ControllerTrace::breakpoints.end()) {
                    controllerTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(textTrace && std::find(TextTrace::breakpoints.begin(),TextTrace::breakpoints.end(),pc)!=TextTrace::breakpoints.end()) {
                    textTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(dialogueTrace && (pc==0x130458 || pc==0x1b59f0)) {
                    dialogueTrace->observe(*vm.m_ee);vm.Resume();continue;
                }
                if(eventTrace && (pc==0x1b5888 || pc==0x1b58ac)) {
                    eventTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(sceneTrace && std::find(sceneBreakpoints.begin(),sceneBreakpoints.end(),pc)!=sceneBreakpoints.end()) {
                    sceneTrace->observe(*vm.m_ee,frames);vm.Resume();continue;
                }
                if(observingDust){vm.Resume();continue;}
                const uint32 address=uint32(cpu.nGPR[CMIPS::A0].nD0)&0x1fffffff;
                std::string path;
                for (unsigned i=0;i<256 && address+i<PS2::EE_RAM_SIZE;++i) {
                    const auto c=vm.m_ee->m_ram[address+i];if (!c) break;
                    if (c<32 || c>126) {path="<non-ASCII argument>";break;}path.push_back(char(c));
                }
                std::cout<<"loader_vblank="<<frames<<" pc=0x"<<std::hex<<pc
                         <<" ra=0x"<<uint32(cpu.nGPR[CMIPS::RA].nD0)
                         <<" argument=0x"<<address<<std::dec<<" text="<<path<<std::endl;
                vm.Resume();
            }
            if(tracing) {
                std::unique_lock lock(stateMutex);
                stateWake.wait_for(lock,std::chrono::milliseconds(50),[&](){
                    return stateChanged.load() || frames>=requested || exited;
                });
            } else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        vm.Pause();
        if (tracing) for (auto address:loaders) vm.m_ee->m_EE.ToggleBreakpoint(address);
        if(musicTrace)for(auto at:MusicTrace::offsets)vm.m_iop->m_cpu.ToggleBreakpoint(musicTrace->base+at);
        for (const auto& module:vm.m_iop->m_bios->GetModulesDebugInfo())
            std::cout<<"IOP module "<<module.name<<" at 0x"<<std::hex<<module.begin<<"..0x"<<module.end<<std::dec<<std::endl;
        std::cout<<"Stopped: vblanks="<<frames<<" exit_request="<<exited<<" gs_registers="<<traffic.registers
                 <<" gs_vertices="<<traffic.vertices<<" uploads="<<traffic.uploads<<std::endl;
        if(actorTrace)actorTrace->finish();
        if(npcTrace)npcTrace->finish();
        if(transitionTrace)transitionTrace->finish();
        if(dustTrace)dustTrace->finish();
        if(interactionTrace)interactionTrace->finish();
        if(movementTrace)movementTrace->finish();
        if(lifecycleTrace)lifecycleTrace->finish();
        if(cameraScheduleTrace)cameraScheduleTrace->finish();
        if(musicTrace)musicTrace->finish();
        if (frames<requested) throw std::runtime_error("Reference boot ended or timed out before requested vblanks");
        if (!saveState.empty() && !vm.SaveState(saveState).get()) throw std::runtime_error("Could not save reference state");
        if (recordedAudio) recordedAudio->finish(audio);
#ifdef OKAGE_REFERENCE_VULKAN
        if (!capture.empty()) captureReference(vm.GetGSHandler(),capture);
#endif
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
