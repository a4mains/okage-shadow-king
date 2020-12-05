// Research-only EE/VU reference. Never linked into the native game or asset tools.
#include "AppConfig.h"
#include "ee/Ee_SubSystem.h"
#include "ee/EeExecutor.h"
#include "iop/Iop_SubSystem.h"
#include "iop/IopBios.h"
#include "Ps2Const.h"
#include "FpUtils.h"
#include "okage/model.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

fs::path CAppConfig::GetBasePath() const {
    auto path=std::filesystem::current_path()/"local/play-oracle-config";
    std::filesystem::create_directories(path);
    return path.string();
}
std::vector<uint8> readFile(const char* path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if (!in) throw std::runtime_error("Cannot open input");
    auto size=in.tellg();
    if (size<0 || size>32*1024*1024) throw std::runtime_error("Input size invalid");
    std::vector<uint8> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("Short input");
    return bytes;
}
void writeFile(const char* path,const void* bytes,size_t size) {
    if (std::filesystem::is_symlink(path)) throw std::runtime_error("Refusing symlink output");
    if (std::filesystem::exists(path)) {
        const auto old=readFile(path);
        if (old.size()!=size || std::memcmp(old.data(),bytes,size))
            throw std::runtime_error("Refusing different existing output");
        return;
    }
    std::ofstream out(path,std::ios::binary);
    if (!out.write(static_cast<const char*>(bytes),size)) throw std::runtime_error("Cannot write output");
}
std::string hash(const std::vector<uint8>& data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(),static_cast<CC_LONG>(data.size()),digest);
    std::ostringstream value;
    for (auto byte:digest) value<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
    return value.str();
}

