#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "AppConfig.h"
#include "gs/GSH_Null.h"
#include "iop/IopBios.h"
#include "iop/ioman/OpticalMediaDevice.h"
#include "reference_input.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

fs::path CAppConfig::GetBasePath() const {
    auto path=std::filesystem::current_path()/"local/play-boot-config";
    std::filesystem::create_directories(path); return path.string();
}

namespace {
constexpr const char* pinnedElf="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
constexpr uint32 stopPc=0x100618,entryPc=0x100008,mainPc=0x1002f8;

std::string hex(const unsigned char* d,size_t n) {
    std::ostringstream v;for(size_t i=0;i<n;++i)v<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(d[i]);return v.str();
}
std::string sha(const uint8* p,size_t n) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(p,static_cast<CC_LONG>(n),digest);return hex(digest,sizeof digest);
}
std::string hx(uint64 v){std::ostringstream o;o<<"\"0x"<<std::hex<<v<<'"';return o.str();}
std::string esc(const std::string& s){
    std::string o;for(unsigned char c:s){
        if(c=='"'||c=='\\'){o+='\\';o+=char(c);}else if(c<0x20||c>=0x7f){char b[8];std::snprintf(b,sizeof b,"\\u%04x",c);o+=b;}else o+=char(c);}
    return o;
}
void writeFresh(const std::filesystem::path& path,const void* data,size_t size) {
    if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing output "+path.string());
    std::ofstream out(path,std::ios::binary);
    if(!out.write(static_cast<const char*>(data),std::streamsize(size)))throw std::runtime_error("Cannot write "+path.string());
}

class LedgerGS final : public CGSH_Null {
public:
    struct Transfer {
        uint64 seq=0,bitbltbuf=0,trxpos=0,trxreg=0,trxdir=0;uint32 total=0,received=0;
        CC_SHA256_CTX ctx;bool open=false;
    };
    std::mutex mutex;
    std::vector<std::string> lines;
    uint64 regWrites=0,kicks=0,transfers=0,resets=0,localToHost=0,localToLocal=0,clutTransfers=0,hostToLocalDone=0;
    std::map<uint8,uint64> regHistogram;
    Transfer current;
    bool armed=false; // true once the initial fill was applied
    // Called on the GS thread only (or after a full sync).
    void flushTransfer() {
        if(!current.open)return;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&current.ctx);
        std::ostringstream o;
        o<<"{\"kind\":\"gs_transfer\",\"seq\":"<<current.seq<<",\"bitbltbuf\":"<<hx(current.bitbltbuf)
         <<",\"dbp\":"<<(current.bitbltbuf>>32&0x3fff)<<",\"dbw\":"<<(current.bitbltbuf>>48&0x3f)
         <<",\"dpsm\":"<<(current.bitbltbuf>>56&0x3f)<<",\"trxpos\":"<<hx(current.trxpos)
         <<",\"dsax\":"<<(current.trxpos>>32&0x7ff)<<",\"dsay\":"<<(current.trxpos>>48&0x7ff)
         <<",\"trxreg\":"<<hx(current.trxreg)<<",\"rrw\":"<<(current.trxreg&0xfff)<<",\"rrh\":"<<(current.trxreg>>32&0xfff)
         <<",\"trxdir\":"<<(current.trxdir&3)<<",\"expected_bytes\":"<<current.total<<",\"received_bytes\":"<<current.received
         <<",\"complete\":"<<(current.received==current.total?"true":"false")
         <<",\"image_sha256\":\""<<hex(digest,sizeof digest)<<"\"}";
        lines.push_back(o.str());current.open=false;
    }
protected:
    void WriteRegisterImpl(uint8 reg,uint64 value) override {
        {
            std::lock_guard lock(mutex);
            ++regWrites;++regHistogram[reg&0x7f];
            if(reg==GS_REG_XYZ2||reg==GS_REG_XYZF2) {
                ++kicks;
                if(kicks<=64) {
                    std::ostringstream o;
                    o<<"{\"kind\":\"gs_vertex_kick\",\"n\":"<<kicks<<",\"reg\":"<<unsigned(reg)<<",\"value\":"<<hx(value)
                     <<",\"prim\":"<<hx(m_nReg[GS_REG_PRIM])<<",\"frame1\":"<<hx(m_nReg[GS_REG_FRAME_1])
                     <<",\"zbuf1\":"<<hx(m_nReg[GS_REG_ZBUF_1])<<",\"test1\":"<<hx(m_nReg[GS_REG_TEST_1])<<"}";
                    lines.push_back(o.str());
                }
            }
        }
        CGSH_Null::WriteRegisterImpl(reg,value);
    }
    void BeginTransferWrite() override {
        std::lock_guard lock(mutex);
        flushTransfer();
        current=Transfer{};current.open=true;current.seq=++transfers;
        current.bitbltbuf=m_nReg[GS_REG_BITBLTBUF];current.trxpos=m_nReg[GS_REG_TRXPOS];
        current.trxreg=m_nReg[GS_REG_TRXREG];current.trxdir=m_nReg[GS_REG_TRXDIR];
        current.total=m_trxCtx.nSize;CC_SHA256_Init(&current.ctx);
        CGSH_Null::BeginTransferWrite();
    }
    void TransferWrite(const uint8* data,uint32 length) override {
        {
            std::lock_guard lock(mutex);
            if(current.open){
                const uint32 take=std::min<uint32>(length,m_trxCtx.nSize);
                CC_SHA256_Update(&current.ctx,data,take);current.received+=take;
            }
        }
        CGSH_Null::TransferWrite(data,length);
    }
    void ProcessHostToLocalTransfer() override {std::lock_guard lock(mutex);++hostToLocalDone;}
    void ProcessLocalToHostTransfer() override {std::lock_guard lock(mutex);++localToHost;lines.push_back("{\"kind\":\"gs_local_to_host\"}");}
    void ProcessLocalToLocalTransfer() override {std::lock_guard lock(mutex);++localToLocal;lines.push_back("{\"kind\":\"gs_local_to_local\"}");}
    void ProcessClutTransfer(uint32 a,uint32 b) override {
        std::lock_guard lock(mutex);++clutTransfers;
        lines.push_back("{\"kind\":\"gs_clut_transfer\",\"a\":"+std::to_string(a)+",\"b\":"+std::to_string(b)+"}");
    }
    void ResetImpl() override {
        {std::lock_guard lock(mutex);++resets;lines.push_back("{\"kind\":\"gs_reset\",\"armed\":"+std::string(armed?"true":"false")+"}");}
        CGSH_Null::ResetImpl();
    }
};

