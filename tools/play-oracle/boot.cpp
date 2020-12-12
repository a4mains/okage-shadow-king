#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "AppConfig.h"
#include "gs/GSH_Null.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>

fs::path CAppConfig::GetBasePath() const {
    auto path=std::filesystem::current_path()/"local/play-boot-config";
    std::filesystem::create_directories(path); return path.string();
}
struct Traffic { std::atomic<uint64> registers{0},vertices{0},uploads{0}; };
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
int main(int argc,char** argv) {
    try {
        const bool load=argc==5 && std::string(argv[4])=="--load";
        if (argc!=3 && argc!=4 && !load)
            throw std::runtime_error("Usage: okage-boot-oracle DISC.cue VBLANKS [STATE.zip [--load]]");
        const auto requested=std::stoul(argv[2]);
        if (!requested || requested>3600) throw std::runtime_error("Boot experiment requires 1..3600 vblanks");
        const auto disc=std::filesystem::absolute(argv[1]);
        if (!std::filesystem::is_regular_file(disc)) throw std::runtime_error("Disc input missing");
        if (argc==4 && (std::filesystem::exists(argv[3]) || std::filesystem::is_symlink(argv[3])))
            throw std::runtime_error("Refusing existing state output");
        if (load && !std::filesystem::is_regular_file(argv[3])) throw std::runtime_error("State input missing");
        Traffic traffic; std::atomic<unsigned> frames{0}; std::atomic<bool> exited{false};
        CPS2VM vm;
        CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH,disc.string());
        vm.Initialize();
        struct Cleanup { CPS2VM& vm; ~Cleanup(){vm.Pause();vm.DestroyGSHandler();vm.Destroy();} } cleanup{vm};
        vm.CreateGSHandler([&](){return new TraceGS(traffic);});
        if (!vm.m_cdrom0) throw std::runtime_error("Reference VM did not mount the disc");
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
            if (!vm.LoadState(argv[3]).get()) throw std::runtime_error("Could not load reference state");
            std::cout<<"Loaded reference state "<<argv[3]<<std::endl;
        }
        std::cout<<"Booting "<<vm.m_ee->m_os->GetExecutableName()<<" with research HLE services and null GS"<<std::endl;
        vm.Resume();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        while (frames<requested && !exited && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        vm.Pause();
        for (const auto& module:vm.m_iop->m_bios->GetModulesDebugInfo())
            std::cout<<"IOP module "<<module.name<<" at 0x"<<std::hex<<module.begin<<"..0x"<<module.end<<std::dec<<std::endl;
        std::cout<<"Stopped: vblanks="<<frames<<" exit_request="<<exited<<" gs_registers="<<traffic.registers
                 <<" gs_vertices="<<traffic.vertices<<" uploads="<<traffic.uploads<<std::endl;
        if (frames<requested) throw std::runtime_error("Reference boot ended or timed out before requested vblanks");
        if (argc==4 && !vm.SaveState(argv[3]).get()) throw std::runtime_error("Could not save reference state");
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