class Reference {
public:
    std::unique_ptr<Iop::CSubSystem> iop;
    std::unique_ptr<Ee::CSubSystem> ee;
    static constexpr uint32 stop=0x007ff000;
    Reference(const std::vector<uint8>& elf) {
        if (hash(elf)!="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")
            throw std::runtime_error("Oracle requires the pinned USA ELF");
        iop=std::make_unique<Iop::CSubSystem>(true);
        auto* bios=dynamic_cast<CIopBios*>(iop->m_bios.get());
        if (!bios) throw std::runtime_error("Missing research IOP BIOS implementation");
        ee=std::make_unique<Ee::CSubSystem>(iop->m_ram,*bios);
        ee->Reset(PS2::EE_RAM_SIZE);
        std::memcpy(ee->m_ram+0x100000,elf.data()+4096,1369296);
        put(stop,0x08000000|(stop>>2)); put(stop+4,0);
        ee->m_EE.ToggleBreakpoint(stop);
        static_cast<CEeExecutor*>(ee->m_EE.m_executor.get())->AddExceptionHandler();
        std::fesetround(FE_TOWARDZERO);
        FpUtils::SetDenormalHandlingMode();
    }
    ~Reference() {
        if (ee) static_cast<CEeExecutor*>(ee->m_EE.m_executor.get())->RemoveExceptionHandler();
        std::fesetround(FE_TONEAREST);
    }
    void call(uint32 address,std::initializer_list<uint32> args={}) {
        auto& cpu=ee->m_EE;
        cpu.m_State.nPC=address;
        cpu.m_State.nHasException=0;
        cpu.m_State.nDelayedJumpAddr=MIPS_INVALID_PC;
        cpu.m_State.nGPR[CMIPS::SP].nD0=0x01ff0000;
        cpu.m_State.nGPR[CMIPS::RA].nD0=stop;
        cpu.m_State.nGPR[CMIPS::GP].nD0=0x002564f0;
        unsigned reg=CMIPS::A0;
        for (auto arg:args) cpu.m_State.nGPR[reg++].nD0=arg;
        for (unsigned step=0;step<1000000;++step) {
            bool executed=false;
            if (!cpu.m_State.callMsEnabled) {
                cpu.m_executor->Execute(1000); executed=true;
            }
            if (cpu.m_State.nHasException==MIPS_EXCEPTION_VU_CALLMS) {
                ee->m_vpu0->ExecuteMicroProgram(cpu.m_State.callMsAddr);
                cpu.m_State.nHasException=0;
            } else if (cpu.m_State.nHasException) {
                std::ostringstream error;
                error<<"Unexpected oracle exception "<<cpu.m_State.nHasException<<" at PC "<<std::hex<<cpu.m_State.nPC;
                throw std::runtime_error(error.str());
            }
            ee->m_vpu0->Execute(1000);
            if (cpu.m_State.callMsEnabled && ee->m_vpu0->IsVuReady()) {
                cpu.m_State.callMsAddr=ee->m_VU0.m_State.nPC;
                cpu.m_State.callMsEnabled=0;
            }
            if (executed && cpu.m_executor->MustBreak()) {
                if (cpu.m_State.nPC!=stop) throw std::runtime_error("Unexpected oracle breakpoint");
                return;
            }
        }
        std::ostringstream error; error<<"Oracle instruction budget exceeded at "<<std::hex<<cpu.m_State.nPC;
        throw std::runtime_error(error.str());
    }
    uint32 get(uint32 address) const { uint32 v; std::memcpy(&v,ee->m_ram+address,4); return v; }
    void put(uint32 address,uint32 value) { std::memcpy(ee->m_ram+address,&value,4); }
    void loadMicro() {
        std::memcpy(ee->m_microMem0,ee->m_ram+0x21ab00,0x660);
        put(0x01ff0000,stop); put(0x01ff0004,0);
        call(0x1b1eb4);
    }
    void animation(const std::vector<uint8>& bytes,unsigned channels,unsigned entry,unsigned frames,const char* output,const char* modelPath=nullptr) {
        if (bytes.size()<32 || bytes.size()>0x700000 || channels==0 || channels>5120 || frames>10000 ||
            (std::uint64_t(frames)+1)*(8+16*channels)>32*1024*1024)
            throw std::runtime_error("Invalid animation oracle bounds");
        std::memcpy(ee->m_ram+0x900000,bytes.data(),bytes.size());
        put(0x213014,0x800000); // Group records.
        put(0x33760c,0x1000000); // Original 0x90-byte channel records.
        put(0x337614,0x850000); // 192 script contexts.
        put(0x337610,1024); put(0x337928,0);
        call(0x1b5d10,{channels,0,0,get(0x900018)!=0});
        call(0x1b5bd8,{0,0x900000});
        for (unsigned ch=0;ch<channels;++ch) call(0x1b43c0,{ch,0x1800000});
        call(0x1b5dd0,{0,entry});
        uint32 bones=0,hierarchy=0,boneCount=0,nodeCount=0;
        if (modelPath) {
            const auto modelBytes=readFile(modelPath);
            const auto model=okage::read_xmd(modelBytes);
            if (model.models.size()!=1 || modelBytes.size()>0x100000 || get(0x900014)!=0)
                throw std::runtime_error("Animation pose oracle currently requires one model and two channels per bone");
            boneCount=model.models[0].bones.size(); nodeCount=model.models[0].hierarchy.size();
            if (channels!=4+boneCount*2 || nodeCount>4096 ||
                (uint64(frames)+1)*(8+16*channels+64*nodeCount)>32*1024*1024)
                throw std::runtime_error("Animation pose oracle bounds mismatch");
            std::memcpy(ee->m_ram+0x1700000,modelBytes.data(),modelBytes.size());
            put(0x1fe250,0); put(0x1fe2ac,0x820000); put(0x1fe2b0,0); put(0x330184,0);
            put(0x33014c,0x1700000+model.models[0].offset);
            call(0x1caa20,{0x1400000}); call(0x1829c0); call(0x1827e0);
            bones=get(0x820000); hierarchy=get(0x820004);
            for (unsigned i=0;i<nodeCount;++i) {
                uint16 flag=0; std::memcpy(ee->m_ram+hierarchy+14+i*8,&flag,2);
            }
            call(0x1b1d88); loadMicro();
        }
        std::vector<uint8> dump;
        auto append=[&](const void* p,size_t n) {
            const auto* data=static_cast<const uint8*>(p); dump.insert(dump.end(),data,data+n);
        };
        for (unsigned frame=0;frame<=frames;++frame) {
            // Same order as 0x1b5e38: channels first, script countdown second.
            if (frame) { call(0x1b4748,{0,channels}); call(0x1b5a90); }
            const uint32 state[]={get(0x850014)-0x900000,get(0x850000)};
            append(state,sizeof(state));
            for (unsigned ch=0;ch<channels;++ch) {
                const auto at=0x1000000+ch*0x90;
                if (get(at)&0x200) call(0x1b3c78,{at});
                append(ee->m_ram+at+0x20,16);
            }
            if (modelPath) {
                call(0x1b4480,{4,boneCount,bones+16});
                call(0x184a20,{hierarchy,bones+16,0x1800000,0});
                append(ee->m_ram+0x1800000,nodeCount*64);
            }
        }
        writeFile(output,dump.data(),dump.size());
        std::cout<<"Executed retail animation: "<<channels<<" channels, "<<frames+1<<" samples\n";
    }
    size_t pose(const std::vector<uint8>& bytes,const char* output) {
        const auto decoded=okage::read_xmd(bytes);
        if (decoded.models.size()!=1 || bytes.size()>0x700000) throw std::runtime_error("Oracle expects one bounded model");
        const auto count=decoded.models[0].hierarchy.size();
        if (count>4096) throw std::runtime_error("Oracle hierarchy too large");
        std::memcpy(ee->m_ram+0x900000,bytes.data(),bytes.size());
        put(0x1fe250,0);
        put(0x1fe2ac,0x800000); put(0x1fe2b0,0); put(0x330184,0);
        put(0x33014c,0x900000+decoded.models[0].offset);
        call(0x1caa20,{0x1000000});
        call(0x1829c0);
        call(0x1827e0); // Original hierarchy conversion.
        const auto bones=get(0x800000), hierarchy=get(0x800004);
        for (size_t i=0;i<count;++i) {
            uint16 flag=0;
            std::memcpy(ee->m_ram+hierarchy+14+i*8,&flag,2);
        }
        call(0x184a20,{hierarchy,bones+16,0x1800000,0});
        writeFile(output,ee->m_ram+0x1800000,count*64);
        return count;
    }
};

