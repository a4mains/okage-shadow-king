#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "AppConfig.h"
#include "gs/GSH_Null.h"
#include "reference_graphics.hpp"
#include "reference_input.hpp"
#include "reference_audio.hpp"
#include "iop/IopBios.h"
#include "iop/ioman/OpticalMediaDevice.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <CommonCrypto/CommonDigest.h>
#include <iomanip>
#include <sstream>

fs::path CAppConfig::GetBasePath() const {
    auto path=std::filesystem::current_path()/"local/play-boot-config";
    std::filesystem::create_directories(path); return path.string();
}
using Traffic=ResearchTraffic;
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
        if (argc<3) throw std::runtime_error("Usage: okage-boot-oracle DISC.cue VBLANKS [STATE.zip [--load]] [--save STATE] [--capture PREFIX] [--pad INPUT] [--audio WAV] [--trace-files] [--trace-loaders]");
        bool load=false,traceFiles=false,traceLoaders=false;std::string state,capture,pad,audio,saveState;
        for (int i=3;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--load") load=true;
            else if (arg=="--trace-files") traceFiles=true;
            else if (arg=="--trace-loaders") traceLoaders=true;
            else if (arg=="--capture" && i+1<argc) capture=argv[++i];
            else if (arg=="--pad" && i+1<argc) pad=argv[++i];
            else if (arg=="--audio" && i+1<argc) audio=argv[++i];
            else if (arg=="--save" && i+1<argc) saveState=argv[++i];
            else if (state.empty() && !arg.starts_with("--")) state=arg;
            else throw std::runtime_error("Unknown boot argument");
        }
        if (load && state.empty()) throw std::runtime_error("--load requires a state path");
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
        RecordedAudio* recordedAudio=nullptr;
        Traffic traffic; std::atomic<unsigned> frames{0}; std::atomic<bool> exited{false};
        CPS2VM vm;
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
        const std::array<uint32,3> loaders={0x1bccf0,0x1babf8,0x1c2e68};
        if (traceLoaders) {
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
        auto onState=vm.OnMachineStateChange.Connect([&](){stateChanged=true;});
        std::cout<<"Booting "<<vm.m_ee->m_os->GetExecutableName()<<" with research HLE services and "<<(capture.empty() ? "null" : "offscreen Vulkan")<<" GS"<<std::endl;
        vm.Resume();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(std::max<unsigned long>(90,requested/30+30));
        unsigned tracedCalls=0;
        while (frames<requested && !exited && std::chrono::steady_clock::now()<deadline) {
            if (traceLoaders && stateChanged.exchange(false) && vm.GetStatus()==CVirtualMachine::PAUSED && frames<requested) {
                const auto& cpu=vm.m_ee->m_EE.m_State;
                const auto pc=cpu.nPC;
                if (std::find(loaders.begin(),loaders.end(),pc)==loaders.end()) throw std::runtime_error("Unexpected reference breakpoint");
                if (++tracedCalls>10000) throw std::runtime_error("Loader trace limit exceeded");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(traceLoaders ? 1 : 50));
        }
        vm.Pause();
        if (traceLoaders) for (auto address:loaders) vm.m_ee->m_EE.ToggleBreakpoint(address);
        for (const auto& module:vm.m_iop->m_bios->GetModulesDebugInfo())
            std::cout<<"IOP module "<<module.name<<" at 0x"<<std::hex<<module.begin<<"..0x"<<module.end<<std::dec<<std::endl;
        std::cout<<"Stopped: vblanks="<<frames<<" exit_request="<<exited<<" gs_registers="<<traffic.registers
                 <<" gs_vertices="<<traffic.vertices<<" uploads="<<traffic.uploads<<std::endl;
        if (frames<requested) throw std::runtime_error("Reference boot ended or timed out before requested vblanks");
        if (!saveState.empty() && !vm.SaveState(saveState).get()) throw std::runtime_error("Could not save reference state");
        if (recordedAudio) recordedAudio->finish(audio);
#ifdef OKAGE_REFERENCE_VULKAN
        if (!capture.empty()) captureReference(vm.GetGSHandler(),capture);
#endif
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