class ResearchVM final : public CPS2VM {
public:
    void wait_for_observer_command(){m_mailBox.WaitForCall(100);}
};

class TraceDisc final : public Iop::Ioman::COpticalMediaDevice {
public:
    std::mutex mutex;std::vector<std::string> rows;std::atomic<int>* topStep;
    TraceDisc(OpticalMediaPtr& media,std::atomic<int>* step):COpticalMediaDevice(media),topStep(step) {}
    Framework::CStream* GetFile(uint32 flags,const char* path) override {
        auto* result=COpticalMediaDevice::GetFile(flags,path);
        std::string hash="null";uint64 size=0;
        if(result) {
            std::unique_ptr<Framework::CStream> again(COpticalMediaDevice::GetFile(flags,path));
            if(again) {
                size=again->GetLength();
                if(size<=512ull*1024*1024) {
                    CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);std::vector<uint8> buf(1<<20);
                    for(uint64 left=size;left;) {
                        const auto n=std::min<uint64>(left,buf.size());
                        if(again->Read(buf.data(),n)!=n)break;
                        CC_SHA256_Update(&ctx,buf.data(),CC_LONG(n));left-=n;
                    }
                    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&ctx);hash='"'+hex(digest,sizeof digest)+'"';
                }
            }
        }
        std::lock_guard lock(mutex);
        std::ostringstream o;
        o<<"{\"kind\":\"disc_open\",\"n\":"<<rows.size()<<",\"flags\":"<<flags<<",\"path\":\""<<esc(path)<<"\",\"found\":"
         <<(result?"true":"false")<<",\"size\":"<<size<<",\"sha256\":"<<hash<<",\"top_step_pc\":"<<topStep->load()<<"}";
        rows.push_back(o.str());
        return result;
    }
};

struct HookDef{uint32 addr;const char* name;};
// Requested set first, then supporting functions needed to attribute them.
const HookDef hookDefs[]={
    {0x1cf008,"alloc"},{0x1cee50,"retain"},{0x1cf728,"free"},{0x1c9bb8,"upload"},
    {0x1cc880,"handle_create"},{0x1ccd48,"handle_alias"},{0x1cc4e0,"handle_name"},{0x1cca30,"handle_retire"},
    {0x1cd298,"palette_bind"},{0x1cd7d0,"clut_alloc_upload"},{0x1cd1e0,"image_alloc_handle_upload"},
    {0x1cb1e8,"color_depth_init"},{0x18f8c0,"capture_layer_init"},{0x190f38,"filter_clut_a"},
    {0x1908b0,"display_list_a_record"},
    {0x1cba48,"transfer"},{0x1cd598,"palette_bind_desc"},{0x18f7a8,"xim_load"},{0x1c2e68,"file_read"},
    {0x1c29d8,"file_read_raw"},{0x1bccf0,"archive_mount"},{0x1bcc80,"archive_unmount"},{0x1c0930,"dir_prefix"},
    {0x192ea0,"noise_texture"},{0x1974f8,"glyph_cache"},{0x1947a0,"font_load"},{0x1001a8,"fonts_init"},
    {0x1011d0,"effects_init"},{0x18fc68,"capture_filter_init"},{0x1943a8,"filter_mesh_noise"},
    {0x192e28,"filter_clut_b_setup"},{0x191030,"filter_clut_b"},{0x1914d0,"display_list_b_record"},
    {0x1cad20,"dma_kick"},{0x1ce940,"allocator_init"},{0x1cbdf8,"handle_system_init"},{0x10e098,"effect_noise_grid"},
    {0x1002f8,"main"},{0x103a88,"effects_load"},{0x103718,"effect_textures"},{0x1cce78,"handle_lookup"},
    {0x1cd5c8,"handle_reserve"},{0x186aa8,"xmd_load"},{0x1cebe8,"clear_reservation"},{0x1cdb80,"layer_build"},
    {0x1cdda0,"layer_attach_color"},{0x1cde18,"layer_attach_depth"},
    {0x1cf910,"occupancy_probe"},{0x10bc78,"effect_script_init"},
};
constexpr uint32 randPc=0x1f14b0;