int main(int argc,char** argv) {
    try {
        if (argc<4) throw std::runtime_error("Usage: okage-pose-oracle sine ELF OUTPUT | vu-asm ELF OUTPUT | pose ELF XMD OUTPUT | pose-corpus ELF DIRECTORY OUTDIR");
        Reference reference(readFile(argv[2]));
        const std::string command=argv[1];
        if (command=="animation" && (argc==8 || argc==9)) {
            reference.animation(readFile(argv[3]),std::stoul(argv[4]),std::stoul(argv[5]),std::stoul(argv[6]),argv[7],argc==9 ? argv[8] : nullptr);
        } else if (command=="sine" && argc==4) {
            reference.call(0x001b1d88);
            writeFile(argv[3],reference.ee->m_ram+0x00202ee0,16384*4);
            std::cout<<"Executed retail sine initializer; wrote 16384 float entries\n";
        } else if (command=="vu-asm" && argc==4) {
            reference.loadMicro();
            auto& cpu=reference.ee->m_VU0;
            std::ostringstream listing;
            for (uint32 at=0;at<0x660;at+=4) {
                uint32 word; std::memcpy(&word,reference.ee->m_microMem0+at,4);
                char name[256]{},operands[256]{};
                cpu.m_pArch->GetInstructionMnemonic(&cpu,at,word,name,sizeof(name));
                cpu.m_pArch->GetInstructionOperands(&cpu,at,word,operands,sizeof(operands));
                listing<<std::hex<<std::setw(4)<<std::setfill('0')<<at<<" "<<name<<" "<<operands<<'\n';
            }
            auto text=listing.str(); writeFile(argv[3],text.data(),text.size());
        } else if (command=="pose" && argc==5) {
            reference.call(0x1b1d88);
            reference.loadMicro();
            std::cout<<"Executed retail EE/VU pose: "<<reference.pose(readFile(argv[3]),argv[4])<<" matrices\n";
        } else if (command=="pose-corpus" && argc==5) {
            reference.call(0x1b1d88); reference.loadMicro();
            std::vector<std::filesystem::path> paths;
            for (const auto& entry:std::filesystem::recursive_directory_iterator(argv[3]))
                if (entry.is_regular_file() && entry.path().extension()==".xmd") paths.push_back(entry.path());
            std::sort(paths.begin(),paths.end());
            if (paths.empty()) throw std::runtime_error("Empty model corpus");
            for (const auto& path:paths) {
                auto output=std::filesystem::path(argv[4])/std::filesystem::relative(path,argv[3]);
                output.replace_extension(".pose"); std::filesystem::create_directories(output.parent_path());
                auto bytes=readFile(path.c_str());
                const auto count=reference.pose(bytes,output.c_str());
                std::cout<<path.string()<<'\t'<<hash(bytes)<<'\t'<<output.string()<<'\t'<<count<<std::endl;
            }
        } else throw std::runtime_error("Invalid command or argument count");
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