struct Frame{uint32 fn;const char* name;uint32 ra;uint64 sp;uint64 enterIndex;bool dynamicBp;
             std::string pendingName;uint64 a[8];uint32 fileBuf=0;};

struct Alloc{int cls,psm,w,h;uint64 desc;};

int bitsPerPixel(uint32 psm){switch(psm){case 0:return 32;case 19:return 8;case 20:case 36:case 44:return 4;default:return 0;}}
} // namespace

int main(int argc,char** argv) {
    try {
        if(argc<5)throw std::runtime_error("Usage: okage-startup-oracle DISC.cue ELF OUTDIR FILL(00|ff) [--deadline-seconds N]");
        const auto disc=std::filesystem::absolute(argv[1]);
        const std::filesystem::path elfPath=argv[2],outDir=argv[3];
        const std::string fillText=argv[4];
        if(fillText!="00"&&fillText!="ff")throw std::runtime_error("FILL must be 00 or ff");
        const uint8 fill=fillText=="ff"?0xff:0x00;
        unsigned deadlineSeconds=1800;bool minimal=false;
        for(int i=5;i<argc;++i){
            if(std::string(argv[i])=="--deadline-seconds"&&i+1<argc)deadlineSeconds=unsigned(std::stoul(argv[++i]));
            else if(std::string(argv[i])=="--minimal")minimal=true;
            else throw std::runtime_error("Unknown argument");
        }
        if(!std::filesystem::is_regular_file(disc))throw std::runtime_error("Disc input missing");
        if(std::filesystem::exists(outDir)||std::filesystem::is_symlink(outDir))throw std::runtime_error("Output directory must be fresh");
        std::vector<uint8> elf;
        {std::ifstream in(elfPath,std::ios::binary);if(!in)throw std::runtime_error("Cannot read ELF");
         elf.assign(std::istreambuf_iterator<char>(in),{});}
        if(sha(elf.data(),elf.size())!=pinnedElf)throw std::runtime_error("ELF is not the pinned retail image");
        std::filesystem::create_directories(outDir);

        // ---- static breakpoint plan, derived from the pinned ELF text only -------------
        auto elfWord=[&](uint32 vaddr)->uint32{
            // .text: file offset 0x1000 -> vaddr 0x100000, size 0xf8434-0x100000
            const uint32 off=vaddr-0x100000+4096;uint32 w;std::memcpy(&w,elf.data()+off,4);return w;};
        std::map<uint32,std::string> entryName;
        for(const auto& h:hookDefs)if(h.name)entryName[h.addr]=h.name;
        std::set<uint32> returnSites;
        for(uint32 a=0x100000;a+8<0x1f8434;a+=4) {
            const auto w=elfWord(a);
            if((w>>26)==3&&entryName.count((w&0x03ffffff)<<2))returnSites.insert(a+8);
        }
        std::map<uint32,uint32> topSites; // pc -> callee
        for(uint32 a=mainPc;a<=0x100618;a+=4) {
            const auto w=elfWord(a);if((w>>26)==3)topSites[a]=(w&0x03ffffff)<<2;
        }
        if(!topSites.count(stopPc))throw std::runtime_error("Stop PC is not a jal in this ELF");

        // ---- VM ------------------------------------------------------------------------
        std::atomic<int> topStep{-1};
        LedgerGS* gs=nullptr;
        ResearchVM vm;
        CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH,disc.string());
        vm.Initialize();
        struct Cleanup { CPS2VM& vm; ~Cleanup(){vm.Pause();vm.DestroySoundHandler();vm.DestroyPadHandler();vm.DestroyGSHandler();vm.Destroy();} } cleanup{vm};
        vm.CreateGSHandler([&](){gs=new LedgerGS();return gs;});
        vm.CreatePadHandler([&](){return new RecordedPad(std::filesystem::path(),36000);});
        if(!vm.m_cdrom0)throw std::runtime_error("VM did not mount the disc");
        auto* bios=dynamic_cast<CIopBios*>(vm.m_iop->m_bios.get());
        if(!bios)throw std::runtime_error("IOP BIOS missing");
        auto traced=std::make_shared<TraceDisc>(vm.m_cdrom0,&topStep);
        for(const auto* name:{"cdrom","cdrom0","cdrom1"})bios->GetIoman()->RegisterDevice(name,traced);
        std::atomic<unsigned> frames{0};
        auto onFrame=vm.OnNewFrame.Connect([&](){++frames;});
        std::atomic<bool> exited{false};
        auto onExit=vm.m_ee->m_os->OnRequestExit.Connect([&](){exited=true;});
        vm.m_ee->m_os->BootFromCDROM();

        // ---- breakpoint bookkeeping ----------------------------------------------------
        std::map<uint32,int> bpRef;
        auto addBp=[&](uint32 a){if(bpRef[a]++==0)vm.m_ee->m_EE.ToggleBreakpoint(a);};
        auto delBp=[&](uint32 a){auto it=bpRef.find(a);if(it==bpRef.end()||it->second<=0)throw std::runtime_error("bp refcount");
            if(--it->second==0)vm.m_ee->m_EE.ToggleBreakpoint(a);};
        addBp(stopPc);
        if(!minimal) {
            addBp(randPc);
            for(const auto& kv:entryName)addBp(kv.first);
            for(auto a:returnSites)addBp(a);
            for(const auto& kv:topSites)addBp(kv.first);
        }

        std::atomic<bool> stateChanged{false},finishing{false};
        std::mutex stateMutex;std::condition_variable stateWake;
        auto onState=vm.OnMachineStateChange.Connect([&](){
            {std::lock_guard lock(stateMutex);stateChanged=true;}
            stateWake.notify_one();
            if(!finishing&&vm.GetStatus()==CVirtualMachine::PAUSED)vm.wait_for_observer_command();
        });

        // ---- observation state ---------------------------------------------------------
        uint8* const ram=vm.m_ee->m_ram;
        auto& cpu=vm.m_ee->m_EE.m_State;
        auto get32=[&](uint32 a){a&=0x1ffffff;uint32 v;std::memcpy(&v,ram+a,4);return v;};
        auto reg=[&](unsigned r){return uint64(cpu.nGPR[r].nD0);};
        auto cstr=[&](uint32 a){std::string s;a&=0x1ffffff;for(unsigned i=0;i<200&&a+i<PS2::EE_RAM_SIZE&&ram[a+i];++i)s.push_back(char(ram[a+i]));return s;};
        auto plausiblePtr=[&](uint64 p,uint64 n){return p>=0x100000&&p+n<=PS2::EE_RAM_SIZE&&p+n>=p;};
        auto norm=[](uint64 p){return uint64(uint32(p)&0x1fffffff);};
        std::ofstream events(outDir/"events.jsonl",std::ios::binary);
        if(!events)throw std::runtime_error("Cannot create events.jsonl");
        std::vector<Frame> stack;
        std::map<uint64,Alloc> allocs; // by allocation id
        std::vector<std::string> evLines;
        uint64 evCount=0;
        bool stopped=false;
        std::string fillReport="null";
        struct FileInfo{std::string name;uint32 ptr=0;uint64 size=0;std::string sha;};
        FileInfo lastFile;
        auto randState=[&]{
            const uint32 p=get32(0x2151b4);
            std::ostringstream o;o<<"\"rand_state_214ec8_58\":"<<get32(0x214ec8+0x58)<<",\"rand_ptr_2151b4\":"<<p;
            if(plausiblePtr(p,0x5c))o<<",\"rand_state_via_ptr\":"<<get32(p+0x58);
            return o.str();
        };
        auto descJson=[&](uint64 d){
            std::ostringstream o;const uint32 hi=uint32(d>>32);
            o<<"{\"raw\":"<<hx(d)<<",\"tbp\":"<<(d&0x3fff)<<",\"tbw\":"<<((d>>14)&0x3f)<<",\"id\":"<<((d>>20)&0xfff)
             <<",\"psm\":"<<(hi&0x3f)<<",\"w\":"<<((hi>>6)&0xfff)<<",\"h\":"<<((hi>>18)&0xfff)<<"}";
            return o.str();
        };
        auto emit=[&](const std::string& body){
            std::string line="{\"i\":"+std::to_string(evCount++)+",\"top\":"+std::to_string(topStep.load())+","+body+"}\n";
            events.write(line.data(),std::streamsize(line.size()));
        };
        auto argsJson=[&](const uint64* a){
            std::ostringstream o;o<<"\"args\":[";
            for(int i=0;i<8;++i)o<<(i?",":"")<<hx(a[i]);o<<"]";return o.str();
        };
        static const unsigned argRegs[8]={CMIPS::A0,CMIPS::A1,CMIPS::A2,CMIPS::A3,CMIPS::T0,CMIPS::T1,CMIPS::T2,CMIPS::T3};

        // Entry-side decoding of hooked functions (reads only).
        auto onEnter=[&](uint32 pc){
            Frame f{};f.fn=pc;f.name=entryName[pc].c_str();f.ra=uint32(reg(CMIPS::RA));f.sp=reg(CMIPS::SP);
            for(int i=0;i<8;++i)f.a[i]=reg(argRegs[i]);
            const std::string name=entryName[pc];
            std::ostringstream o;
            o<<"\"ev\":\"enter\",\"fn\":"<<hx(pc)<<",\"name\":\""<<name<<"\",\"depth\":"<<stack.size()
             <<",\"parent\":"<<(stack.empty()?std::string("null"):"\""+std::string(stack.back().name)+"\"")
             <<",\"ra\":"<<hx(f.ra)<<",\"sp\":"<<hx(f.sp)<<","<<argsJson(f.a);
            const auto& a=f.a;
            if(name=="alloc") o<<",\"class\":"<<int32(a[0])<<",\"psm\":"<<int32(a[1])<<",\"w\":"<<int32(a[2])<<",\"h\":"<<int32(a[3]);
            else if(name=="retain"||name=="free"||name=="handle_create") {
                o<<",\"desc\":"<<descJson(a[0]);
                if(name=="retain")o<<",\"delta\":"<<int32(a[1]);
            }
            else if(name=="upload") {
                const uint32 psm=uint32(a[0]>>32)&0x3f;const uint64 w=uint32(a[3]),h=uint32(a[4]),src=norm(a[5]);
                o<<",\"desc\":"<<descJson(a[0])<<",\"x\":"<<int32(a[1])<<",\"y\":"<<int32(a[2])<<",\"w\":"<<w<<",\"h\":"<<h<<",\"src\":"<<hx(src);
                const int bits=bitsPerPixel(psm);
                if(bits&&src) {
                    const uint64 qwc=w*h*bits/128,bytes=qwc*16;
                    o<<",\"qwc\":"<<qwc<<",\"src_bytes\":"<<bytes;
                    if(plausiblePtr(src,bytes))o<<",\"src_sha256\":\""<<sha(ram+src,bytes)<<"\"";
                    else o<<",\"src_sha256\":null";
                } else o<<",\"src_sha256\":null,\"src_note\":\""<<(src?"unsupported psm for hashing":"null source")<<"\"";
            }
            else if(name=="image_alloc_handle_upload") {
                const uint32 psm=uint32(a[0]),src=uint32(norm(a[3]));const uint64 w=uint32(a[1]),h=uint32(a[2]);
                o<<",\"psm\":"<<psm<<",\"w\":"<<w<<",\"h\":"<<h<<",\"src\":"<<hx(src);
                const int bits=bitsPerPixel(psm);
                if(bits&&src&&plausiblePtr(src,w*h*bits/8))o<<",\"src_nominal_bytes\":"<<w*h*bits/8<<",\"src_nominal_sha256\":\""<<sha(ram+src,w*h*bits/8)<<"\"";
                else o<<",\"src_nominal_sha256\":null,\"src_note\":\""<<(src?"unsupported psm or range":"null source (no upload)")<<"\"";
            }
            else if(name=="clut_alloc_upload") {
                const uint64 n=uint32(a[0]);const uint32 src=uint32(norm(a[1]));
                o<<",\"colors\":"<<n<<",\"src\":"<<hx(src);
                if(src&&n&&n<=4096&&plausiblePtr(src,n*4))o<<",\"src_bytes\":"<<n*4<<",\"src_sha256\":\""<<sha(ram+src,n*4)<<"\"";
                else o<<",\"src_sha256\":null,\"src_note\":\""<<(src?"range":"null source: allocated, no upload expected")<<"\"";
            }
            else if(name=="palette_bind") o<<",\"handle\":"<<int32(a[0])<<",\"level\":"<<int32(a[1])<<",\"desc\":"<<descJson(a[2]);
            else if(name=="palette_bind_desc") o<<",\"handle\":"<<int32(a[0])<<",\"desc\":"<<descJson(a[1]);
            else if(name=="handle_retire"||name=="handle_alias") o<<",\"handle_or_record\":"<<int32(a[0]);
            else if(name=="handle_name") {
                o<<",\"handle\":"<<int32(a[0]);o<<",\"name_str\":"<<(a[1]&&plausiblePtr(uint32(a[1]),1)?"\""+esc(cstr(uint32(a[1])))+"\"":std::string("null"));
            }
            else if(name=="handle_lookup"||name=="dir_prefix"||name=="archive_mount"||name=="file_read")
                o<<",\"str\":"<<(a[0]&&plausiblePtr(uint32(a[0]),1)?"\""+esc(cstr(uint32(a[0])))+"\"":std::string("null"));
            else if(name=="file_read_raw") {
                o<<",\"str\":"<<(a[0]&&plausiblePtr(uint32(a[0]),1)?"\""+esc(cstr(uint32(a[0])))+"\"":std::string("null"));
                f.fileBuf=uint32(a[1]);lastFile=FileInfo{};lastFile.name=cstr(uint32(a[0]));lastFile.ptr=uint32(a[1]);
            }
            else if(name=="color_depth_init") o<<",\"layers_w\":"<<int32(a[1])<<",\"layers_h\":"<<int32(a[2]);
            else if(name=="font_load") o<<",\"index\":"<<int32(a[0])<<",\"buf\":"<<hx(a[1])<<",\"flags\":"<<hx(a[2]);
            if(name=="main"||name=="noise_texture"||name=="effect_noise_grid"||name=="effects_load"||name=="fonts_init"||name=="capture_filter_init"||name=="glyph_cache"||name=="effect_script_init")
                o<<","<<randState();
            f.enterIndex=evCount;
            emit(o.str());
            if(!returnSites.count(f.ra)) {
                f.dynamicBp=true;addBp(f.ra);
                emit("\"ev\":\"note\",\"note\":\"dynamic return breakpoint (indirect call)\",\"fn\":"+hx(pc)+",\"ra\":"+hx(f.ra));
            }
            stack.push_back(f);
        };
        auto onExitAt=[&](size_t idx){
            Frame f=stack[idx];stack.erase(stack.begin()+long(idx));
            const std::string name=f.name;
            const uint64 v0=reg(CMIPS::V0),v1=reg(CMIPS::V1);
            std::ostringstream o;
            o<<"\"ev\":\"exit\",\"fn\":"<<hx(f.fn)<<",\"name\":\""<<name<<"\",\"enter\":"<<f.enterIndex<<",\"depth\":"<<stack.size()
             <<",\"v0\":"<<hx(v0)<<",\"v1\":"<<hx(v1);
            if(name=="alloc") {
                if(v0) {
                    o<<",\"result\":"<<descJson(v0);
                    allocs[(v0>>20)&0xfff]=Alloc{int32(f.a[0]),int32(f.a[1]),int32(f.a[2]),int32(f.a[3]),v0};
                } else o<<",\"result\":null";
            } else if(name=="clut_alloc_upload") {
                o<<",\"result\":"<<(v0?descJson(v0):std::string("null"));
            } else if(name=="handle_create"||name=="image_alloc_handle_upload"||name=="xim_load"||name=="handle_alias"||name=="handle_lookup")
                o<<",\"handle\":"<<int32(v0);
            else if(name=="file_read_raw") {
                lastFile.size=uint32(v0);
                const uint32 s2=uint32(reg(CMIPS::S2));
                o<<",\"bytes_read\":"<<int32(v0)<<",\"caller_s2\":"<<s2;
                if(int32(v0)>0&&plausiblePtr(lastFile.ptr,uint32(v0))){lastFile.sha=sha(ram+lastFile.ptr,uint32(v0));o<<",\"buf\":"<<hx(lastFile.ptr)<<",\"sha256\":\""<<lastFile.sha<<"\"";}
                else o<<",\"sha256\":null";
                o<<",\"file\":\""<<esc(lastFile.name)<<"\"";
            } else if(name=="file_read") {
                o<<",\"buf\":"<<hx(v0)<<",\"last_raw_read\":{\"name\":\""<<esc(lastFile.name)<<"\",\"size\":"<<lastFile.size<<",\"sha256\":"
                 <<(lastFile.sha.empty()?std::string("null"):"\""+lastFile.sha+"\"")<<"}";
            }
            if(name=="main"||name=="noise_texture"||name=="effect_noise_grid"||name=="effects_load"||name=="fonts_init"||name=="capture_filter_init"||name=="glyph_cache"||name=="effect_script_init")
                o<<","<<randState();
            emit(o.str());
            if(f.dynamicBp)delBp(f.ra);
        };

        auto fillGs=[&]{
            // Called before the first guest instruction: PC must be the retail entry point.
            if(uint32(cpu.nPC)!=entryPc)throw std::runtime_error("EE is not at the retail entry point before the first resume");
            gs->SendGSCall([]{},true);
            const uint8* vram=gs->GetRam();bool zero=true;for(uint32 i=0;i<CGSHandler::RAMSIZE;++i)if(vram[i]){zero=false;break;}
            uint64 regs,kicks,tr,res;
            {std::lock_guard lock(gs->mutex);regs=gs->regWrites;kicks=gs->kicks;tr=gs->transfers;res=gs->resets;}
            gs->SendGSCall([&]{std::memset(gs->GetRam(),fill,CGSHandler::RAMSIZE);gs->armed=true;},true);
            std::ostringstream o;
            o<<"{\"fill\":"<<unsigned(fill)<<",\"ee_pc\":"<<hx(cpu.nPC)<<",\"vram_all_zero_before_fill\":"<<(zero?"true":"false")
             <<",\"gs_reg_writes_before_fill\":"<<regs<<",\"gs_vertex_kicks_before_fill\":"<<kicks
             <<",\"gs_transfers_before_fill\":"<<tr<<",\"gs_resets_before_fill\":"<<res<<"}";
            fillReport=o.str();
        };
        auto isControlTransfer=[](uint32 w){
            const uint32 op=w>>26,fn=w&0x3f,rs=(w>>21)&0x1f;
            if(op==2||op==3||(op>=4&&op<=7)||op==1||(op>=0x14&&op<=0x17))return true;
            if(op==0&&(fn==8||fn==9))return true;
            if((op==0x10||op==0x11||op==0x12)&&rs==8)return true;
            return false;
        };
        auto firstControlTransfer=[&](uint32 pc)->uint32{
            if(pc<0x100000||pc>=0x1f8434)return 0;
            for(uint32 a=pc;a<0x1f8434&&a<pc+0x2000;a+=4)if(isControlTransfer(elfWord(a)))return a;
            return 0;
        };

        fillGs();
        const auto startTime=std::chrono::steady_clock::now();
        vm.Resume();
        const auto deadline=startTime+std::chrono::seconds(deadlineSeconds);
        uint64 stops=0,benignBlockStops=0,randCalls=0;std::vector<uint32> history;
        while(!stopped&&!exited&&std::chrono::steady_clock::now()<deadline) {
            if(stateChanged.exchange(false)&&vm.GetStatus()==CVirtualMachine::PAUSED) {
                const uint32 pc=cpu.nPC;++stops;if(history.size()<100000)history.push_back(pc);
                const uint64 sp=reg(CMIPS::SP);
                bool explained=false;
                // exits first (return addresses), then entries, then top-level call sites
                for(;;) {
                    bool any=false;
                    for(size_t i=stack.size();i-->0;)
                        if(stack[i].ra==pc&&stack[i].sp==sp){onExitAt(i);any=true;explained=true;break;}
                    if(!any)break;
                }
                if(pc==randPc){
                    emit("\"ev\":\"rand\",\"ra\":"+hx(reg(CMIPS::RA))+",\"state\":"+std::to_string(get32(0x214ec8+0x58)));
                    ++randCalls;explained=true;
                }
                if(entryName.count(pc)){onEnter(pc);explained=true;}
                const uint32 ct=firstControlTransfer(pc);
                auto top=ct?topSites.find(ct):topSites.end();
                if(top!=topSites.end()) {
                    topStep.store(int(ct));explained=true;
                    if(ct==stopPc) {
                        emit("\"ev\":\"stop\",\"block_start\":"+hx(pc)+",\"pc\":"+hx(ct)+",\"callee\":"+hx(top->second)+",\"stack_depth\":"+std::to_string(stack.size())+","+randState());
                        stopped=true;break; // stay paused: the jal at 0x100618 has NOT executed
                    }
                    emit("\"ev\":\"top\",\"block_start\":"+hx(pc)+",\"pc\":"+hx(ct)+",\"callee\":"+hx(top->second));
                }
                if(!explained) {
                    // A block start that contains some breakpoint address but no event we model.
                    bool contains=false;
                    if(ct)for(const auto& kv:bpRef)if(kv.second>0&&kv.first>=pc&&kv.first<=ct+4){contains=true;break;}
                    if(!contains) {
                        std::ostringstream d;d<<"Unexplained stop at EE PC "<<hx(pc)<<" ct="<<hx(ct)<<" ra="<<hx(reg(CMIPS::RA))
                            <<" stops="<<stops<<" stack="<<stack.size()<<" exc="<<cpu.nHasException;
                        throw std::runtime_error(d.str());
                    }
                    ++benignBlockStops;
                }
                vm.Resume();continue;
            }
            std::unique_lock lock(stateMutex);
            stateWake.wait_for(lock,std::chrono::milliseconds(50),[&]{return stateChanged.load()||exited.load();});
        }
        if(!stopped)throw std::runtime_error("Startup did not reach the stop PC (exit="+std::to_string(exited.load())+" stops="+std::to_string(stops)+")");
        if(vm.GetStatus()!=CVirtualMachine::PAUSED)throw std::runtime_error("VM not paused at stop");
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-startTime).count();
        if(uint32(cpu.nPC)!=stopPc)throw std::runtime_error("Stop PC mismatch");

        // ---- final state (VM paused, GS drained) -----------------------------------------
        // Finish: flush any buffered GS register writes, then wait for the GS thread.
        std::string dmaReport;
        {
            std::ostringstream o;
            o<<"{\"dma2_chcr\":"<<vm.m_ee->m_dmac.GetRegister(CDMAC::D2_CHCR)<<",\"dma1_chcr\":"<<vm.m_ee->m_dmac.GetRegister(CDMAC::D1_CHCR)
             <<",\"gif_active_path\":"<<vm.m_ee->m_gif.GetActivePath()<<"}";dmaReport=o.str();
        }
        gs->FlushWriteBuffer();
        gs->SendGSCall([]{},true);
        {std::lock_guard lock(gs->mutex);gs->flushTransfer();}
        const uint8* vram=gs->GetRam();
        writeFresh(outDir/"vram.bin",vram,CGSHandler::RAMSIZE);
        writeFresh(outDir/"ee-ram-32mb.bin",ram,0x2000000);

        auto s16at=[&](uint32 a){int16 v;std::memcpy(&v,ram+(a&0x1ffffff),2);return int(v);};
        auto u64at=[&](uint32 a){uint64 v;std::memcpy(&v,ram+(a&0x1ffffff),8);return v;};
        auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
        auto allocId=[](uint64 d){return d?int((d>>20)&0xfff):-1;};
        std::ostringstream out;
        {
            const uint32 R=get32(0x3a5af0),X=get32(0x3a5af4),rec=get32(0x3a7b00),extraDesc=get32(0x3a7b04),extraNext=get32(0x3a7b08),hashT=get32(0x3a7b0c);
            const int hsize=int(get32(0x2138c0));
            out<<"{\"kind\":\"final_state\",\"pc\":"<<hx(cpu.nPC)<<",\"handles\":[";
            bool first=true;
            for(int id=0;id<0x1000;++id){const int r=s16at(0x3a5b00+id*2);if(r>=0){out<<(first?"":",")<<'['<<id<<','<<r<<']';first=false;}}
            out<<"],\"records\":[";
            std::vector<int> live;
            if(R){int e=s16at(R+2);for(int n=0;e!=1&&n<=0x208;++n){require(e>=8&&e<0x208,"record list node");live.push_back(e-8);e=s16at(R+e*4+2);}}
            std::sort(live.begin(),live.end());first=true;
            for(int r:live){const uint32 b=rec+r*0x30;const std::string name=cstr(b+0x10);
                out<<(first?"":",")<<"{\"r\":"<<r<<",\"image\":"<<allocId(u64at(b))<<",\"pal0\":"<<allocId(u64at(b+8))
                   <<",\"name\":\""<<esc(name)<<"\",\"hash_slot\":"<<s16at(b+0x24)<<",\"handles\":"<<(get32(b+0x2c)&0xffff)<<",\"extra\":[";
                first=false;bool f2=true;int e=s16at(b+0x2e);
                for(int n=0;e!=-1;++n){require(n<=0x80&&e>=0&&e<0x80,"extra chain");
                    out<<(f2?"":",")<<allocId(u64at(extraDesc+e*8));f2=false;e=s16at(extraNext+e*2);}
                out<<"]}";}
            out<<"],\"hash\":[";first=true;
            for(int i=0;hashT&&i<hsize;++i){const int v=s16at(hashT+i*2);if(v){out<<(first?"":",")<<'['<<i<<','<<v<<']';first=false;}}
            out<<"],\"live_handles\":"<<int(get32(0x3a5af8))<<",\"reserved\":[";
            {first=true;int e=s16at(0x3a1ad0+2*4+2);for(int n=0;e!=4;++n){require(n<=0x1008&&e>=8&&e<0x1008,"reserved queue node");
                out<<(first?"":",")<<(e-8);first=false;e=s16at(0x3a1ad0+e*4+2);}}
            auto freeCount=[&](uint32 pool,int limit){int c=0;if(!pool)return 0;int e=s16at(pool+6*4+2);
                while(e!=7){require(c<=limit&&e>=8&&e<limit+8,"free list node");++c;e=s16at(pool+e*4+2);}return c;};
            out<<"],\"free_records\":"<<freeCount(R,0x200)<<",\"free_extras\":"<<freeCount(X,0x80)<<",\"descriptors\":[";
            std::vector<std::pair<int,int>> d;
            {int e=s16at(0x3adc20+0x1000*6+4);
             for(int n=0;e!=0x1001;++n){require(n<=0x1000&&e>=0&&e<0x1000,"descriptor ring node");d.push_back({e,s16at(0x3adc20+e*6)});e=s16at(0x3adc20+e*6+4);}
             std::sort(d.begin(),d.end());first=true;
             for(auto& p:d){out<<(first?"":",")<<'['<<p.first<<','<<p.second<<']';first=false;}}
            out<<"],\"allocations\":[";first=true;
            for(const auto& kv:allocs){
                int refs=-1;for(auto& p:d)if(uint64(p.first)==kv.first)refs=p.second;
                const auto& a=kv.second;
                out<<(first?"":",")<<"{\"id\":"<<kv.first<<",\"class\":"<<a.cls<<",\"req_psm\":"<<a.psm<<",\"req_w\":"<<a.w<<",\"req_h\":"<<a.h
                   <<",\"desc\":"<<descJson(a.desc)<<",\"live\":"<<(refs>=0?"true":"false")<<",\"refs\":"<<refs<<"}";first=false;}
            out<<"],\"vram\":{\"available\":"<<get32(0x3adc1c)<<",\"cursors\":[";
            for(unsigned j=0;j<64;++j)out<<(j?",":"")<<get32(0x3adb18+j*4);
            out<<"],\"occupancy_sha256\":\""<<sha(ram+0x3abb18,8192)<<"\",\"nodes_sha256\":\""<<sha(ram+0x3adc20,4096*6+24)
               <<"\",\"run_fill\":"<<unsigned(fill)<<",\"run_vram_sha256\":\""<<sha(vram,CGSHandler::RAMSIZE)<<"\"},"
               <<randState()<<",\"free_count_0x3adc1c\":"<<get32(0x3adc1c)<<",\"ee_ram_32mb_sha256\":\""<<sha(ram,0x2000000)
               <<"\",\"ee_code_sha256\":\""<<sha(ram+0x100000,0x1f8434-0x100000)<<"\"}\n";
        }
        writeFresh(outDir/"final-state.jsonl",out.str().data(),out.str().size());

        // GS ledger
        {
            std::ostringstream o;
            {std::lock_guard lock(gs->mutex);
             for(const auto& l:gs->lines)o<<l<<'\n';
             o<<"{\"kind\":\"gs_summary\",\"register_writes\":"<<gs->regWrites<<",\"vertex_kicks\":"<<gs->kicks<<",\"host_to_local_transfers\":"<<gs->transfers
              <<",\"host_to_local_completed\":"<<gs->hostToLocalDone<<",\"local_to_host\":"<<gs->localToHost<<",\"local_to_local\":"<<gs->localToLocal
              <<",\"clut_transfers\":"<<gs->clutTransfers<<",\"resets\":"<<gs->resets<<",\"register_histogram\":{";
             bool first=true;for(const auto& kv:gs->regHistogram){o<<(first?"":",")<<'"'<<unsigned(kv.first)<<"\":"<<kv.second;first=false;}
             o<<"},\"dma_gif_at_stop\":"<<dmaReport<<"}\n";}
            writeFresh(outDir/"gs-ledger.jsonl",o.str().data(),o.str().size());
        }
        // Disc opens
        {
            std::ostringstream o;{std::lock_guard lock(traced->mutex);for(const auto& r:traced->rows)o<<r<<'\n';}
            writeFresh(outDir/"disc-opens.jsonl",o.str().data(),o.str().size());
        }
        events.flush();events.close();
        {
            std::ostringstream o;
            o<<"{\"fill\":"<<unsigned(fill)<<",\"fill_report\":"<<fillReport<<",\"events\":"<<evCount<<",\"breakpoint_stops\":"<<stops<<",\"benign_block_start_stops\":"<<benignBlockStops<<",\"rand_calls\":"<<randCalls<<",\"minimal\":"<<(minimal?"true":"false")
             <<",\"vblanks\":"<<frames.load()<<",\"wall_seconds\":"<<std::fixed<<std::setprecision(2)<<seconds
             <<",\"open_frames_at_stop\":[";
            for(size_t i=0;i<stack.size();++i)o<<(i?",":"")<<"\""<<stack[i].name<<"\"";
            o<<"],\"stack_depth_at_stop\":"<<stack.size()<<",\"static_return_sites\":"<<returnSites.size()<<"}\n";
            writeFresh(outDir/"run-meta.json",o.str().data(),o.str().size());
        }
        finishing=true;
        std::cout<<"OK events="<<evCount<<" stops="<<stops<<" vblanks="<<frames.load()<<" seconds="<<seconds<<std::endl;
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
