// Research-only EE/VU reference. Never linked into the native game or asset tools.
#include "AppConfig.h"
#include "ee/Ee_SubSystem.h"
#include "ee/EeExecutor.h"
#include "ee/Vif.h"
#include "ee/PS2OS.h"
#include "iop/Iop_SubSystem.h"
#include "iop/IopBios.h"
#include "iop/Iop_SpuBase.h"
#include "Ps2Const.h"
#include "FpUtils.h"
#include "gs/GSH_Null.h"
#include "FrameDump.h"
#ifdef OKAGE_REFERENCE_VULKAN
#include "reference_graphics.hpp"
#endif
#include "okage/model.hpp"
#include "okage/texture_animation.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <functional>
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

// Packet-only research sink. No rasterization, texture upload, thread or window.
class MapPacketGs final : public CGSHandler {
public:
    MapPacketGs():CGSHandler(false) {}
    // Nonthreaded GS still uses its mailbox. Drain before reading observations
    // or reusing a register-write buffer; no display/Flip implementation runs.
    void drain() {SendGSCall([this]{m_flipped=true;});ProcessSingleFrame();}
    void begin() {drain();m_frameDump=std::make_unique<CFrameDump>();}
    const CFrameDump::PacketArray& packets() const {return m_frameDump->GetPackets();}
    void ProcessHostToLocalTransfer() override {throw std::runtime_error("Unexpected map image transfer");}
    void ProcessLocalToHostTransfer() override {throw std::runtime_error("Unexpected map image readback");}
    void ProcessLocalToLocalTransfer() override {throw std::runtime_error("Unexpected map local transfer");}
    void ProcessClutTransfer(uint32,uint32) override {} // No pixels claimed; register observation only.
private:
    void InitializeImpl() override {}
    void ReleaseImpl() override {}
};

class Reference {
public:
    std::unique_ptr<Iop::CSubSystem> iop;
#ifdef OKAGE_REFERENCE_VULKAN
    ResearchTraffic researchTraffic;
#endif
    std::unique_ptr<CGSHandler> researchGs;
    std::unique_ptr<Ee::CSubSystem> ee;
    std::unique_ptr<CPS2OS> effectOs;
    bool streamTransferProbe=false;
    bool templateLoadProbe=false;
    std::vector<uint32> templateRandomLog;
    static constexpr uint32 stop=0x007ff000;
    uint32 expectedStop=stop;
    unsigned callIterationLimit=1000000;
    std::function<bool(uint32)> boundedInstructionProbe;
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
        if(researchGs)researchGs->Release();
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
        for (unsigned step=0;step<callIterationLimit;++step) {
            bool executed=false;
            if (!cpu.m_State.callMsEnabled) {
                cpu.m_executor->Execute(1000); executed=true;
            }
            if (cpu.m_State.nHasException==MIPS_EXCEPTION_VU_CALLMS) {
                ee->m_vpu0->ExecuteMicroProgram(cpu.m_State.callMsAddr);
                cpu.m_State.nHasException=0;
            } else if(effectOs && cpu.m_State.nHasException==MIPS_EXCEPTION_SYSCALL) {
                auto function=cpu.m_State.nGPR[CMIPS::V1].nV[0];
                if(function&0x80000000)function=0-function;
                if(function!=0x2f&&function!=0x48&&function!=0x44&&function!=0x42&&function!=0x45)
                    throw std::runtime_error("Unexpected effect OS syscall "+std::to_string(function));
                if(function!=0x2f) {
                    const auto id=cpu.m_State.nGPR[CMIPS::A0].nV[0];
                    if(id>=256||!get(0xe000+id*24)||get(0xe000+id*24+12)||
                       (function==0x44&&!get(0xe000+id*24+4)))
                        throw std::runtime_error("Effect semaphore would need scheduling");
                }
                const auto pc=cpu.m_State.nPC,thread=get(0x4014);
                effectOs->HandleSyscall();
                if(cpu.m_State.nPC!=pc||get(0x4014)!=thread)
                    throw std::runtime_error("Unexpected effect OS reschedule");
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
                if(boundedInstructionProbe&&boundedInstructionProbe(cpu.m_State.nPC)) {
                    cpu.m_executor->DisableBreakpointsOnce();continue;
                }
                if(templateLoadProbe&&cpu.m_State.nPC==0x1f14b0) {
                    if(templateRandomLog.size()>=2*10000)throw std::runtime_error("Template RNG observation limit");
                    templateRandomLog.insert(templateRandomLog.end(),{
                        cpu.m_State.nGPR[CMIPS::RA].nV[0],get(get(0x2151b4)+0x58)});
                    // Observe entry, then execute the original function unchanged.
                    cpu.m_executor->DisableBreakpointsOnce();continue;
                }
                if(streamTransferProbe && cpu.m_State.nPC==0x16e300) {
                    const auto dst=cpu.m_State.nGPR[CMIPS::A0].nV[0];
                    const auto src=cpu.m_State.nGPR[CMIPS::A1].nV[0];
                    const auto size=cpu.m_State.nGPR[CMIPS::A2].nV[0];
                    if(dst<0x40000 || dst>0x140000 || size>0x140000-dst ||
                       src>32*1024*1024 || size>32*1024*1024-src)
                        throw std::runtime_error("Stream probe DMA bounds");
                    std::memcpy(iop->m_ram+dst,ee->m_ram+src,size);
                    cpu.m_State.nGPR[CMIPS::V0].nD0=1;
                    cpu.m_State.nPC=cpu.m_State.nGPR[CMIPS::RA].nV[0];
                    cpu.m_State.nDelayedJumpAddr=MIPS_INVALID_PC;
                    continue;
                }
                if (cpu.m_State.nPC!=expectedStop) {
                    std::ostringstream error;error<<"Unexpected oracle breakpoint at "<<std::hex<<cpu.m_State.nPC
                        <<", wanted "<<expectedStop;throw std::runtime_error(error.str());
                }
                return;
            }
        }
        std::ostringstream error; error<<"Oracle instruction budget exceeded at "<<std::hex<<cpu.m_State.nPC
            <<" RA="<<cpu.m_State.nGPR[CMIPS::RA].nV[0]<<" A0="<<cpu.m_State.nGPR[CMIPS::A0].nV[0]
            <<" A1="<<cpu.m_State.nGPR[CMIPS::A1].nV[0]<<" A2="<<cpu.m_State.nGPR[CMIPS::A2].nV[0];
        throw std::runtime_error(error.str());
    }
    void streamPrepare(const std::vector<uint8>& source,unsigned ring,const char* output) {
        if(source.empty()||source.size()%16||source.size()>512*1024||ring<64||ring>0x10000||ring%64)
            throw std::runtime_error("Stream prepare fixture bounds");
        constexpr uint32 context=0x2fd644,input=0x800000;
        std::memset(ee->m_ram+context,0,64);
        ee->m_ram[context+5]=1;
        put(context+0x14,0x40000);put(context+0x18,0x100000);
        put(context+0x2c,ring);
        std::memset(ee->m_ram+0x2f2e80,0,64);
        std::memcpy(ee->m_ram+input,source.data(),source.size());
        streamTransferProbe=true;ee->m_EE.ToggleBreakpoint(0x16e300);
        std::vector<uint8> commands;
        auto collect=[&] {
            const auto n=get(0x1fe200);
            if(n>4096||get(0x1fe204))throw std::runtime_error("Stream command extent");
            commands.insert(commands.end(),ee->m_ram+0x3072c0,ee->m_ram+0x3072c0+n);
            put(0x1fe200,0);put(0x1fe204,0);
        };
        put(0x1fe200,0);put(0x1fe204,0);
        for(unsigned at=0;at<source.size();) {
            const auto count=std::min<unsigned>(4096,source.size()-at);
            call(0x173088,{0,input+at,0,count,0});collect();at+=count;
        }
        call(0x173940,{0});collect();
        if(ee->m_ram[context+8]!=1)throw std::runtime_error("Stream EOF not set");
        const auto extent=get(context+0x20)+64;
        if(extent!=(source.size()+63)/64*64+64)throw std::runtime_error("Stream EOF extent");
        writeFile(output,iop->m_ram+0x40000,extent);
        writeFile((std::string(output)+".commands.bin").c_str(),commands.data(),commands.size());
        writeFile((std::string(output)+".context.bin").c_str(),ee->m_ram+context,64);
        ee->m_EE.ToggleBreakpoint(0x16e300);streamTransferProbe=false;
    }
    void exteriorCameraStep(const std::vector<uint8>& ram,const std::vector<uint8>& cases,const char* output) {
        if(ram.size()!=32*1024*1024||cases.empty()||cases.size()%184||cases.size()>184*2000)
            throw std::runtime_error("Exterior camera fixture extent");
        std::vector<uint32> result;
        for(unsigned at=0;at<cases.size();at+=184) {
            std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
            std::memcpy(ee->m_microMem0,ee->m_ram+0x21ab00,0x660);
            std::array<uint32,46> row;std::memcpy(row.data(),cases.data()+at,184);
            if(row[2]!=0||row[44]>65535||row[45]>255)throw std::runtime_error("Exterior camera oracle input scope");
            const auto objects=get(0x21301c),hero=get(objects+get(0x337b64)*0x3c+4)&0xffff,camera=get(0x337d8c);
            auto set=[&](uint32 id,unsigned index) {
                std::memcpy(ee->m_ram+0x80000,row.data()+index,16);call(0x1b4220,{id,0x80000});call(0x1b43c0,{id,0x80000});
            };
            set(hero+1,28);set(hero,32);set(camera,36);
            for(unsigned i=0;i<22;++i)put(0x2d9c28+4*i,row[i]);
            for(unsigned i=0;i<6;++i)put(0x1fb9a0+4*i,row[22+i]);
            put(0x1fb970,0);put(0x33958c,row[44]);ee->m_ram[0x3395b8]=row[45];
            call(0x136d00);
            for(unsigned i=0;i<22;++i)row[i]=get(0x2d9c28+4*i);
            for(unsigned i=0;i<6;++i)row[22+i]=get(0x1fb9a0+4*i);
            call(0x1b43c0,{camera,0x80000});for(unsigned i=0;i<4;++i)row[36+i]=get(0x80000+4*i);
            for(unsigned i=0;i<4;++i)row[40+i]=get(0x337db0+4*i);
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void enemyStep(const std::vector<uint8>& ram,const std::vector<uint8>& cases,const char* output) {
        if(ram.size()!=32*1024*1024||cases.empty()||cases.size()%144||cases.size()>144*2000)
            throw std::runtime_error("Enemy oracle fixture extent");
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                        ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        std::vector<uint32> result;
        for(std::size_t at=0;at<cases.size();at+=144) {
            std::cout<<"enemy_case="<<at/144<<std::endl;
            std::memcpy(ee->m_ram,ram.data(),ram.size());
            put(stop,0x08000000|(stop>>2));put(stop+4,0);
            std::memcpy(ee->m_microMem0,ee->m_ram+0x21ab00,0x660);
            std::array<uint32,36> row;std::memcpy(row.data(),cases.data()+at,144);
            const auto field=get(0x2d9bf4),player=get(field+0x44),vars=get(0x24f788);
            if(get(field+0x70)!=7)throw std::runtime_error("Enemy fixture requires exterior seven-NPC snapshot");
            const auto npc=get(field+0x118+2*4),instance=get(npc+0x40),objects=get(0x21301c);
            auto first=[&](uint32 id){if(id>=2048)throw std::runtime_error("Enemy fixture instance");return get(objects+id*0x3c+4)&0xffff;};
            const auto channel=first(instance),hero=first(get(0x337b64)),camera=get(0x337d8c),object=objects+instance*0x3c;
            auto set=[&](uint32 id,unsigned index){
                std::memcpy(ee->m_ram+0x80000,row.data()+index,16);call(0x1b4220,{id,0x80000});
                call(0x1b43c0,{id,0x80000});
            };
            set(channel+1,12);set(channel,16);set(channel+3,20);set(hero+1,24);set(hero,28);set(camera+1,32);
            put(npc+0x3c,row[0]);put(npc+0x50,row[1]);put(npc+0x54,row[2]);
            const auto random=get(0x2151b4)+0x58;put(random,row[3]);
            put(vars+0x510+6*4,row[4]);put(player+0x230,row[5]);put(field+0x48,row[6]);
            put(vars+0x510+4*4,row[7]);call(0x1baa60,{instance,row[8]});put(object+0x24,row[9]);
            put(field+0xfc,row[10]);const auto oldMode=row[6];
            call(0x137218,{npc});
            row[0]=get(npc+0x3c);row[1]=get(npc+0x50);row[2]=get(npc+0x54);row[3]=get(random);
            row[6]=get(field+0x48);row[8]=(get(object)&0x800)!=0;row[9]=get(object+0x24);row[10]=get(field+0xfc);
            if(oldMode==0&&(row[6]==8||row[6]==11))row[11]=(row[3]&0x7fffffff)%24;
            auto read=[&](uint32 id,unsigned index){call(0x1b43c0,{id,0x80000});std::memcpy(row.data()+index,ee->m_ram+0x80000,16);};
            read(channel+1,12);read(channel,16);read(channel+3,20);
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    uint32 get(uint32 address) const { uint32 v; std::memcpy(&v,ee->m_ram+address,4); return v; }
    void put(uint32 address,uint32 value) { std::memcpy(ee->m_ram+address,&value,4); }
    void put64(uint32 address,uint64 value) { std::memcpy(ee->m_ram+address,&value,8); }
    uint64 get64(uint32 address) const { uint64 value; std::memcpy(&value,ee->m_ram+address,8); return value; }
    void spuSample(const std::vector<uint8>& blocks,const char* output) {
        if(blocks.empty() || blocks.size()%16 || blocks.size()>1024*1024)
            throw std::runtime_error("SPU sample fixture bounds");
        std::vector<uint8> ram(2*1024*1024);
        constexpr uint32 start=0x10000;
        std::memcpy(ram.data()+start,blocks.data(),blocks.size());
        Iop::CSpuSampleCache cache;Iop::CSpuIrqWatcher irq;
        Iop::CSpuBase spu(ram.data(),ram.size(),&cache,&irq,0);
        spu.SetReverbEnabled(false);spu.SetBaseSamplingRate(48000);spu.SetDestinationSamplingRate(48000);
        auto& voice=spu.GetChannel(0);voice.address=start;voice.repeat=start;voice.pitch=0x1000;
        spu.OnChannelPitchChanged(0);spu.SendKeyOn(1);
        std::vector<int16> pcm;
        int32 s1=0,s2=0;
        for(uint32 at=0;at<blocks.size();at+=16) {
            if((blocks[at]>>4)>=5)throw std::runtime_error("SPU reference predictor bounds");
            const auto key=(blocks[at]>>4)==0 ? Iop::CSpuSampleCache::KEY{start+at,0,0} :
                                              Iop::CSpuSampleCache::KEY{start+at,s1,s2};
            const Iop::CSpuSampleCache::ITEM* item=cache.GetItem(key);
            for(unsigned ticks=0;!item && ticks<100;++ticks) {
                int16 stereo[2];spu.Render(stereo,2);item=cache.GetItem(key);
            }
            if(!item)throw std::runtime_error("Reference SPU did not decode expected sample block");
            pcm.insert(pcm.end(),std::begin(item->samples),std::end(item->samples));
            s1=item->outS1;s2=item->outS2;
            if(blocks[at+1]&1) {writeFile(output,pcm.data(),pcm.size()*2);return;}
        }
        throw std::runtime_error("Reference SPU fixture lacks end block");
    }
    void dialogueWait(const std::vector<uint8>& cases,const char* output) {
        if(cases.empty() || cases.size()%48)throw std::runtime_error("Dialogue fixture record bounds");
        std::vector<uint32> results;
        for(std::size_t at=0;at<cases.size();at+=48) {
            std::array<uint32,12> in;std::memcpy(in.data(),cases.data()+at,48);
            if(in[8]>16 || (in[10]!=0 && in[10]!=0xffffffff) || in[6]>1)
                throw std::runtime_error("Dialogue fixture input bounds");
            if(!in[6] && in[0]!=0 && in[0]!=0xffffffff && !in[3])
                throw std::runtime_error("Original progress wait would dereference missing message");
            std::memset(ee->m_ram+0x800000,0,0x800);
            std::memset(ee->m_ram+0x900000,0,0x1000);
            put(0x2d9bf4,0x800000);put(0x800048,in[1]);put(0x800030,in[2]);
            put(0x800054,in[3]);put(0x800058,in[4]);put(0x80005c,in[5]);
            put(0x800050,in[6] ? 0x900000 : 0);put(0x90001c,in[7]);put(0x900e40,in[8]);
            for(unsigned i=0;i<in[8];++i)put(0x900090+i*0xe0,(in[9]&(1u<<i)) ? 0x40 : 0);
            put(0x900120,in[10]);put(0x200688,0xa00000);put(0xa03d08,in[11]);
            // Signed o32 argument: unlike an address, -1 must be sign-extended.
            ee->m_EE.m_State.nGPR[CMIPS::A0].nD0=int64(int32(in[0]));
            call(0x130458);
            results.insert(results.end(),{get(0x800054),get(0x800058),get(0x80005c),
                           uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)});
        }
        writeFile(output,results.data(),results.size()*4);
    }
    void nameExpansion(const std::vector<uint8>& ram,const char* raw,const char* name,const char* output) {
        const std::string sourceText(raw),playerName(name);
        const auto printable=[](const std::string& text) {
            return std::all_of(text.begin(),text.end(),[](unsigned char c){return c>=32&&c<127;});
        };
        if(ram.size()!=32*1024*1024||sourceText.empty()||sourceText.size()>200||
           playerName.empty()||playerName.size()>12||!printable(sourceText)||!printable(playerName))
            throw std::runtime_error("Name expansion requires RAM, a 1..200-byte ASCII line and a 1..12-byte ASCII name");
        constexpr uint32 begin=0x147ad0,end=0x147b5c,sp=0x1ff0000,source=0x1f9cb8,destination=sp+0x200;
        const std::vector<uint8> originalCode(ee->m_ram+begin,ee->m_ram+end);
        std::memcpy(ee->m_ram,ram.data(),ram.size());
        if(std::memcmp(ee->m_ram+begin,originalCode.data(),originalCode.size()))
            throw std::runtime_error("Name expansion snapshot differs from pinned original loop");
        put(stop,0x08000000|(stop>>2));put(stop+4,0); // Existing harness return trampoline.
        const auto save=get(0x24f788);
        if(save<0x100000||save>ram.size()-0x900)
            throw std::runtime_error("Name expansion snapshot has invalid variable storage");
        const auto nameAddress=save+0x844;
        call(0x123530,{0xd5});
        const auto marker=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];
        if(marker>ram.size()-13||std::memcmp(ee->m_ram+marker,"############",13))
            throw std::runtime_error("Name expansion requires the owned USA sysmes marker");
        std::memset(ee->m_ram+nameAddress,0,13);
        std::memcpy(ee->m_ram+nameAddress,playerName.c_str(),playerName.size()+1);
        std::memcpy(ee->m_ram+source,sourceText.c_str(),sourceText.size()+1);
        call(0x1f1a5c,{destination,source}); // The original preceding full strcpy.
        const std::vector<uint8> sourceBefore(ee->m_ram+source,ee->m_ram+source+256);
        const std::vector<uint8> destinationBefore(ee->m_ram+destination,ee->m_ram+destination+256);
        put(sp+0x330,save+0x510);
        auto& cpu=ee->m_EE;
        cpu.m_State.nGPR[CMIPS::S1].nD0=source;
        cpu.m_State.nGPR[CMIPS::S2].nD0=destination;
        cpu.m_State.nGPR[CMIPS::S6].nD0=destination;
        std::vector<std::array<uint32,3>> trace;
        constexpr std::array<uint32,3> observations={begin,0x147b18,0x147b48};
        boundedInstructionProbe=[&](uint32 pc) {
            if(pc!=0x147b04&&std::find(observations.begin(),observations.end(),pc)==observations.end())return false;
            const auto src=cpu.m_State.nGPR[CMIPS::S1].nV[0],dst=cpu.m_State.nGPR[CMIPS::S2].nV[0];
            if(trace.size()>=4096||src<source||src>=source+256||dst<destination||dst>=destination+256||
               (pc==begin&&dst>destination+244))
                throw std::runtime_error("Name expansion escaped its bounded scratch fixture");
            if(pc==0x147b18) {
                const auto cursor=cpu.m_State.nGPR[CMIPS::V1].nV[0];
                if(cursor<nameAddress||cursor>=nameAddress+playerName.size())
                    throw std::runtime_error("Name expansion escaped its name bytes");
            }
            trace.push_back({pc,src-source,dst-destination});return true;
        };
        expectedStop=end;cpu.ToggleBreakpoint(end);
        for(auto address:observations)cpu.ToggleBreakpoint(address);
        const auto cleanup=[&] {
            boundedInstructionProbe={};expectedStop=stop;cpu.ToggleBreakpoint(end);
            for(auto address:observations)cpu.ToggleBreakpoint(address);
        };
        try {call(begin);}catch(...){cleanup();throw;}
        cleanup();
        if(std::memcmp(ee->m_ram+source,sourceBefore.data(),sourceBefore.size())||
           std::memcmp(ee->m_ram+nameAddress,playerName.c_str(),playerName.size()+1)||
           std::memcmp(ee->m_ram+begin,originalCode.data(),originalCode.size()))
            throw std::runtime_error("Name expansion changed its source, name or original code");
        const auto terminator=std::find(ee->m_ram+destination,ee->m_ram+destination+256,0);
        if(terminator==ee->m_ram+destination+256)throw std::runtime_error("Name expansion result is unterminated");
        const auto hex=[](const uint8* bytes,std::size_t size) {
            std::ostringstream result;result<<std::hex<<std::setfill('0');
            for(std::size_t i=0;i<size;++i)result<<std::setw(2)<<unsigned(bytes[i]);
            return result.str();
        };
        std::ostringstream result;
        result<<"{\n  \"scope\":\"Original 0x147ad0..0x147b5c only; retained scratch tails, synthetic line/name\",\n"
              <<"  \"ram_sha256\":\""<<hash(ram)<<"\",\n  \"marker_address\":"<<marker
              <<",\n  \"name_address\":"<<nameAddress<<",\n  \"raw_length\":"<<sourceText.size()
              <<",\n  \"name_hex\":\""<<hex(reinterpret_cast<const uint8*>(playerName.data()),playerName.size())
              <<"\",\n  \"source_before_hex\":\""<<hex(sourceBefore.data(),sourceBefore.size())
              <<"\",\n  \"destination_before_hex\":\""<<hex(destinationBefore.data(),destinationBefore.size())
              <<"\",\n  \"destination_after_hex\":\""<<hex(ee->m_ram+destination,256)
              <<"\",\n  \"result_hex\":\""<<hex(ee->m_ram+destination,terminator-(ee->m_ram+destination))
              <<"\",\n  \"final_source_offset\":"<<cpu.m_State.nGPR[CMIPS::S1].nV[0]-source
              <<",\n  \"final_destination_offset\":"<<cpu.m_State.nGPR[CMIPS::S2].nV[0]-destination
              <<",\n  \"source_name_code_unchanged\":true,\n  \"trace_pc_source_destination\":[";
        for(std::size_t i=0;i<trace.size();++i) {
            if(i)result<<',';result<<'['<<trace[i][0]<<','<<trace[i][1]<<','<<trace[i][2]<<']';
        }
        result<<"]\n}\n";const auto data=result.str();writeFile(output,data.data(),data.size());
    }
    void textControls(const std::vector<uint8>& text,const char* output) {
        if(text.empty() || text.size()>0x10000)throw std::runtime_error("Text fixture bounds");
        constexpr uint32 object=0xa00000,source=0xa10000;
        std::memset(ee->m_ram+object,0,0x4000);
        std::memcpy(ee->m_ram+source,text.data(),text.size());ee->m_ram[source+text.size()]=0;
        put(object+0x3bf4,source);put(object+0x22c,0x80808080);
        put(object+0xcb0,5);put(object+0xcb4,7);put(object+0x1968,3);
        put(object+0x21c,24);put(object+0xcd4,2);
        std::vector<uint8> result;
        auto word=[&](uint32 n){for(unsigned i=0;i<4;++i)result.push_back(n>>(8*i));};
        for(unsigned count=0;get(object+0x3e34)<text.size() && count<100000;++count) {
            call(0x19d9a0,{object});
            const uint32 status=ee->m_EE.m_State.nGPR[CMIPS::V0].nD0;
            for(auto offset:{0x3e34,0x1900,0x3d08,0x22c,0x230,0xcb0,0xcb4,0x1968,0x21c,0x20c,0xcd4,0xcd0,0xcd8})word(get(object+offset));
            word(status);
            for(auto offset:{0x1a70,0x1c70,0x1970,0x1b70,0x1d70}) {
                const auto* begin=ee->m_ram+object+offset;
                const auto* end=std::find(begin,begin+256,0);
                if(end==begin+256)throw std::runtime_error("Text fixture metadata bounds");
                word(end-begin);result.insert(result.end(),begin,end);
            }
            if(!status) {
                const auto cursor=get(object+0x3e34);
                put(object+0x3e34,cursor+(text.at(cursor)>=0x80 ? 2 : 1));
            }
        }
        if(get(object+0x3e34)!=text.size())throw std::runtime_error("Text fixture did not finish");
        writeFile(output,result.data(),result.size());
    }
    void messageInit(const std::vector<uint8>& ram,unsigned id,const char* output) {
        if(ram.size()!=32*1024*1024||id<10000||id>99999)
            throw std::runtime_error("Field message initialization fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                       ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        loadMicro();
        for(unsigned i=0;i<8;++i)if(get(0x200688+i*4))
            throw std::runtime_error("Message fixture already has text objects");
        const auto field=get(0x2d9bf4);
        if(!field||get(field+0x50))throw std::runtime_error("Message fixture has active field dialogue");
        call(0x11f570,{field,0x2293e0,0,id});
        const auto message=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        if(!message||message>ram.size()-0xef0||get(message+0xe40)!=2)
            throw std::runtime_error("Field message construction did not produce two layers");
        std::vector<uint8> result;
        const auto capture=[&]{
            result.insert(result.end(),ee->m_ram+message,ee->m_ram+message+0xef0);
            for(unsigned i=0;i<2;++i){
                const auto handle=get(message+0x40+i*0xe0);
                if(handle!=i)throw std::runtime_error("Field message handle order");
                const auto object=get(0x200688+handle*4);
                if(!object||object>ram.size()-0x6ec0)throw std::runtime_error("Field message object bounds");
                result.insert(result.end(),ee->m_ram+object,ee->m_ram+object+0x6ec0);
            }
        };
        capture();
        call(0x1980f8);call(0x198390,{0});capture();
        call(0x11fec8,{message});capture();
        writeFile(output,result.data(),result.size());
    }
    void shadowBatches(const std::vector<uint8>& bytes,const char* output) {
        const auto decoded=okage::read_xmd(bytes);
        if(decoded.models.size()!=1||bytes.size()>0x100000)throw std::runtime_error("Shadow batch model extent");
        constexpr unsigned source=0x900000,scratch=0x1800000;
        std::memcpy(ee->m_ram+source,bytes.data(),bytes.size());
        put(0x1fe250,0);put(0x1fe254,1);put(0x3321b4,scratch);
        std::vector<uint32> result{static_cast<uint32>(decoded.models[0].blocks.size())};
        for(const auto& block:decoded.models[0].blocks)for(unsigned pass=0;pass<2;++pass) {
            put(0x330148,pass);call(0x17ac20,{source+static_cast<uint32>(block.offset)+16});
            const auto count_slot=result.size();result.push_back(0);
            for(unsigned guard=0;;++guard) {
                if(guard>4096)throw std::runtime_error("Shadow batch loop bound");
                call(0x17acc8);if(ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0])break;
                call(0x17ac90,{1});call(0x17b9d8);
                const auto polygons=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];
                if(!polygons)break;
                const auto count=get(0x3301b0)&0xffff,end=get(0x3321c4);
                if(count>97||polygons>97||end<scratch||end>scratch+0x10000)
                    throw std::runtime_error("Original shadow batch extent");
                ++result[count_slot];result.insert(result.end(),{count,polygons});
                for(unsigned i=0;i<count;++i)result.push_back(get(0x3301b8+i*4));
                unsigned observed=0;
                for(unsigned at=scratch;at<end;) {
                    const auto header=get(at),size=header&0xffffff,op=header>>24;
                    if(size<4||size%4||size>end-at)throw std::runtime_error("Original shadow batch command extent");
                    if(op==11||op==12) {
                        const unsigned corners=op==11?3:4;
                        if(size!=8+corners*8)throw std::runtime_error("Original shadow batch polygon extent");
                        result.push_back(corners);++observed;
                        for(unsigned i=0;i<corners;++i)result.push_back(get(at+8+i*8)&0xffff);
                    } else if(op!=9&&op!=10)throw std::runtime_error("Unexpected original shadow batch command");
                    at+=size;
                }
                if(observed!=polygons)throw std::runtime_error("Original shadow polygon count");
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowBind(const std::vector<uint8>& main,const std::vector<uint8>& shadow,const char* output) {
        const auto a=okage::read_xmd(main),b=okage::read_xmd(shadow);
        if(a.models.size()!=1||b.models.size()!=1||main.size()>0x100000||shadow.size()>0x100000)
            throw std::runtime_error("Shadow binding expects two bounded models");
        const auto count=a.models[0].hierarchy.size();
        if(!count||count>4096||b.models[0].hierarchy.size()!=count)
            throw std::runtime_error("Shadow binding hierarchy extent");
        put(0x1fe250,0);put(0x1fe2ac,0x800000);put(0x330184,0);
        call(0x1caa20,{0x1400000});
        for(unsigned i=0;i<2;++i) {
            const auto& bytes=i?shadow:main;const auto& decoded=i?b:a;
            const auto base=0x900000+i*0x100000;
            std::memcpy(ee->m_ram+base,bytes.data(),bytes.size());
            put(0x1fe2b0,i);put(0x33014c,base+decoded.models[0].offset);
            call(0x1829c0);call(0x1827e0);call(0x182408);
        }
        call(0x1b6b40,{1,0});
        if(ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0]||get(0x800034)!=get(0x800000))
            throw std::runtime_error("Original shadow binding failed");
        const auto bones=get(0x800034),hierarchy=get(0x800038);
        std::vector<uint32> result{static_cast<uint32>(count)};
        for(unsigned i=0;i<count;++i) {
            result.push_back(get(hierarchy+8+i*8)&0xffff);
            uint16 flag=0;std::memcpy(ee->m_ram+hierarchy+14+i*8,&flag,2);
        }
        if(!output)return;
        call(0x1b1d88);loadMicro();call(0x184a20,{hierarchy,bones+16,0x1800000,0});
        for(unsigned i=0;i<count*16;++i)result.push_back(get(0x1800000+i*4));
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowVisibility(const std::vector<uint8>& input,const char* output) {
        std::size_t at=0;
        auto word=[&]() {if(at+4>input.size())throw std::runtime_error("Truncated visibility fixture");
            uint32 value;std::memcpy(&value,input.data()+at,4);at+=4;return value;};
        constexpr unsigned table=0x800000,poses=0x900000,bounds=0xa00000,pointers=0xb00000,selected=0xc00000;
        ee->m_EE.ToggleBreakpoint(0x185a98);ee->m_EE.ToggleBreakpoint(0x185c1c);
        const unsigned settingsAddresses[]={0x1fe2ec,0x1fe2e4,0x1fe2e8,0x1fe2b4,0x1fe2b8,
            0x1fe2d4,0x1fe2d8,0x1fe2c4,0x1fe2c8,0x1fe2cc,0x1fe2d0,0x1fe2dc,0x1fe2e0,0x1fe2bc,0x1fe2c0};
        std::vector<uint32> result;
        while(at<input.size()) {
            const auto id=word(),culling=word(),count=word(),nodes=word();
            if(culling>1||!count||count>256||!nodes||nodes>4096)throw std::runtime_error("Visibility fixture bounds");
            for(auto address:settingsAddresses)put(address,word());
            for(unsigned i=0;i<nodes*16;++i)put(poses+i*4,word());
            unsigned cursor=bounds;
            for(unsigned block=0;block<count;++block) {
                const auto n=word();if(!n||n>4096||cursor+16+n*32>pointers)throw std::runtime_error("Visibility bound capacity");
                put(pointers+block*4,cursor);put(cursor,n);
                for(unsigned i=0;i<n;++i)for(unsigned j=0;j<6;++j) {
                    const auto value=word();if(j==5&&value>=nodes)throw std::runtime_error("Visibility hierarchy index");
                    put(cursor+16+i*32+j*4,value);
                }
                cursor+=16+n*32;
            }
            put(table+12,pointers);put(table+28,0);put(0x1fe2fc,culling);put(0x1ff0040,count);
            auto& cpu=ee->m_EE;
            cpu.m_State.nGPR[CMIPS::S2].nD0=selected;cpu.m_State.nGPR[CMIPS::S3].nD0=0;
            cpu.m_State.nGPR[CMIPS::S4].nD0=table;cpu.m_State.nGPR[CMIPS::S5].nD0=0;
            cpu.m_State.nGPR[CMIPS::FP].nD0=poses;cpu.m_State.nGPR[CMIPS::T6].nD0=0;
            expectedStop=0x185a98;call(0x1857bc);
            const auto size=cpu.m_State.nGPR[CMIPS::S5].nV[0];
            if(size>count||cpu.m_State.nGPR[CMIPS::S2].nV[0]!=selected)throw std::runtime_error("Visibility result extent");
            if(size) {
                cpu.m_State.nGPR[CMIPS::T0].nD0=1;expectedStop=0x185c1c;call(0x185ab0);
            }
            result.insert(result.end(),{id,size});
            for(unsigned i=0;i<size;++i)result.insert(result.end(),{get(selected+i*16),get(selected+i*16+4)});
        }
        expectedStop=stop;ee->m_EE.ToggleBreakpoint(0x185a98);ee->m_EE.ToggleBreakpoint(0x185c1c);
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowBounds(const std::vector<uint8>& main,const std::vector<uint8>& shadow,const char* output) {
        shadowBind(main,shadow,nullptr);
        const auto decoded=okage::read_xmd(shadow);const auto hierarchy=get(0x800038);
        constexpr unsigned scratch=0x1800000;
        std::vector<uint32> result{static_cast<uint32>(decoded.models[0].blocks.size())};
        for(const auto& block:decoded.models[0].blocks)for(unsigned mode=0;mode<2;++mode) {
            std::memset(ee->m_ram+scratch,0,0x40000);put(0x1fe254,mode);
            call(0x181e40,{scratch,0xa00000+static_cast<unsigned>(block.offset),hierarchy});
            const auto count=get(scratch);
            if(count>4096)throw std::runtime_error("Original model bound extent");
            result.push_back(count);
            for(unsigned i=0;i<count;++i)for(unsigned j=0;j<6;++j)result.push_back(get(scratch+16+i*32+j*4));
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowAssembled(const std::vector<uint8>& main,const std::vector<uint8>& shadow,
            const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=58*4,top=0x19c,scratch=0x1a00000;
        if(input.empty()||input.size()%stride||input.size()>stride*32)throw std::runtime_error("Shadow assembly fixture extent");
        shadowBind(main,shadow,nullptr);const auto decoded=okage::read_xmd(shadow);
        const auto& model=decoded.models[0];const auto bones=get(0x800034),hierarchy=get(0x800038);
        call(0x1b1d88);loadMicro();
        researchGs=std::make_unique<CGSH_Null>();researchGs->Initialize();researchGs->Reset();ee->m_gs=researchGs.get();
        std::memcpy(ee->m_microMem1,ee->m_ram+0x219700,0x1358);
        auto run=[&](unsigned entry,unsigned count=0) {
            put(0x1900000,0x03000000|top);put(0x1900004,0x02000000);
            put(0x1900008,0x04000000|count);put(0x190000c,0x14000000|entry);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||ee->m_VU1.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Assembled shadow VU entry did not complete");
        };
        auto header=[&](unsigned count,unsigned source,unsigned dest=0) {
            const uint32 values[]={count,source,dest,0};std::memcpy(ee->m_vuMem1+top*16,values,16);
        };
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=stride) {
            std::array<uint32,58> f;std::memcpy(f.data(),input.data()+at,stride);
            if(f[1]<4||f[1]>7)throw std::runtime_error("Assembled shadow projection mode");
            std::vector<uint8> projectionInput(108);std::memcpy(projectionInput.data(),f.data(),24*4);
            std::memcpy(projectionInput.data()+24*4,f.data()+55,12);
            const auto packet=shadowProjection(projectionInput,nullptr);
            std::array<uint32,4> direction;for(unsigned i=0;i<4;++i)direction[i]=get(0x81000+i*4);
            unsigned cursor=3+4+10;
            if(packet.at(cursor)==0x6c018000) {
                for(unsigned i=0;i<4;++i)direction[i]=packet.at(cursor+1+i);
                if(packet.at(cursor+5)!=0x1400000a)throw std::runtime_error("Assembled direction packet");cursor+=6;
            }
            std::array<std::array<uint32,16>,2> projections;
            for(unsigned i=0;i<2;++i) {
                if(packet.at(cursor)!=0x6c048000||packet.at(cursor+17)!=0x14000006+i*2)
                    throw std::runtime_error("Assembled projection packet");
                std::copy_n(packet.begin()+cursor+1,16,projections[i].begin());cursor+=18;
            }
            for(unsigned i=0;i<8;++i)ee->m_EE.m_State.nCOP1[12+i]=f[40+i];
            for(unsigned i=0;i<7;++i)put(0x1ff0000+i*8,f[48+i]);call(0x1acc10,{0x82000});
            std::memcpy(ee->m_ram+0x3322a0,f.data()+8,64);std::memcpy(ee->m_ram+0x83000,f.data()+24,64);
            put(0x1fe2f4,0);call(0x1caa20,{0x1700000});call(0x184bd8,{hierarchy,bones+16,0x83000});
            const auto poses=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];
            if(poses!=0x1700000)throw std::runtime_error("Original composed pose arena");
            const auto countSlot=result.size()+2;
            result.insert(result.end(),{f[0],static_cast<uint32>(model.hierarchy.size()),0});
            for(unsigned i=0;i<model.hierarchy.size()*16;++i)result.push_back(get(poses+i*4));
            put(0x1fe254,1);put(0x3321b4,scratch);put(0x330148,0);
            for(unsigned blockIndex=0;blockIndex<model.blocks.size();++blockIndex) {
                const auto& block=model.blocks[blockIndex];call(0x17ac20,{0xa00000+static_cast<uint32>(block.offset)+16});
                for(unsigned batch=0;;++batch) {
                    if(batch>4096)throw std::runtime_error("Assembled batch loop bound");
                    call(0x17acc8);if(ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0])break;
                    call(0x17ac90,{1});call(0x17b9d8);if(!ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0])break;
                    const unsigned count=get(0x3301b0)&0xffff,end=get(0x3321c4);
                    if(!count||count>97||end<scratch||end>scratch+0x10000)throw std::runtime_error("Assembled batch extent");
                    std::vector<unsigned> mapping;for(unsigned i=0;i<count;++i)mapping.push_back(get(0x3301b8+i*4));
                    std::memset(ee->m_vuMem1,0,0x4000);
                    std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x202de0,9*16);run(0);
                    std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x82000,128);run(2);
                    for(unsigned i=0;i<2;++i){std::memcpy(ee->m_vuMem1+top*16,projections[i].data(),64);run(6+i*2);}
                    std::memcpy(ee->m_vuMem1+top*16,direction.data(),16);run(10);header(count,0);run(12);
                    for(const auto& range:block.attributes) {
                        if(range.kind!=5)continue;
                        std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+poses+range.hierarchy_node*64,64);run(4);
                        for(unsigned i=0;i<count;) {
                            if(mapping[i]<range.first||mapping[i]>=range.first+range.values.size()){++i;continue;}
                            unsigned n=1;
                            while(n<31&&i+n<count&&mapping[i+n]>=range.first&&mapping[i+n]<range.first+range.values.size())++n;
                            header(n,i);
                            for(unsigned j=0;j<n;++j)std::memcpy(ee->m_vuMem1+(top+1+j)*16,range.values[mapping[i+j]-range.first].data(),16);
                            run(14);i+=n;
                        }
                    }
                    header(count,0,0x61);run(18);header(count,0);run(16);
                    header(count,0);run(20);header(count,0x61);run(20);
                    unsigned triangle=0;
                    auto draw=[&](unsigned x,unsigned y,unsigned z) {
                        header(x,y,z);const auto start=ee->m_VU1.m_State.nCOP2VI[12];run(22,1);
                        const auto next=ee->m_VU1.m_State.nCOP2VI[12];uint32 first;
                        std::memcpy(&first,ee->m_vuMem1+start*16,4);
                        if((start!=0x1dc&&start!=0x2ee)||(next!=0x1dc&&next!=0x2ee))throw std::runtime_error("Assembled packet buffer");
                        const bool emitted=next!=start&&first==1;
                        if(next!=start&&first!=1&&first!=0x4b008000)throw std::runtime_error("Assembled packet header");
                        ++result[countSlot];result.insert(result.end(),{blockIndex,batch,triangle++,unsigned(emitted)});
                        if(emitted)for(unsigned offset:{2u,18u,33u})for(unsigned word=0;word<48;++word) {
                            uint32 value;std::memcpy(&value,ee->m_vuMem1+(start+offset)*16+word*4,4);result.push_back(value);
                        }
                    };
                    for(unsigned command=scratch;command<end;) {
                        const auto word=get(command),size=word&0xffffff,op=word>>24;
                        if(size<4||size%4||size>end-command)throw std::runtime_error("Assembled command extent");
                        if(op==11||op==12) {
                            if(size!=(op==11?32u:40u))throw std::runtime_error("Assembled polygon extent");
                            const auto a=get(command+8)&0xffff,b=get(command+16)&0xffff,c=get(command+24)&0xffff;
                            draw(a,b,c);if(op==12)draw(c,get(command+32)&0xffff,a);
                        } else if(op!=9&&op!=10)throw std::runtime_error("Assembled polygon opcode");
                        command+=size;
                    }
                }
            }
            if(result.size()>8*1024*1024)throw std::runtime_error("Assembled output limit");
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowMotion(const std::vector<uint8>& main,const std::vector<uint8>& shadow,
            const std::vector<uint8>& motion,const char* mainPath,const char* xtaPath,unsigned entry,unsigned frames,const char* output) {
        const auto a=okage::read_xmd(main),b=okage::read_xmd(shadow);
        if(a.models.size()!=1||b.models.size()!=1||motion.size()<32||motion.size()>0x100000||frames>180)
            throw std::runtime_error("Shadow motion fixture extent");
        shadowBind(main,shadow,nullptr);
        const auto& model=b.models[0];
        const unsigned nodes=model.hierarchy.size(),boneCount=a.models[0].bones.size();
        const auto bones=get(0x800034),hierarchy=get(0x800038);
        constexpr unsigned motionBase=0xb00000,top=0x19c;
        std::memcpy(ee->m_ram+motionBase,motion.data(),motion.size());
        const bool scale=get(motionBase+20)!=0;const unsigned channels=4+boneCount*(scale?3:2);
        if(channels>5120)throw std::runtime_error("Shadow motion channel extent");
        put(0x213014,0x810000);put(0x33760c,0x1000000);put(0x337614,0x850000);
        put(0x337610,1024);put(0x337928,0);
        call(0x1b5d10,{channels,0,0,get(motionBase+24)!=0});call(0x1b5bd8,{0,motionBase});
        for(unsigned ch=0;ch<channels;++ch)call(0x1b43c0,{ch,0x1800000});
        call(0x1b1d88);loadMicro();
        const auto textures=textureFixture(mainPath,xtaPath);
        call(0x1b5dd0,{0,entry});
        std::memcpy(ee->m_microMem1,ee->m_ram+0x219700,0x1358);
        auto run=[&](unsigned vuEntry) {
            put(0x1900000,0x03000000|top);put(0x1900004,0x02000000);
            put(0x1900008,0);put(0x190000c,0x14000000|vuEntry);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||ee->m_VU1.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Shadow skinning VU entry did not complete");
        };
        auto header=[&](unsigned count,unsigned dest) {
            const uint32 values[]={count,dest,0,0};std::memcpy(ee->m_vuMem1+top*16,values,16);
        };
        std::vector<uint32> result{nodes,static_cast<uint32>(model.blocks.size()),static_cast<uint32>(textures.packets.size())};
        for(const auto& block:model.blocks)result.push_back(block.attribute_extents[0]);
        for(unsigned frame=0;frame<=frames;++frame) {
            if(frame){call(0x1b4748,{0,channels});call(0x1b5a90);}
            result.insert(result.end(),{frame,get(0x850014)-motionBase,get(0x850000)});
            call(scale?0x1b4560:0x1b4480,{4,boneCount,bones+16});
            call(0x184a20,{hierarchy,bones+16,0x1800000,0});
            for(unsigned i=0;i<nodes*16;++i)result.push_back(get(0x1800000+i*4));
            for(const auto& block:model.blocks)for(unsigned first=0;first<block.attribute_extents[0];first+=97) {
                const unsigned count=std::min(97u,block.attribute_extents[0]-first);
                std::memset(ee->m_vuMem1,0,0x4000);
                std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x202de0,9*16);run(0);
                header(count,0);run(12);
                for(const auto& range:block.attributes) {
                    if(range.kind!=5)continue;
                    const unsigned start=std::max(first,unsigned(range.first));
                    const unsigned end=std::min(first+count,unsigned(range.first+range.values.size()));
                    if(start>=end)continue;
                    std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x1800000+range.hierarchy_node*64,64);run(4);
                    for(unsigned at=start;at<end;) {
                        const unsigned n=std::min(31u,end-at);header(n,at-first);
                        std::memcpy(ee->m_vuMem1+(top+1)*16,range.values.data()+at-range.first,n*16);
                        run(14);at+=n;
                    }
                }
                for(unsigned i=0;i<count*4;++i) {
                    uint32 v;std::memcpy(&v,ee->m_vuMem1+0x18*16+i*4,4);result.push_back(v);
                }
            }
            const auto selection=textureSelection(textures);result.insert(result.end(),selection.begin(),selection.end());
            if(result.size()>8*1024*1024)throw std::runtime_error("Shadow motion output bound");
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowCache(const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=73*4,top=0x19c;
        if(input.empty()||input.size()%stride||input.size()>stride*128)
            throw std::runtime_error("Shadow cache fixture extent");
        std::memcpy(ee->m_microMem1,ee->m_ram+0x219700,0x1358);
        auto& cpu=ee->m_VU1;
        auto run=[&](unsigned entry) {
            // Fixed research input arena; BASE/OFFSET are normal VIF commands.
            put(0x1900000,0x03000000|top);put(0x1900004,0x02000000);
            put(0x1900008,0);put(0x190000c,0x14000000|entry);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||cpu.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Shadow cache VU entry did not complete");
        };
        auto header=[&](unsigned count,unsigned source,unsigned dest) {
            const uint32 values[]={count,source,dest,0};
            std::memcpy(ee->m_vuMem1+top*16,values,16);
        };
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=stride) {
            std::array<uint32,73> f;std::memcpy(f.data(),input.data()+at,stride);
            const unsigned count=f[1];
            if(!count||count>6)throw std::runtime_error("Shadow cache fixture vertex count");
            for(unsigned i=0;i<8;++i)ee->m_EE.m_State.nCOP1[12+i]=f[2+i];
            for(unsigned i=0;i<7;++i)put(0x1ff0000+i*8,f[10+i]);
            call(0x1acc10,{0x80000});
            std::memset(ee->m_vuMem1,0,0x4000);
            std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x202de0,9*16);run(0);
            std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x80000,128);run(2);
            std::memcpy(ee->m_vuMem1+top*16,f.data()+17,64);run(6);
            std::memcpy(ee->m_vuMem1+top*16,f.data()+33,64);run(8);
            header(count,0,0);run(12);
            std::memcpy(ee->m_vuMem1+0x18*16,f.data()+49,count*16);
            header(count,0,0x61);run(18);header(count,0,0);run(16);
            header(count,0,0);run(20);header(count,0x61,0);run(20);
            const int32 status=static_cast<int16>(cpu.m_State.nCOP2VI[11]);
            result.insert(result.end(),{f[0],count,static_cast<uint32>(status)});
            for(unsigned i=0;i<32;++i)result.push_back(get(0x80000+i*4));
            for(unsigned cache=0;cache<2;++cache)for(unsigned i=0;i<count;++i) {
                for(unsigned base:{0x18u,0xdau})for(unsigned j=0;j<4;++j) {
                    uint32 value;std::memcpy(&value,ee->m_vuMem1+(base+cache*0x61+i)*16+j*4,4);
                    result.push_back(value);
                }
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void shadowPrism(const std::vector<uint8>& input,const char* output,bool raster=false) {
        const unsigned stride=(raster?58:54)*4;
        if(input.empty()||input.size()%stride||input.size()>stride*(raster?48:128))
            throw std::runtime_error("Shadow prism fixture extent");
        if(raster) {
#ifdef OKAGE_REFERENCE_VULKAN
            researchGs.reset(vulkanReference(researchTraffic)());
#else
            throw std::runtime_error("Shadow raster requires research Vulkan build");
#endif
        } else researchGs=std::make_unique<CGSH_Null>();
        researchGs->Initialize();researchGs->Reset();ee->m_gs=researchGs.get();
        std::memcpy(ee->m_microMem1,ee->m_ram+0x219700,0x1358);
        auto& cpu=ee->m_VU1;
        auto run=[&](unsigned entry,unsigned count) {
            put(0x1900000,0x04000000|count);put(0x1900004,0x14000000|entry);
            put(0x1900008,0);put(0x190000c,0);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||cpu.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Shadow prism VU entry did not complete");
        };
        std::vector<uint32> result;std::vector<uint8> pixels;
        for(unsigned at=0;at<input.size();at+=stride) {
            std::array<uint32,58> f{};std::memcpy(f.data(),input.data()+at,stride);
            if(raster) {
                if(f[55]>0xffffff||!f[57]||f[57]>8)throw std::runtime_error("Shadow raster depth/repeat range");
                auto reg=[&](uint8 address,uint64 value){researchGs->WriteRegister({address,value});};
                constexpr uint64 frame=uint64(10)<<16,zbuf=128;
                reg(GS_REG_FRAME_1,frame);reg(GS_REG_ZBUF_1,zbuf);
                reg(GS_REG_SCISSOR_1,(uint64(639)<<16)|(uint64(223)<<48));
                reg(GS_REG_XYOFFSET_1,uint64(1728*16)|(uint64(1936*16)<<32));
                reg(GS_REG_PRMODECONT,1);reg(GS_REG_COLCLAMP,1);reg(GS_REG_FBA_1,0);
                reg(GS_REG_TEST_1,0x30000);reg(GS_REG_PRIM,6);
                reg(GS_REG_RGBAQ,uint64(f[54]&0xffffff)|0x40000000|(uint64(0x3f800000)<<32));
                auto xyz=[&](unsigned x,unsigned y){return uint64(x*16)|(uint64(y*16)<<16)|(uint64(f[55])<<32);};
                reg(GS_REG_XYZ2,xyz(1728,1936));reg(GS_REG_XYZ2,xyz(1728+640,1936+224));
                reg(GS_REG_ZBUF_1,zbuf|(uint64(1)<<32));reg(GS_REG_TEST_1,0x5000c);
                reg(GS_REG_FRAME_1,frame|(uint64(0x7fffffff)<<32));reg(GS_REG_ALPHA_1,0x54);
                reg(GS_REG_RGBAQ,uint64(f[56]&0xffffff)|(uint64(0x3f800000)<<32));
                researchGs->ProcessWriteBuffer(nullptr);researchGs->FlushWriteBuffer();
                // Synthetic framebuffer allocation; original template control words unchanged.
                put(0x202e20,frame);put(0x202e24,0);put(0x202e50,frame);put(0x202e54,0x7fffffff);
            }
            std::memset(ee->m_vuMem1,0,0x4000);
            std::memcpy(ee->m_vuMem1,ee->m_ram+0x202de0,9*16);
            run(0,0);
            std::memcpy(ee->m_vuMem1+0x10*16,f.data()+2,16);
            const uint32 indices[]={0,1,2,0};std::memcpy(ee->m_vuMem1,indices,16);
            for(unsigned i=0;i<4;++i) {
                const unsigned base=std::array<unsigned,4>{0x18,0x79,0xda,0x13b}[i];
                std::memcpy(ee->m_vuMem1+base*16,f.data()+6+i*12,48);
            }
            cpu.m_State.nCOP2VI[11]=f[1];run(22,1);
            if(raster) {
#ifdef OKAGE_REFERENCE_VULKAN
                for(unsigned repeat=1;repeat<f[57];++repeat)run(22,1);
                const auto image=captureShadowPixels(researchGs.get());pixels.insert(pixels.end(),image.begin(),image.end());
#endif
                continue;
            }
            uint32 first;std::memcpy(&first,ee->m_vuMem1+0x1dc*16,4);
            if(first!=1&&first!=0x4b008000)throw std::runtime_error("Unexpected shadow prism output tag");
            const auto nextBuffer=cpu.m_State.nCOP2VI[12];
            if(nextBuffer!=0x1dc&&nextBuffer!=0x2ee)
                throw std::runtime_error("Unexpected shadow prism buffer selector");
            const unsigned qwords=nextBuffer==0x1dc?0:(first==1?46:1);
            if(!qwords){result.insert(result.end(),{f[0],0});continue;}
            uint32 end;std::memcpy(&end,ee->m_vuMem1+(0x1dc+qwords-1)*16,4);
            if(end!=0x4b008000)
                throw std::runtime_error("Shadow prism terminal tag missing");
            result.insert(result.end(),{f[0],qwords});
            for(unsigned i=0;i<qwords*4;++i) {
                uint32 word;std::memcpy(&word,ee->m_vuMem1+0x1dc*16+i*4,4);result.push_back(word);
            }
        }
        if(raster)writeFile(output,pixels.data(),pixels.size());
        else writeFile(output,result.data(),result.size()*4);
    }
    std::vector<uint32> shadowProjection(const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=27*4;
        if(input.empty()||input.size()%stride||input.size()>stride*128)
            throw std::runtime_error("Shadow projection fixture extent");
        loadMicro();std::vector<uint32> result,setup;
        for(unsigned at=0;at<input.size();at+=stride) {
            std::array<uint32,27> f;std::memcpy(f.data(),input.data()+at,stride);
            if(f[1]>7)throw std::runtime_error("Shadow projection mode");
            auto& cpu=ee->m_EE;
            std::memcpy(ee->m_ram+0x80000,f.data()+8,64);
            put(0x337e90,0x80000);call(0x1e2350,{0x337ea0,0x80000});
            for(unsigned i=0;i<3;++i)cpu.m_State.nCOP1[12+i]=f[4+i];
            call(0x1b15c0);
            cpu.m_State.nCOP1[12]=0;cpu.m_State.nCOP1[13]=0xc0800000;cpu.m_State.nCOP1[14]=0;
            call(0x1b15e8);
            cpu.m_State.nCOP1[12]=0;cpu.m_State.nCOP1[13]=0x40800000;cpu.m_State.nCOP1[14]=0;
            call(0x1b1608);call(0x1b1508);
            put(0x337e88,0x3353d0);put(0x337e8c,0x335410);
            put(0x337e60,f[4]);put(0x337e3c,f[3]);
            float slope;std::memcpy(&slope,f.data()+4,4);const float span=slope+slope;
            uint32 spanBits;std::memcpy(&spanBits,&span,4);put(0x337e38,spanBits);
            for(unsigned i=0;i<3;++i)put(0x337e78+4*i,f[24+i]);
            // Same direction transform/normalization as0x1b1710, without
            // invoking unrelated GPU/queue setup. All math executes unchanged.
            call(0x1e2210,{0x81000,0x80000,0x202ea0});call(0x1e22d0,{0x81000,0x81000});
            setup.push_back(f[0]);
            for(auto pair:{std::pair<uint32,unsigned>{0x337ea0,16},{0x3353d0,32},{0x202ea0,4},{0x81000,4}})
                for(unsigned i=0;i<pair.second;++i)setup.push_back(get(pair.first+4*i));
            constexpr uint32 arena=0x1800000;
            std::memset(ee->m_ram+arena,0xcd,4096);
            put(0x3a0aa0,arena);put(0x3a0aa4,0);put(0x2138b4,arena+16);put(0x335328,0);
            cpu.m_State.nCOP1[12]=f[2];call(0x1b8dc8,{f[1]});
            const auto end=get(0x3a0aa0);
            if(end<arena+16||end>arena+4080||(end&15))throw std::runtime_error("Shadow packet arena extent");
            result.insert(result.end(),{f[0],f[1],end-arena});
            for(uint32 p=arena;p<end;p+=4)result.push_back(get(p));
        }
        if(output) {
            writeFile(output,result.data(),result.size()*4);
            writeFile((std::string(output)+".setup.bin").c_str(),setup.data(),setup.size()*4);
        }
        return result;
    }
    void shadowUpdate(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=30*4;
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%stride||input.size()>stride*32)
            throw std::runtime_error("Shadow update fixture extent");
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=stride) {
            ee->m_EE.m_executor->Reset();
            std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);loadMicro();
            std::array<uint32,30> f;std::memcpy(f.data(),input.data()+at,stride);
            if(f[1]>5||f[6]>65535||f[7]>1||f[9]>128)throw std::runtime_error("Shadow update fixture bounds");
            const auto field=get(0x2d9bf4),player=get(field+0x44),objects=get(0x21301c),hero=get(0x337b64),stun=get(player+0x25c);
            if(player>ram.size()-0x284||hero>=640||stun>=640||objects>ram.size()-640*0x3c)
                throw std::runtime_error("Shadow update actor bounds");
            const auto heroFirst=get(objects+hero*0x3c+4)&65535,stunFirst=get(objects+stun*0x3c+4)&65535;
            if(heroFirst>=5118||stunFirst>=5118)throw std::runtime_error("Shadow channel bounds");
            for(unsigned i=0;i<5;++i)put(player+0x260+i*4,f[1+i]);
            put(0x337e3c,f[8]);call(0x1baae8,{hero,f[6]});call(0x1baa60,{stun,f[7]});
            const auto setChannel=[&](unsigned channel,const uint32* words) {
                std::memcpy(ee->m_ram+0x80000,words,16);call(0x1b4220,{channel,0x80000});
            };
            setChannel(heroFirst,f.data()+10);setChannel(heroFirst+1,f.data()+14);
            for(unsigned i=0;i<3;++i)setChannel(stunFirst+i,f.data()+18+i*4);
            const auto capture=[&](unsigned step) {
                result.insert(result.end(),{f[0],step});
                for(unsigned i=0;i<5;++i)result.push_back(get(player+0x260+i*4));
                result.insert(result.end(),{get(objects+hero*0x3c+0x2c)&65535,
                    unsigned(bool(get(objects+stun*0x3c)&0x800)),get(0x337e3c)});
                for(unsigned i=0;i<3;++i) {
                    call(0x1b43c0,{stunFirst+i,0x80000});
                    for(unsigned k=0;k<4;++k)result.push_back(get(0x80000+k*4));
                }
            };
            capture(0);
            for(unsigned step=1;step<=f[9];++step){call(0x135100,{player});capture(step);}
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void effectTint(const std::vector<uint8>& input,const char* output) {
        if(input.empty()||input.size()%48||input.size()>48*64)throw std::runtime_error("Effect tint fixture extent");
        loadMicro();std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=48) {
            std::memcpy(ee->m_ram+0x80000,input.data()+at,48);
            call(0x183d68,{0x80000});call(0x183d30,{0x80010});call(0x183ce0,{0,0x80020});
            call(0x1020e0);
            for(unsigned i=0;i<4;++i)result.push_back(get(0x24e620+i*4));
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void effectTemplateLoad(const std::vector<uint8>& ram,const std::vector<uint8>& efd,uint32 seed,const char* output,bool geometry=false) {
        if(ram.size()!=32*1024*1024||efd.size()<16||efd.size()>0x70000)
            throw std::runtime_error("Effect template load fixture extent");
        uint32 magic,count;std::memcpy(&magic,efd.data(),4);std::memcpy(&count,efd.data()+4,4);
        if(magic!=0x20444645||count>128)throw std::runtime_error("Effect template EFD header");
        ee->m_EE.m_executor->Reset();std::memcpy(ee->m_ram,ram.data(),ram.size());
        put(stop,0x08000000|(stop>>2));put(stop+4,0);loadMicro();
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        const auto table=get(0x1f849c),random=get(0x2151b4)+0x58;
        if(table>ram.size()-212*4||random>ram.size()-4)throw std::runtime_error("Template load pointers");
        for(unsigned i=0;i<212;++i)put(table+i*4,0xffffffff);
        for(unsigned i=0;i<66;++i){put(0x24e790+i*4,0);put(0x24e898+i*4,0);}
        std::memcpy(ee->m_ram+0x80000,efd.data(),efd.size());put(random,seed);
        templateRandomLog.clear();templateLoadProbe=true;ee->m_EE.ToggleBreakpoint(0x1f14b0);
        call(0x10e098,{8});const auto grid=get(0x24e9fc),gridRng=get(random);
        if(grid>ram.size()-384*4)throw std::runtime_error("Template grid extent");
        std::vector<uint32> noise;for(unsigned i=0;i<384;++i)noise.push_back(get(grid+i*4));
        call(0x10ba38,{0x80000});
        if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=count)throw std::runtime_error("Template importer count");
        ee->m_EE.ToggleBreakpoint(0x1f14b0);templateLoadProbe=false;
        std::vector<uint32> result{0x504d5445,seed,gridRng,get(random),count,unsigned(templateRandomLog.size()/2)};
        result.insert(result.end(),noise.begin(),noise.end());
        result.insert(result.end(),templateRandomLog.begin(),templateRandomLog.end());
        for(unsigned shape=0;shape<66;++shape){result.push_back(get(0x24e790+shape*4));result.push_back(get(0x24e898+shape*4));}
        for(unsigned id=0;id<128;++id) {
            const auto job=get(table+(84+id)*4);
            if(job==0xffffffff){result.insert(result.end(),{id,0xffffffff,0,0});continue;}
            if(job>ram.size()-128)throw std::runtime_error("Imported template pointer");
            result.insert(result.end(),{id,get(job),get(job+4),get(job+8)});
        }
        writeFile(output,result.data(),result.size()*4);
        if(geometry) {
            std::vector<uint32> data;
            for(unsigned shape:{43u,45u,55u,56u,62u}) {
                const auto owner=get(0x24e790+shape*4),id=get(0x24e898+shape*4);
                if(!owner||owner>=128)throw std::runtime_error("Prepared geometry cache owner");
                const auto job=get(table+(84+owner)*4);
                if(job>ram.size()-128)throw std::runtime_error("Prepared geometry job");
                const auto count=get(job+8),elements=get(job+12);
                if(count>4096||elements>ram.size()-count*128)throw std::runtime_error("Prepared geometry elements");
                uint32 element=0;
                for(unsigned i=0;i<count;++i)if(get(elements+i*128+0x50)==id){element=elements+i*128;break;}
                if(!element)throw std::runtime_error("Prepared geometry cache element");
                std::vector<uint32> pieces;
                for(auto g=get(element+0x64);g;g=get(g)) {
                    if(g>ram.size()-48||pieces.size()>=32)throw std::runtime_error("Prepared geometry piece extent");
                    pieces.push_back(g);
                }
                data.insert(data.end(),{shape,owner,id,unsigned(pieces.size())});
                for(auto g:pieces) {
                    for(unsigned kind=0;kind<2;++kind) {
                        const auto n=(get(g+0x1c)>>(kind*16))&65535,p=get(g+0x14+kind*4);
                        if(n>512||(n&&p>ram.size()-n))throw std::runtime_error("Prepared geometry tags");
                        data.push_back(n);for(unsigned i=0;i<n;++i)data.push_back(ee->m_ram[p+i]);
                    }
                    for(unsigned offset:{4u,8u,12u,16u}) {
                        if(offset==8&&(get(g+0x1c)>>16)==0){data.push_back(0);continue;}
                        const auto p=get(g+offset);if(!p){data.push_back(0);continue;}
                        if(p>ram.size()-4)throw std::runtime_error("Prepared geometry packet pointer");
                        const auto n=(get(p)&65535)*16+32;
                        if(n>8192||p>ram.size()-n)throw std::runtime_error("Prepared geometry packet extent");
                        data.push_back(n);for(unsigned i=0;i<n;i+=4)data.push_back(get(p+i));
                    }
                }
            }
            writeFile((std::string(output)+".geometry.bin").c_str(),data.data(),data.size()*4);
        }
        std::cout<<"Imported "<<count<<" EFD templates with "<<templateRandomLog.size()/2<<" observed RNG calls including noise grid\n";
    }
    void effectNoise(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%8||input.size()>8*16)
            throw std::runtime_error("Effect noise fixture extent");
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=8) {
            uint32 f[2];std::memcpy(f,input.data()+at,8);
            ee->m_EE.m_executor->Reset();std::memcpy(ee->m_ram,ram.data(),ram.size());
            put(stop,0x08000000|(stop>>2));put(stop+4,0);loadMicro();
            effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
            const auto table=get(0x1f849c),random=get(0x2151b4)+0x58;
            if(table>ram.size()-212*4||random>ram.size()-4)throw std::runtime_error("Effect noise fixture pointers");
            for(unsigned i=0;i<84;++i)put(table+i*4,0xffffffff);
            put(random,f[1]);call(0x10e098,{8});
            const auto grid=get(0x24e9fc);
            if(grid>ram.size()-384*4)throw std::runtime_error("Effect noise grid extent");
            result.insert(result.end(),{f[0],f[1],get(random),384});
            for(unsigned i=0;i<384;++i)result.push_back(get(grid+i*4));
            for(unsigned shape=10;shape<=16;++shape) {
                put(0x24e790+shape*4,0);std::memset(ee->m_ram+0x80000,0,48+116);
                put(0x80000,124);put(0x80004,80);put(0x80010,1);put(0x80014,0x80030);
                put(0x80034,1);put(0x80038,shape);put(0x8003c,0x21);put(0x80048,0xffffffff);
                call(0x10b7a0,{0x80000});
                if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=0)throw std::runtime_error("Effect noise construction failed");
                const auto job=get(table+(84+124)*4);
                if(job>ram.size()-128||get(job)!=124||get(job+8)!=1)throw std::runtime_error("Effect noise template extent");
                const auto element=get(job+12);if(element>ram.size()-128)throw std::runtime_error("Effect noise element extent");
                std::vector<uint32> pieces;
                for(auto g=get(element+0x64);g;g=get(g)) {
                    if(g>ram.size()-48||pieces.size()>=32)throw std::runtime_error("Effect noise piece extent");
                    pieces.push_back(g);
                }
                result.insert(result.end(),{shape,get(random),unsigned(pieces.size())});
                for(auto g:pieces)for(unsigned offset:{4u,8u,12u,16u}) {
                    if(offset==8&&(get(g+0x1c)>>16)==0){result.push_back(0);continue;}
                    const auto p=get(g+offset);if(!p){result.push_back(0);continue;}
                    if(p>ram.size()-4)throw std::runtime_error("Effect noise packet pointer");
                    const auto n=(get(p)&65535)*16+32;
                    if(n>8192||p>ram.size()-n)throw std::runtime_error("Effect noise packet extent");
                    result.push_back(n);for(unsigned i=0;i<n;i+=4)result.push_back(get(p+i));
                }
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void effectShape(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output,bool procedural=false) {
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%12||input.size()>12*32)
            throw std::runtime_error("Effect shape fixture extent");
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=12) {
            uint32 f[3];std::memcpy(f,input.data()+at,12);
            if(procedural?(f[1]!=43&&f[1]!=45&&f[1]!=55&&f[1]!=56&&f[1]!=62):(f[1]!=3&&f[1]!=55))
                throw std::runtime_error("Effect shape scope");
            ee->m_EE.m_executor->Reset();std::memcpy(ee->m_ram,ram.data(),ram.size());
            put(stop,0x08000000|(stop>>2));put(stop+4,0);loadMicro();
            effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
            const auto table=get(0x1f849c),random=get(0x2151b4)+0x58;
            if(table>ram.size()-212*4||random>ram.size()-4)throw std::runtime_error("Effect shape fixture pointers");
            for(unsigned i=0;i<84;++i)put(table+i*4,0xffffffff);
            put(0x24e790+f[1]*4,0);put(random,f[2]);
            std::memset(ee->m_ram+0x80000,0,48+116);
            put(0x80000,124);put(0x80004,80);put(0x80010,1);put(0x80014,0x80030);
            put(0x80034,1);put(0x80038,f[1]);put(0x8003c,0x21);put(0x80048,0xffffffff);
            if(procedural&&(f[1]==56||f[1]==62))put(0x80048,f[1]==56?30:10);
            call(0x10b7a0,{0x80000});
            if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=0)throw std::runtime_error("Effect shape construction failed");
            const auto job=get(table+(84+124)*4);
            if(job>ram.size()-128||get(job)!=124||get(job+8)!=1)throw std::runtime_error("Effect shape template extent");
            const auto element=get(job+12);if(element>ram.size()-128)throw std::runtime_error("Effect shape element extent");
            std::vector<uint32> pieces;
            for(auto g=get(element+0x64);g;g=get(g)) {
                if(g>ram.size()-48||pieces.size()>=32)throw std::runtime_error("Effect shape piece extent");
                pieces.push_back(g);
            }
            result.insert(result.end(),{f[0],f[1],f[2],get(random),unsigned(pieces.size())});
            for(auto g:pieces) {
                if(procedural)for(unsigned kind=0;kind<2;++kind) {
                    const auto n=(get(g+0x1c)>>(kind*16))&65535,p=get(g+0x14+kind*4);
                    if(n>512||(n&&p>ram.size()-n))throw std::runtime_error("Procedural tag extent");
                    result.push_back(n);for(unsigned i=0;i<n;++i)result.push_back(ee->m_ram[p+i]);
                }
                for(unsigned offset:{4u,8u,12u,16u}) {
                if(offset==8&&(get(g+0x1c)>>16)==0){result.push_back(0);continue;}
                const auto p=get(g+offset);if(!p){result.push_back(0);continue;}
                if(p>ram.size()-4)throw std::runtime_error("Effect shape packet pointer case="+std::to_string(f[0])+" geometry="+std::to_string(g)+" offset="+std::to_string(offset)+" pointer="+std::to_string(p));
                const auto n=(get(p)&65535)*16+32;
                if(n>8192||p>ram.size()-n)throw std::runtime_error("Effect shape packet extent");
                result.push_back(n);for(unsigned i=0;i<n;i+=4)result.push_back(get(p+i));
                }
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void mapEffectGeometry(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output,unsigned templateId=25,const std::vector<uint8>& efd={}) {
        const bool church=templateId==14,shadow=templateId==24,dark=templateId==6,summon=templateId==33||templateId==115,deferred=church||dark||templateId==16||summon;
        const bool torch=templateId==91,flame=templateId==16||torch,cylinder=templateId==26;
        const unsigned count=church?7:dark?4:cylinder?2:templateId==33?9:templateId==115?5:templateId==16?3:torch?4:8;
        const unsigned stride=(38+count*20+(flame?36:shadow?90:templateId==33?30:cylinder?34:0))*4,sp=0x1ff0000,slot=church||shadow||dark||flame||summon||cylinder?0:2;
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%stride||input.size()>stride*64)
            throw std::runtime_error("Effect geometry fixture bounds");
        std::vector<uint32> result,packets;
        for(unsigned at=0;at<input.size();at+=stride) {
            ee->m_EE.m_executor->Reset();
            std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
            loadMicro();const auto table=get(0x1f849c);
            if(shadow||deferred||cylinder) {
                effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                    ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
                for(unsigned i=0;i<84;++i)put(table+4*i,0xffffffff);
                put(get(0x2151b4)+0x58,1);
                std::memcpy(ee->m_ram+0x81000,input.data()+at+4,48);
                if(shadow) {
                    const auto word=[&](unsigned p){if(p>efd.size()||efd.size()-p<4)throw std::runtime_error("Geometry EFD bounds");uint32 x;std::memcpy(&x,efd.data()+p,4);return x;};
                    if(word(0)!=0x20444645||word(4)>128)throw std::runtime_error("Geometry EFD header");
                    bool found=false;
                    for(unsigned row=0,p=16;row<word(4);++row) {
                        const auto n=word(p+16);if(n>128)throw std::runtime_error("Geometry EFD element count");
                        const auto bytes=48+n*116;if(p>efd.size()||efd.size()-p<bytes)throw std::runtime_error("Geometry EFD record extent");
                        if(word(p)==24) {
                            if(n!=8)throw std::runtime_error("Shadow geometry EFD scope");
                            std::memcpy(ee->m_ram+0x82000,efd.data()+p,bytes);put(0x82014,0x82030);
                            for(unsigned shape:{3u,25u,31u,55u})put(0x24e790+shape*4,0);
                            call(0x10b7a0,{0x82000});found=true;break;
                        }
                        p+=bytes;
                    }
                    if(!found)throw std::runtime_error("Missing shadow geometry template");
                }
                if(church||shadow||dark||summon||cylinder)call(0x1012f8,{templateId,0x81000});
                else call(0x101440,{templateId,0x81000,0x81010,0x81020});
                if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=slot)
                    throw std::runtime_error("Deferred geometry allocation order");
            }
            const auto job=get(table+slot*4);
            if(get(job)!=templateId||get(job+8)!=count)throw std::runtime_error("Unexpected captured map effect");
            std::memcpy(ee->m_ram+job+0x10,input.data()+at+4,64);
            call(0x1842c8);const auto camera=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            std::memcpy(ee->m_ram+camera,input.data()+at+68,16);
            std::memcpy(ee->m_ram+0x80000,input.data()+at+84,64);
            std::memcpy(ee->m_ram+0x1f8484,input.data()+at+148,4);
            put(0x1f8490,get(job+0x78));put(sp+0xe0,0x80000);put(sp+0xf4,sp+0xa0);
            auto& cpu=ee->m_EE;
            cpu.m_State.nGPR[CMIPS::S3].nD0=slot*4;cpu.m_State.nGPR[CMIPS::S6].nD0=slot;
            cpu.m_State.nGPR[CMIPS::FP].nD0=0x202ee0;
            auto block=[&](uint32 begin,uint32 end) {
                expectedStop=end;cpu.ToggleBreakpoint(end);call(begin);
                cpu.ToggleBreakpoint(end);expectedStop=stop;
            };
            if(deferred) {
                cpu.m_State.nGPR[CMIPS::S4].nD0=slot*4;
                cpu.m_State.nGPR[CMIPS::S7].nD0=0x200000;
                put(sp+0xc0,slot);put(sp+0xd0,0x80000);
                block(0x102e58,0x10311c);
                cpu.m_State.nGPR[CMIPS::S5].nD0=sp+0x90;
                cpu.m_State.nGPR[CMIPS::FP].nD0=sp+0x10;
            } else block(0x1024e0,0x1027b8);
            // Caller scratch destinations from0x1027b8..0x1027c0.
            if(!deferred){put(sp+0xf8,sp+0x10);cpu.m_State.nGPR[CMIPS::S7].nD0=sp+0x90;}
            uint32 identity;std::memcpy(&identity,input.data()+at,4);result.push_back(identity);
            for(unsigned i=0;i<count;++i) {
                const auto e=get(job+12)+i*128,r=get(e+0x78),g=get(e+0x64);
                const auto source=input.data()+at+152+i*80;
                std::array<uint32,4> meta;std::memcpy(meta.data(),source+64,16);
                unsigned texture=0;for(;texture<31;++texture)if(get(get(0x24e780)+texture*4)==(get(e+0x5c)&65535))break;
                if((church||shadow||dark||summon||(torch&&i==3))&&(get(e+0x5c)&65535)==65535)texture=0xffffffff;
                const unsigned shape=church?(i==0?25:i==1?40:35):shadow?(i<3?25:i==3?3:i==4?55:31):dark?(i%2?9:7):cylinder?24:summon?(templateId==115?(i?31:32):i==0?40:i==1?25:i==2?59:29):flame?(i<3?28:40):(i<3?24:29);
                if(meta[0]!=shape||meta[1]!=get(e+0x54)||meta[2]!=get(e+0x58)||meta[3]!=texture)
                    throw std::runtime_error("Effect geometry input metadata mismatch");
                std::memcpy(ee->m_ram+r,source,64);
                if(flame&&i<3) {
                    const auto xyz=get(g+4)+48,uv=get(g+8)+48;
                    if(xyz>ram.size()-24||uv>ram.size()-16)throw std::runtime_error("Torch geometry extent");
                    for(unsigned v=0;v<4;++v) {
                        const auto packed=input.data()+at+152+count*80+i*48+v*12;
                        std::memcpy(ee->m_ram+xyz+v*6,packed,6);
                        std::memcpy(ee->m_ram+uv+v*4,packed+8,4);
                    }
                }
                if(cylinder) {
                    if(get(g)||(get(g+0x1c)>>16)!=34)throw std::runtime_error("Cylinder UV extent");
                    call(0x113720,{g,0});const auto uv=uint32(cpu.m_State.nGPR[CMIPS::V0].nD0);
                    if(uv>ram.size()-34*4)throw std::runtime_error("Cylinder UV address");
                    for(unsigned v=0;v<17;++v) {
                        uint32 t;std::memcpy(&t,input.data()+at+152+count*80+(i*17+v)*4,4);
                        if(t>65535)throw std::runtime_error("Cylinder UV fixture extent");
                        const uint16 packed=uint16(t);std::memcpy(ee->m_ram+uv+(v*2+1)*4+2,&packed,2);
                    }
                }
                if((templateId==33&&i==1)||(shadow&&i<3)) {
                    if(get(g)||(get(g+0x1c)&65535)!=60)throw std::runtime_error("Summoning ring extent");
                    for(unsigned v=0;v<30;++v) {
                        uint32 y;std::memcpy(&y,input.data()+at+152+count*80+(v+(shadow?i*30:0))*4,4);
                        if(y>65535)throw std::runtime_error("Summoning ring signed16 fixture extent");
                        call(0x113690,{g,v*2+1});const auto point=uint32(cpu.m_State.nGPR[CMIPS::V0].nD0);
                        if(point>ram.size()-6)throw std::runtime_error("Summoning ring position extent");
                        const uint16 packed=uint16(y);std::memcpy(ee->m_ram+point+2,&packed,2);
                    }
                }
                cpu.m_State.nGPR[CMIPS::S1].nD0=e;cpu.m_State.nGPR[CMIPS::S2].nD0=r;
                cpu.m_State.nGPR[deferred?CMIPS::S3:CMIPS::S4].nD0=i;
                block(deferred?0x1032cc:0x102978,0x1aeb10);
                block(deferred?0x103370:0x102a20,0x1ad3c8);
                for(unsigned k=0;k<16;++k)result.push_back(get(sp+0x10+k*4));
                for(unsigned k=0;k<4;++k)result.push_back(get(sp+0x90+k*4));
                if(at==0) {
                    packets.insert(packets.end(),{i,meta[0],get(e+0x68),get(e+0x6c)});
                    for(unsigned k=0;k<6;++k)packets.push_back(get(r+0x40+k*4));
                    std::vector<uint32> pieces;
                    for(auto piece=g;piece;piece=get(piece)) {
                        if(piece>ram.size()-48||pieces.size()>=32)throw std::runtime_error("Effect packet chain extent");
                        pieces.push_back(piece);
                    }
                    if(church||shadow||dark||summon)packets.push_back(unsigned(pieces.size()));
                    else if(pieces.size()!=1)throw std::runtime_error("Unexpected linked effect geometry");
                    for(const auto piece:pieces)for(unsigned offset:{4u,8u,12u,16u}) {
                        if(offset==8&&(get(piece+0x1c)>>16)==0){packets.push_back(0);continue;}
                        const auto p=get(piece+offset),bytes=p?(get(p)&65535)*16+32:0;
                        if(bytes>8192||p>ram.size()-bytes)throw std::runtime_error("Effect packet extent");
                        packets.push_back(bytes);
                        for(unsigned k=0;k<bytes;k+=4)packets.push_back(get(p+k));
                    }
                }
            }
        }
        writeFile(output,result.data(),result.size()*4);
        writeFile((std::string(output)+".packets.bin").c_str(),packets.data(),packets.size()*4);
    }
    void effectReferenceProbe(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%8||input.size()>128*8)
            throw std::runtime_error("Effect reference fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());const auto table=get(0x1f849c);
        if(table>ram.size()-212*4)throw std::runtime_error("Effect template table extent");
        std::vector<uint32> out;
        for(unsigned at=0;at<input.size();at+=8) {
            std::array<uint32,2> ids;std::memcpy(ids.data(),input.data()+at,8);
            if(ids[0]>=128||ids[1]>=128)throw std::runtime_error("Effect reference ID extent");
            const auto job=get(table+(84+ids[0])*4),source=get(table+(84+ids[1])*4);
            if(job>ram.size()-128||source>ram.size()-128||get(job+12)!=get(source+12)||get(job+8)!=get(source+8))
                throw std::runtime_error("Original reference does not share source elements");
            out.insert(out.end(),{ids[0],get(job),get(job+4),get(job+96),get(job+8)});
            for(unsigned i=0;i<20;++i)out.push_back(get(job+16+i*4));
        }
        writeFile(output,out.data(),out.size()*4);
    }
    void mapEffectProbe(const std::vector<uint8>& ram,const std::vector<uint8>& input,unsigned frames,const char* output) {
        if(ram.size()!=32*1024*1024||(input.size()!=80&&input.size()!=96)||frames>12000)
            throw std::runtime_error("Map effect probe fixture bounds");
        std::array<uint32,20> request;std::memcpy(request.data(),input.data(),80);
        if((request[0]!=6&&request[0]!=14&&request[0]!=16&&request[0]!=17&&request[0]!=19&&request[0]!=24&&request[0]!=25&&request[0]!=26&&request[0]!=33&&request[0]!=34&&request[0]!=90&&request[0]!=91&&request[0]!=98&&request[0]!=105&&request[0]!=106&&request[0]!=115)||request[2]>0xfff||request[3]!=0x100)
            throw std::runtime_error("Map effect probe request scope");
        const bool torch=request[0]==91,flame=torch||request[0]==16,cameraGlow=torch||request[0]==90;
        if((cameraGlow&&input.size()!=96)||(!cameraGlow&&input.size()!=80)||(!flame&&frames>1000))
            throw std::runtime_error("Map effect camera/frame scope");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                        ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        loadMicro();
        if(cameraGlow) {
            call(0x1842d8);const auto camera=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            if(camera>ram.size()-16)throw std::runtime_error("Torch camera pointer extent");
            std::array<float,4> eye;std::memcpy(eye.data(),input.data()+80,16);
            for(unsigned i=0;i<4;++i){if(!std::isfinite(eye[i]))throw std::runtime_error("Torch camera finite input");
                const float inverse=-eye[i];uint32 word;std::memcpy(&word,&inverse,4);put(camera+4*i,word);}
        }
        const auto table=get(0x1f849c),random=get(0x2151b4)+0x58;
        if(table>ram.size()-212*4||random>ram.size()-4)throw std::runtime_error("Map effect fixture pointers");
        for(unsigned i=0;i<84;++i)put(table+4*i,0xffffffff);
        put(random,request[1]);std::memcpy(ee->m_ram+0x80000,request.data()+4,64);
        const bool attached=request[0]==14||request[0]==24;
        const bool positioned=attached||request[0]==6||request[0]==17||request[0]==26||request[0]==33||request[0]==115;
        uint32 handle;
        if(attached) {
            const auto actor=get(0x337b64);
            call(0x1bb5b8,{actor});const auto channel=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            call(0x1b4220,{channel,0x80010});call(0x1b4220,{channel+1,0x80000});
            if(request[0]==14) {
                std::memset(ee->m_ram+0x2d9bf8,0xff,32);put(0x1fb948,15);
                call(0x130be0,{0x39,14,actor,0x80000});
                if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=0x80000||get(0x1fb948)!=0)
                    throw std::runtime_error("Attached effect service payload/ring extent");
                for(unsigned i=0;i<15;++i)if((get(0x2d9bf8+2*i)&0xffff)!=0xffff)
                    throw std::runtime_error("Attached effect changed an unrelated ring entry");
                handle=get(0x2d9bf8+30)&0xffff;
            } else {
                call(0x1015e8,{24,actor});handle=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            }
        }
        else if(request[0]==6||request[0]==26||request[0]==33||request[0]==115) {
            std::memset(ee->m_ram+0x2d9bf8,0xff,32);put(0x1fb948,15);
            call(0x130c50,{0x3e,0x123,request[0],0x80000});
            if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=0x8000c||get(0x1fb948)!=0)
                throw std::runtime_error("Positioned effect service payload/ring extent");
            for(unsigned i=0;i<15;++i)if((get(0x2d9bf8+2*i)&0xffff)!=0xffff)
                throw std::runtime_error("Positioned effect changed an unrelated ring entry");
            handle=get(0x2d9bf8+30)&0xffff;
        }
        else if(positioned){call(0x1012f8,{request[0],0x80000});handle=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);}
        else {call(0x101440,{request[0],0x80000,0x80010,0x80020});handle=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);}
        if(handle!=0)throw std::runtime_error("Map effect allocation order");
        put(0x1f85a4,1);
        std::vector<uint32> result,geometry,torchGeometry,uvGeometry,shadowUV;
        auto capture=[&](unsigned step){
            if(flame)torchGeometry.push_back(step);
            const auto job=get(table+4*handle);
            if(job>ram.size()-128||get(job+8)>32)throw std::runtime_error("Map effect job extent");
            result.insert(result.end(),{step,handle,get(job),get(job+4),get(job+8),get(job+0x60),get(job+0x64),get(random)});
            for(unsigned offset=0x10;offset<=0x5c;offset+=4)result.push_back(get(job+offset));
            result.push_back(get(job+0x6c));
            for(unsigned i=0;i<get(job+8);++i) {
                const auto element=get(job+12)+i*128,render=get(element+0x78);
                if(element>ram.size()-128||render>ram.size()-96)throw std::runtime_error("Map effect element extent");
                for(unsigned offset=0;offset<80;offset+=4)result.push_back(get(element+offset));
                for(unsigned offset:{0x54u,0x60u,0x70u,0x74u})result.push_back(get(element+offset));
                for(unsigned offset=0;offset<64;offset+=4)result.push_back(get(render+offset));
                unsigned texture=0;
                for(;texture<get(0x1f8498);++texture)if(get(get(0x24e780)+4*texture)==(get(element+0x5c)&0xffff))break;
                if(texture==get(0x1f8498)) {
                    if((attached||request[0]==6||request[0]==33||request[0]==90||request[0]==115||torch)&&(get(element+0x5c)&0xffff)==0xffff)texture=0xffffffff;
                    else throw std::runtime_error("Map effect texture identity");
                }
                result.insert(result.end(),{texture,get(element+0x68),get(element+0x6c),get(render+0x58)});
                if(flame&&i<3) {
                    const auto g=get(element+0x64);
                    if(g>ram.size()-48||get(g)||(get(g+0x1c)&0xffff)!=4)
                        throw std::runtime_error("Torch quad geometry extent");
                    torchGeometry.push_back(i);
                    for(unsigned vertex=0;vertex<4;++vertex) {
                        call(0x113690,{g,vertex});const auto point=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
                        if(point>ram.size()-6)throw std::runtime_error("Torch point extent");
                        torchGeometry.insert(torchGeometry.end(),{get(point),get(point+4)&0xffff});
                        call(0x113720,{g,0});const auto uv=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)+vertex*4;
                        if(uv>ram.size()-4)throw std::runtime_error("Torch UV extent");
                        torchGeometry.push_back(get(uv));
                    }
                }
                if(attached||request[0]==6||request[0]==26||request[0]==33||request[0]==90||request[0]==105||request[0]==106||request[0]==115) {
                    unsigned piece=0;
                    for(auto g=get(element+0x64);g;g=get(g)) {
                        if(g>ram.size()-48||piece>=32)throw std::runtime_error("Effect geometry chain extent");
                        const auto count=get(g+0x1c)&0xffff,tags=get(g+0x14);
                        if(count>512||tags>ram.size()-count)throw std::runtime_error("Effect geometry vertex extent");
                        geometry.insert(geometry.end(),{step,i,piece++,count});
                        for(unsigned vertex=0;vertex<count;++vertex) {
                            call(0x113690,{g,vertex});
                            const auto point=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
                            if(point>ram.size()-8)throw std::runtime_error("Effect geometry point extent");
                            geometry.insert(geometry.end(),{get(point),get(point+4),ee->m_ram[tags+vertex]});
                        }
                        if(request[0]==24&&i>=5) {
                            const auto count=get(g+0x1c)>>16;
                            if(count!=4)throw std::runtime_error("Shadow shared quad UV count");
                            call(0x113720,{g,0});const auto uv=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
                            if(uv>ram.size()-16)throw std::runtime_error("Shadow UV address");
                            shadowUV.insert(shadowUV.end(),{step,i,count});
                            for(unsigned j=0;j<4;++j)shadowUV.push_back(get(uv+j*4));
                        }
                        if(request[0]==26||request[0]==14||request[0]==105||request[0]==106) {
                            //0x105790 walks this UV array and its own tag table;
                            // XYZ tags are a separate geometry stream.
                            const auto uvCount=get(g+0x1c)>>16,uvTags=get(g+0x18);
                            if(uvCount>512||uvTags>ram.size()-uvCount)
                                throw std::runtime_error("Effect UV tag extent");
                            if(!uvCount)continue;
                            call(0x113720,{g,0});const auto uv=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
                            if(uv>ram.size()-uvCount*4)throw std::runtime_error("Effect UV extent");
                            uvGeometry.insert(uvGeometry.end(),{step,i,piece-1,uvCount});
                            for(unsigned vertex=0;vertex<uvCount;++vertex)
                                uvGeometry.insert(uvGeometry.end(),{get(uv+vertex*4),ee->m_ram[uvTags+vertex]});
                        }
                    }
                }
            }
        };
        capture(0);
        if(!positioned) {
            call(0x130d88,{0,request[2],request[3],0x80030});
            const auto next=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            if(next!=(request[2]<0xf00?0x80030u:0x8003cu))throw std::runtime_error("Map effect service payload extent");
        }
        capture(1);
        for(unsigned frame=0;frame<frames;++frame){
            const auto job=get(table+4*handle);
            if(attached&&!(get(job+4)&0x100)) {
                if(get(job+0x68)!=0x101118)throw std::runtime_error("Unexpected effect attachment callback");
                call(0x101118,{job});
            }
            call(0x1131d0,{handle});capture(frame+2);
        }
        if(request[0]==17||request[0]==19) {
            // Research-only geometry/material dump, not a native runtime input.
            std::vector<uint32> packets;
            const auto job=get(table+4*handle);
            for(unsigned i=0;i<9;++i) {
                const auto e=get(job+12)+i*128,r=get(e+0x78),g=get(e+0x64);
                packets.insert(packets.end(),{i,31,get(e+0x68),get(e+0x6c)});
                for(unsigned k=0;k<6;++k)packets.push_back(get(r+0x40+k*4));
                for(unsigned offset:{4u,8u,12u,16u}) {
                    const auto p=get(g+offset),bytes=(get(p)&65535)*16+32;
                    if(bytes>8192||p>ram.size()-bytes)throw std::runtime_error("Shape31 packet extent");
                    packets.push_back(bytes);
                    for(unsigned k=0;k<bytes;k+=4)packets.push_back(get(p+k));
                }
            }
            writeFile((std::string(output)+".packets.bin").c_str(),packets.data(),packets.size()*4);
            writeFile((std::string(output)+".tint.bin").c_str(),ee->m_ram+0x24e620,16);
        }
        if(flame)writeFile((std::string(output)+".torch-geometry.bin").c_str(),torchGeometry.data(),torchGeometry.size()*4);
        if(!geometry.empty())writeFile((std::string(output)+".geometry.bin").c_str(),geometry.data(),geometry.size()*4);
        if(!shadowUV.empty())writeFile((std::string(output)+".shadow-uv.bin").c_str(),shadowUV.data(),shadowUV.size()*4);
        if(!uvGeometry.empty())writeFile((std::string(output)+".uv.bin").c_str(),uvGeometry.data(),uvGeometry.size()*4);
        writeFile(output,result.data(),result.size()*4);
    }
    void effectProbe(const std::vector<uint8>& ram,unsigned id,unsigned parameter,unsigned frames,const char* output) {
        if(ram.size()!=32*1024*1024||id!=30||parameter>=32||frames>1000)
            throw std::runtime_error("Effect probe fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                        ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        loadMicro();call(0x1842c8);
        const auto camera=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        if(camera+16>ram.size())throw std::runtime_error("Effect camera pointer bounds");
        std::vector<uint32> result;
        for(unsigned i=0;i<4;++i)result.push_back(get(camera+i*4));
        call(0x101558,{id,0x10008,2});
        const auto handle=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        if(handle>=84)throw std::runtime_error("Effect allocation failed");
        call(0x1016a8,{handle,parameter});
        put(0x1f85a4,1);
        for(unsigned frame=0;frame<=frames;++frame) {
            if(frame)call(0x1131d0,{handle});
            const auto object=get(get(0x1f849c)+handle*4);
            if(object+128>ram.size()||get(object+8)!=2)throw std::runtime_error("Effect object bounds");
            result.push_back(get(object+0x64));result.push_back(get(object+4));
            for(unsigned i=0;i<2;++i) {
                const auto element=get(object+12)+i*128,render=get(element+0x78);
                if(element+128>ram.size()||render+96>ram.size())throw std::runtime_error("Effect element bounds");
                for(unsigned offset:{0u,4u,8u,16u,20u,24u,32u,36u,40u,44u,48u,52u,56u,60u})result.push_back(get(render+offset));
                result.push_back(get(element+0x2c));result.push_back(get(element+0x48));
                result.push_back(get(element+0x60));
                const auto texture=get(element+0x5c)&0xffff;
                unsigned logical=0;
                for(;logical<get(0x1f8498);++logical)if(get(get(0x24e780)+logical*4)==texture)break;
                if(logical==get(0x1f8498))throw std::runtime_error("Effect texture mapping absent");
                result.push_back(logical);result.push_back(get(element+0x68));result.push_back(get(element+0x6c));
            }
        }
        call(0x101648,{handle});
        const auto object=get(get(0x1f849c)+handle*4);
        if(!(get(object+4)&0x100))throw std::runtime_error("Original removal request failed");
        writeFile(output,result.data(),result.size()*4);
    }
    void cameraLookAt(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024 || input.empty() || input.size()%32)throw std::runtime_error("Camera fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        loadMicro();std::vector<uint8> result;
        for(std::size_t at=0;at<input.size();at+=32) {
            std::memcpy(ee->m_ram+0x1800000,input.data()+at,32);
            call(0x1b9440,{0x1800000,0x1800040,0x1800010});
            result.insert(result.end(),ee->m_ram+0x1800040,ee->m_ram+0x1800050);
        }
        writeFile(output,result.data(),result.size());
    }
    void streamBank(const std::vector<uint8>& input,const char* output) {
        constexpr uint32 source=0xa00000,slot=0x3092c0;
        if(input.size()<16 || input.size()>0x800000)throw std::runtime_error("RXWS oracle extent");
        std::memcpy(ee->m_ram+source,input.data(),input.size());
        std::memset(ee->m_ram+slot,0,256*0xa4);
        for(auto address:{0x178538u,0x178548u}){put(address,0x03e00008);put(address+4,0);}
        call(0x174950,{0,source});
        if(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0!=0)throw std::runtime_error("RXWS original registration failed");
        std::vector<uint32> result;
        for(auto offset:{8u,12u,16u,20u,24u}){const auto value=get(slot+offset);result.push_back(value ? value-source : 0);}
        result.push_back(get(slot+28));
        const auto count=get(get(slot+8));if(count>65536)throw std::runtime_error("RXWS original record count");result.push_back(count);
        for(unsigned i=0;i<count;++i){call(0x174f28,{0,i});const auto address=ee->m_EE.m_State.nGPR[CMIPS::V0].nD0;
            if(address<source || address-source+2>input.size())throw std::runtime_error("RXWS original record pointer");
            result.push_back(address-source);result.push_back(ee->m_ram[address+1]);}
        for(auto index:{0xffffffffu,count}){call(0x174f28,{0,index});result.push_back(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);}
        writeFile(output,result.data(),result.size()*4);
    }
    void instanceVisibility(const std::vector<uint8>& input,const char* output) {
        if(input.empty()||input.size()%32)throw std::runtime_error("Visibility fixture extent");
        constexpr uint32 objects=0xa00000,bodies=0xa20000,payload=0x900800;
        put(0x21301c,objects);put(0x337ef8,bodies);
        std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=32) {
            std::array<uint32,8> r{};std::memcpy(r.data(),input.data()+at,32);
            if(r[0]>=2048||!(r[1]&256)||r[2]>65535||r[3]>=4)
                throw std::runtime_error("Visibility fixture range");
            const auto obj=objects+r[0]*0x3c;
            put(obj,r[2]);put(obj+0x30,r[3]<<16);
            for(unsigned i=0;i<4;++i)put(bodies+i*0xb0+0xc,r[4+i]);
            // Unpatched retail callback and visibility service, nonnegative ID.
            call(0x1b7b48,{0,r[1],r[0],payload});
            const auto expected=(r[2]&~0x800u)|((r[1]&255)?0x800u:0u);
            if(get(obj)!=expected)throw std::runtime_error("Original changed unrelated instance flags");
            result.push_back((get(obj)&0x800)!=0);
            for(unsigned i=0;i<4;++i)result.push_back(get(bodies+i*0xb0+0xc));
            result.push_back(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void sceneEntryFade(const char* output) {
        for(auto address:{0x114ac0u,0x18fb68u,0x18fbf8u,0x18fc10u}){put(address,0x03e00008);put(address+4,0);}
        std::vector<uint32> result;
        for(uint32 initial:{0u,0x3f000000u,0x3f800000u}) {
            call(0x114c20);put(0x24eab0,initial);put(0x24eabc,0x123456);
            ee->m_EE.m_State.nCOP1[12]=0x3f800000;
            call(0x114cc0);
            for(unsigned frame=0;frame<=60;++frame) {
                if(frame)call(0x114b58);
                for(uint32 address:{0x24eaa4u,0x24eaa8u,0x24eab0u,0x24eab4u,0x24eabcu})result.push_back(get(address));
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void sceneFade(const std::vector<uint8>& input,const char* output) {
        if(input.empty()||input.size()%16)throw std::runtime_error("Scene fade fixture extent");
        put(0x18fb68,0x03e00008);put(0x18fb6c,0);
        const std::array<uint32,4> rgba={0x3c1a00a0,0xaf460000,0x03e00008,0};
        std::memcpy(ee->m_ram+0x18fc10,rgba.data(),sizeof(rgba));
        std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=16) {
            std::array<uint32,4> r{};std::memcpy(r.data(),input.data()+at,16);
            if(r[0]>100||r[2]>0xffffff)throw std::runtime_error("Scene fade fixture range");
            put(0x24eaac,1);put(0x24eaa4,0);put(0x24eaa8,0);put(0x24eab0,r[3]);
            put(0x24eab4,0);put(0x24eabc,r[2]);put(0xa00000,0xdeadbeef);
            call(0x130fb8,{0,r[0],r[1],0x900800});
            if(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0!=0x900800)throw std::runtime_error("Scene fade consumed payload");
            for(uint32 address:{0x24eaa4u,0x24eaa8u,0x24eab0u,0x24eab4u,0x24eabcu,0xa00000u})
                result.push_back(get(address));
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void jingleRouting(const char* output) {
        constexpr uint32 record=0xa00000;
        const std::array<uint32,8> startCode={0x3c1a00a0,0xaf440000,0xaf450004,0xaf460008,
            0xaf47000c,0x24021234,0x03e00008,0};
        const std::array<uint32,5> gainCode={0x3c1a00a0,0xaf440010,0xaf450014,0x03e00008,0};
        std::memcpy(ee->m_ram+0x1708f8,startCode.data(),sizeof(startCode));
        std::memcpy(ee->m_ram+0x170878,gainCode.data(),sizeof(gainCode));
        std::vector<uint32> result;
        for(uint32 current:{0xffffffffu,10u})for(uint32 id:{20u,21u,22u,23u,33u,34u}) {
            std::memset(ee->m_ram+record,0xff,24);
            for(unsigned i=0;i<6;++i){put(0x2d9ae0+i*4,0x100+i);put(0x2d9af8+i*4,0x200+i);}
            put(0x2d9ab4,current);put(0x2d9abc,1);put(0x2d9ab8,0x8765);
            put(0x2d9ac8,0x123000);put(0x2d9acc,0x456000);put(0x2d9ad0,0x2000);put(0x2d9a00,1);
            call(0x12d580,{id,0,17});
            result.insert(result.end(),{id,current});
            for(unsigned i=0;i<6;++i)result.push_back(get(record+i*4));
            for(uint32 address:{0x2d9ab4u,0x2d9abcu,0x2d9ab8u,0x2d9ac8u,0x2d9accu,0x2d9ad0u,0x2d9a00u})
                result.push_back(get(address));
            for(unsigned i=0;i<6;++i)result.push_back(get(0x2d9af8+i*4));
            for(unsigned i=0;i<6;++i)result.push_back(get(0x2d9ae0+i*4));
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void musicChannels(const std::vector<uint8>& input,const char* output) {
        if(input.empty()||input.size()%16||input.size()>16*512)throw std::runtime_error("Music channel fixture bounds");
        for(auto address:{0x1c7910u,0x16ddc0u,0x174900u,0x1709d8u}){put(address,0x03e00008);put(address+4,0);}
        put(0x178a60,0x03e00008);put(0x178a64,0x00001021);
        constexpr uint32 record=0xa00000,payload=0x80000;
        const std::array<uint32,11> code={0x3c1a00a0,0x8f5b0000,0x277b0001,0xaf5b0000,
            0x0005d880,0x037ad821,0x00071400,0x00461025,0xaf620010,0x03e00008,0};
        std::memcpy(ee->m_ram+0x170c00,code.data(),sizeof(code));
        std::memset(ee->m_ram+0x2d9a00,0,0x100);put(0x2d9ab8,0xffffffff);
        std::vector<uint32> outputWords;
        for(unsigned at=0;at<input.size();at+=16) {
            std::array<uint32,4> r;std::memcpy(r.data(),input.data()+at,16);
            put(record,0);for(unsigned i=0;i<16;++i)put(record+16+i*4,0xffffffff);
            if(r[0]==0) {
                if(r[1]>15||r[2]>15||r[3]>128)throw std::runtime_error("Music channel request scope");
                put(payload,r[3]);call(0x130b40,{0,r[2],r[1],payload});
                if(uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)!=payload+4)
                    throw std::runtime_error("Music channel payload consumption");
            } else if(r[0]==1) {
                if(r[1]>1||r[2]||r[3])throw std::runtime_error("Music channel flush scope");
                put(0x2d9ab8,r[1]?3:0xffffffff);call(0x12ce20);
            } else if(r[0]==2) {
                if(r[1]||r[2]||r[3])throw std::runtime_error("Music channel cancel scope");
                call(0x12d470);
            } else throw std::runtime_error("Unknown music channel probe operation");
            outputWords.push_back(get(0x2d9a00));
            for(unsigned i=0;i<16;++i)outputWords.push_back(get(0x2d9a08+i*4));
            outputWords.push_back(get(record));
            for(unsigned i=0;i<16;++i)outputWords.push_back(get(record+16+i*4));
        }
        writeFile(output,outputWords.data(),outputWords.size()*4);
    }
    void musicFade(const std::vector<uint8>& input,const char* output) {
        if(input.empty() || input.size()%16)throw std::runtime_error("Music fade fixture bounds");
        for(auto address:{0x1c7910u,0x16ddc0u,0x174900u}){put(address,0x03e00008);put(address+4,0);}
        // Original stop path requires the main-thread predicate to be zero.
        put(0x178a60,0x03e00008);put(0x178a64,0x00001021);
        constexpr uint32 record=0xa00000;
        const std::array<uint32,5> stopCode={0x3c1a00a0,0x241b0001,0xaf5b0000,0x03e00008,0};
        const std::array<uint32,7> gainCode={0x3c1a00a0,0x241b0001,0xaf5b0004,0xaf440008,0xaf45000c,0x03e00008,0};
        std::memcpy(ee->m_ram+0x1709d8,stopCode.data(),sizeof(stopCode));
        std::memcpy(ee->m_ram+0x170878,gainCode.data(),sizeof(gainCode));
        std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=16){
            std::array<uint32,4> r;std::memcpy(r.data(),input.data()+at,16);
            if(r[0]>0x800000 || r[1]>128 || r[2]>32767 || r[3]>10000)throw std::runtime_error("Music fade fixture range");
            std::memset(ee->m_ram+0x2d9a00,0,0x100);put(0x2d9ac8,r[0]);put(0x2d9ab8,3);put(record,0);
            call(0x12d778,{r[1],r[2]});
            for(unsigned i=0;i<r[3];++i){
                put(record+4,0);put(record+8,0xffffffff);put(record+12,0xffffffff);
                call(0x12ce20);
                for(auto address:{0x2d9ac8u,0x2d9accu,0x2d9ad0u,record,record+4,record+8,record+12})result.push_back(get(address));
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void dialogueOpacity(const std::vector<uint8>& input,const char* output) {
        if(input.empty() || input.size()%16)throw std::runtime_error("Opacity fixture bounds");
        constexpr uint32 object=0xa00000;std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=16) {
            std::memcpy(ee->m_ram+object+0x270,input.data()+at,16);
            call(0x1998a8,{object});
            for(unsigned i=0;i<4;++i)result.push_back(get(object+0x270+i*4));
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void textGlyphs(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024 || input.empty() || input.size()%28)throw std::runtime_error("Glyph fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        constexpr uint32 object=0xa00000,positions=0xb00000,dimensions=0xb00100;
        std::memset(ee->m_ram+object,0,0x7000);
        put(object+0x650,positions);put(object+0x654,dimensions);
        put(object+0x224,0x3f800000);put(object+0x228,0x3f800000);put(object+0x4f8,0x10);
        std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=28) {
            std::array<uint32,7> r;std::memcpy(r.data(),input.data()+at,28);
            if(r[0]<32 || r[0]>255 || r[1]>2)throw std::runtime_error("Unsupported glyph fixture");
            put(object+0x20c,r[1]);put(object+0x22c,r[2]);
            const float code=r[0];uint32 bits;std::memcpy(&bits,&code,4);put(dimensions+8,bits);
            call(0x19c568,{object,0,0,r[3],r[4],r[5],r[6]});
            for(auto p:{positions,positions+4,positions+12,dimensions,dimensions+4,dimensions+8,dimensions+12})result.push_back(get(p));
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void fontMetrics(const std::vector<uint8>& input,const char* output) {
        if(input.size()<8 || input.size()>0x100000 || input[4]>input[5])throw std::runtime_error("Font fixture bounds");
        constexpr uint32 table=0xa00000,font=0xb00000;
        const auto count=unsigned(input[5])-input[4]+1;
        if(input.size()<8+count*20)throw std::runtime_error("Font fixture records");
        std::memset(ee->m_ram+table,0,0x400);put(0x2003ec,table);
        std::memcpy(ee->m_ram+font,input.data(),input.size());
        // Original loader mode 0, flag 0x800: convert metrics without creating
        // a GS texture. This is a real original option; no instructions patched.
        call(0x1947a0,{0,font,0x800});
        if(get(table+0xc)!=count || get(table+0x10)!=font+8)throw std::runtime_error("Original font result");
        writeFile(output,ee->m_ram+font+8,count*20);
    }
    void textClock(const std::vector<uint8>& input,const char* output) {
        if(input.empty() || input.size()%40)throw std::runtime_error("Text clock fixture bounds");
        constexpr uint32 object=0xa00000;
        std::vector<uint32> results;
        for(std::size_t at=0;at<input.size();at+=40) {
            std::array<uint32,10> r;std::memcpy(r.data(),input.data()+at,40);
            std::memset(ee->m_ram+object,0,0x7000);
            std::memset(ee->m_ram+0x200688,0,32);put(0x200688,object);put(0x2006b0,1);
            put(object+0x1900,r[0]);put(object+0x25c,r[1]);put(object+0xcd0,r[2]);put(object+0xcd4,r[3]);
            put(object+0xcd8,r[4]);put(object+0xce8,r[5]);put(object+0x260,r[6]);put(object+0x264,r[7]);
            put(object+0x268,r[8]);put(object+0x26c,r[9]);
            call(0x1980f8);
            for(auto offset:{0x1900,0xcd0,0xcd4,0xcd8,0xce8,0x264})results.push_back(get(object+offset));
        }
        writeFile(output,results.data(),results.size()*4);
    }
    void pmeMessage(const std::vector<uint8>& ram,int32 id,const char* output,bool indices=false,const char* name=nullptr) {
        if(ram.size()!=32*1024*1024)throw std::runtime_error("Message oracle requires 32 MiB EE snapshot");
        std::memcpy(ee->m_ram,ram.data(),ram.size());
        put(stop,0x08000000|(stop>>2));put(stop+4,0);
        if(!get(0x20068c)) {
            for(unsigned i=0;i<8;++i)if(get(0x200688+i*4))
                throw std::runtime_error("PME fixture has an unexpected text roster");
            const auto field=get(0x2d9bf4);
            if(!field||field>ram.size()-0x800||get(field+0x50))
                throw std::runtime_error("PME fixture has an active/invalid field dialogue");
            effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                           ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
            loadMicro();call(0x11f570,{field,0x2293e0,0,uint32(id)});
            if(!get(0x20068c))throw std::runtime_error("PME fixture message construction failed");
        }
        if(name) {
            if(std::strlen(name)>12)throw std::runtime_error("Name oracle fixture exceeds reserved name capacity");
            constexpr uint32 tag=0x7fd000,value=0x7fd100;
            std::memcpy(ee->m_ram+tag,"############",13);
            std::memcpy(ee->m_ram+value,name,std::strlen(name)+1);
            call(0x19a5c8,{1,tag,value});
        }
        // Live text handle1 and the snapshot's loaded bank0.
        ee->m_EE.m_State.nGPR[CMIPS::A2].nD0=int64(id);
        call(0x19a150,{1,0});
        if(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0!=1)throw std::runtime_error("Original message selection failed");
        const auto object=get(0x20068c);
        std::vector<uint8> result;
        for(unsigned i=0;i<=256;++i) {
            const auto at=get(object+0x2ee0+i*4);
            if(!at) {writeFile(output,result.data(),result.size());return;}
            if(at>=ram.size())throw std::runtime_error("Message oracle string bounds");
            if(indices) {
                const auto bank=get(0x2005c8),table=get(bank+8),count=get(bank);
                uint32 index=0;
                for(;index<count;++index)if(get(table+index*20+8)==at)break;
                if(index==count)throw std::runtime_error("Selected string missing from live bank");
                for(unsigned j=0;j<4;++j)result.push_back(index>>(8*j));
                continue;
            }
            auto end=std::find(ee->m_ram+at,ee->m_ram+ram.size(),0);
            if(end==ee->m_ram+ram.size())throw std::runtime_error("Message oracle unterminated string");
            const uint32 size=end-(ee->m_ram+at);
            for(unsigned j=0;j<4;++j)result.push_back(size>>(8*j));
            result.insert(result.end(),ee->m_ram+at,end);
        }
        throw std::runtime_error("Message oracle fragment limit");
    }
    void padRepeat(const std::vector<uint8>& input,const char* output) {
        if(input.empty() || input.size()%4)throw std::runtime_error("Pad fixture requires held/pressed halfwords");
        put(0x339594,2400u<<16);put(0x339598,480);std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=4){
            std::memcpy(ee->m_ram+0x33958c,input.data()+at,4);call(0x1bf7c8,{0,0});
            result.insert(result.end(),{get(0x339590)>>16,get(0x339594)&0xffff,get(0x339598)>>16});
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void dialogueShadow(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%32||input.size()>32*256)
            throw std::runtime_error("Dialogue shadow fixture extent");
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=32)for(unsigned dispatch=0;dispatch<2;++dispatch) {
            uint32 f[8];std::memcpy(f,input.data()+at,32);
            if(f[2]>5||f[3]>1||f[4]>1||f[7]>4)throw std::runtime_error("Dialogue shadow fixture bounds");
            ee->m_EE.m_executor->Reset();std::memcpy(ee->m_ram,ram.data(),ram.size());
            put(stop,0x08000000|(stop>>2));put(stop+4,0);loadMicro();
            effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
            const auto field=get(0x2d9bf4),object=get(0x20068c),vars=get(0x24f788);
            if(!field||field>ram.size()-0x800||!object||object>ram.size()-0x7000||!vars||vars>ram.size()-0x600)
                throw std::runtime_error("Dialogue shadow fixture pointers");
            const auto message=get(field+0x50),player=get(field+0x44);
            if(!message||message>ram.size()-0x1000||!player||player>ram.size()-0x284)
                throw std::runtime_error("Dialogue shadow field pointers");
            put(0x24f738,f[7]);
            put(player+0x260,f[2]);put(player+0x264,f[3]);put(player+0x268,f[4]);
            put(player+0x26c,0xffffffff);put(player+0x270,0x3ec00000);
            put(vars+0x518,9);
            put(message+0xe48,1);put(message+0xe8c,0);put(message+0xe4c,5);put(message+0xed8,1);
            put(message+0xed4,4);put(message+0x170,0x50);put(message+0x1f0,4);
            put(0x1fcb60,1);put(0x1fcb64,0);put(0x33958c,0);put(0x339590,0);
            put(object+0x1970,255);put(object+0x1900,f[6]);put(object+0xcd0,9);
            put(object+0xcd4,0xfffffffe);put(object+0xcd8,f[5]);put(object+0x3d08,5);
            const auto command="SHADOW("+std::to_string(int32(f[1]))+")";
            std::strcpy(reinterpret_cast<char*>(ee->m_ram+object+0x1c70),command.c_str());
            try {if(dispatch)call(0x148dc8,{message});else call(0x148240,{message,object+0x1c70,0});}
            catch(const std::exception& e){throw std::runtime_error("Dialogue shadow case="+std::to_string(f[0])+" dispatch="+std::to_string(dispatch)+": "+e.what());}
            result.insert(result.end(),{f[0],dispatch,dispatch?0u:ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0]});
            for(unsigned i=0;i<5;++i)result.push_back(get(player+0x260+i*4));
            result.push_back(get(vars+0x518));
            for(auto offset:{0x1900,0xcd0,0xcd4,0xcd8,0x3d08})result.push_back(get(object+offset));
            result.insert(result.end(),{get(message+0xed4),get(message+0x170),get(message+0xe4c),
                get(object+0x1c70)&0xffff,get(message+0xed8),get(0x24f738)});
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void relationship(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%40||input.size()>40*256)
            throw std::runtime_error("Relationship fixture extent");
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=40) {
            ee->m_EE.m_executor->Reset();
            std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
            effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                           ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
            const auto field=get(0x2d9bf4),message=get(field+0x50),object=get(0x20068c),vars=get(0x24f788);
            if(!message||get(message+0x8c)!=10001||!object||!vars)
                throw std::runtime_error("Relationship fixture message identity");
            uint32 row[10];std::memcpy(row,input.data()+at,40);
            if(row[0]>2||row[1]>12||row[2]>12||row[3]>12)throw std::runtime_error("Relationship fixture selection/code");
            const auto command=std::string("LA(")+std::to_string(row[1])+"/"+std::to_string(row[2])+"/"+std::to_string(row[3])+")";
            const auto setup=[&]{
                put(message+0xe48,1);put(message+0xe8c,0);put(message+0xe4c,5);put(message+0xed8,1);
                put(message+0xed4,4);put(message+0x170,0x50);put(message+0x1f0,4);
                put(0x1fcb60,1);put(0x1fcb64,0);put(0x33958c,0);put(0x339590,0);
                for(unsigned i=0;i<6;++i)put(vars+0x510+(16+i)*4,row[4+i]);
                put(object+0x1e70,row[0]);put(object+0x1970,255);
                put(object+0x1900,0x8910);put(object+0xcd0,9);put(object+0xcd4,0xfffffffe);
                put(object+0xcd8,7);put(object+0x3d08,5);
                std::strcpy(reinterpret_cast<char*>(ee->m_ram+object+0x1c70),command.c_str());
            };
            const auto capture=[&]{
                for(unsigned i=0;i<6;++i)result.push_back(get(vars+0x510+(16+i)*4));
                for(auto offset:{0x1900,0xcd0,0xcd4,0xcd8,0x3d08})result.push_back(get(object+offset));
            };
            setup();call(0x148240,{message,object+0x1c70,0});
            result.push_back(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);capture();result.push_back(get(object+0x1c70)&0xffff);
            setup();call(0x148dc8,{message});capture();
            result.insert(result.end(),{get(message+0xed4),get(message+0x170),get(message+0xe4c),
                                       get(object+0x1c70)&0xffff,get(object+0x1e70)});
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void comparisonOptions(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%16||input.size()>16*512)
            throw std::runtime_error("Comparison option fixture extent");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        constexpr uint32 message=0x900000,command=0x902000;
        std::memset(ee->m_ram+message,0,0x1000);put(message+0x40,1);
        const std::array<const char*,4> operators={"<",">","=","!="};std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=16) {
            uint32 row[4];std::memcpy(row,input.data()+at,16);
            if(row[0]>3||(row[1]!=0&&row[1]!=1&&row[1]!=4))throw std::runtime_error("Comparison option fixture scope");
            call(0x125918,{0,row[1],row[2]});
            const auto text="CMP(0/"+std::to_string(row[1])+"/"+std::to_string(int32(row[3]))+")"+operators[row[0]];
            std::strcpy(reinterpret_cast<char*>(ee->m_ram+command),text.c_str());
            call(0x148240,{message,command,1});result.push_back(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void choiceOptions(const std::vector<uint8>& ram,const char* output) {
        if(ram.size()!=32*1024*1024)throw std::runtime_error("Option oracle requires EE snapshot");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        constexpr uint32 message=0x900000,command=0x902000;
        std::memset(ee->m_ram+message,0,0x1000);put(message+0x40,1);
        const auto object=get(0x20068c);std::vector<uint32> result;
        for(auto option:{"CC","BC","BC=0","BC=1","BC!=0"})for(unsigned selection=0;selection<3;++selection){
            std::strcpy(reinterpret_cast<char*>(ee->m_ram+command),option);put(object+0x1e70,selection);
            call(0x148240,{message,command,1});result.push_back(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void pmeChoice(const std::vector<uint8>& ram,int32 id,unsigned fragment,unsigned selection,const char* output) {
        if(ram.size()!=32*1024*1024 || fragment>=256 || selection>=512)
            throw std::runtime_error("Choice oracle fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());
        put(stop,0x08000000|(stop>>2));put(stop+4,0);
        ee->m_EE.m_State.nGPR[CMIPS::A2].nD0=int64(id);
        call(0x19a150,{1,0});
        if(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0!=1)throw std::runtime_error("Original message selection failed");
        const auto object=get(0x20068c),bank=get(0x2005c8),table=get(bank+8),count=get(bank);
        if(!get(object+0x2ee0+fragment*4))throw std::runtime_error("Choice fragment absent");
        put(object+0x269c,fragment);put(object+0x1e74,selection);
        std::vector<uint32> result;
        auto snapshot=[&]{
            for(auto offset:{0x1900,0x3d08,0x1e70,0x1e74,0x269c,0x1968,0x196c,0xcd0,0xcd4,0xcd8,0x22c})
                result.push_back(get(object+offset));
            const auto sizeAt=result.size();result.push_back(0);
            for(unsigned i=0;i<=256;++i){
                const auto address=get(object+0x2ee0+i*4);if(!address)return;
                if(i==256)throw std::runtime_error("Choice fragment capacity");
                uint32 index=0;for(;index<count;++index)if(get(table+index*20+8)==address)break;
                if(index==count)throw std::runtime_error("Choice string absent from bank");
                result.push_back(index);result.push_back(get(object+0x32e0+i*4));++result[sizeAt];
            }
        };
        snapshot();
        // Execute the original selector and reset; no calls patched or stubbed.
        call(0x19b448,{1,selection});snapshot();
        writeFile(output,result.data(),result.size()*4);
    }
    void xclProbe(const std::vector<uint8>& asset,const std::vector<uint8>& queries,const char* output) {
        if(asset.size()<69||asset.size()>0x400000||queries.size()%28)throw std::runtime_error("XCL oracle fixture bounds");
        constexpr uint32 base=0x900000;
        std::memcpy(ee->m_ram+base,asset.data(),asset.size());
        call(0x189028,{base,uint32(asset.size())});
        const auto size=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        if(size>asset.size()-4)throw std::runtime_error("XCL relocation result outside input");
        std::vector<uint8> result;auto append=[&](uint32 v){const auto* p=reinterpret_cast<const uint8*>(&v);result.insert(result.end(),p,p+4);};
        append(size);result.insert(result.end(),ee->m_ram+base,ee->m_ram+base+size);
        call(0x188f90,{base});
        for(unsigned at=0;at<queries.size();at+=28) {
            for(unsigned i=0;i<6;++i)std::memcpy(&ee->m_EE.m_State.nCOP1[12+i],queries.data()+at+i*4,4);
            uint32 mode;std::memcpy(&mode,queries.data()+at+24,4);if(mode>2)throw std::runtime_error("XCL oracle query mode");
            call(0x189258,{mode});const auto count=get(0x1fec48);
            if(count>64)throw std::runtime_error("Original XCL query overflow");append(count);
            for(unsigned i=0;i<count;++i){const auto p=get(0x1feb48+i*4);if(p<base||p>=base+size)throw std::runtime_error("XCL query pointer bounds");append(p-base);}
        }
        writeFile(output,result.data(),result.size());
    }
    void materialColors(const std::vector<uint8>& input,const char* output) {
        if(input.empty()||input.size()%20||input.size()>20*8192)
            throw std::runtime_error("Material color fixture bounds");
        std::vector<uint32> result;
        for(std::size_t at=0;at<input.size();at+=20) {
            auto& cpu=ee->m_EE.m_State;
            std::memcpy(&cpu.nCOP1[12],input.data()+at,4);
            call(0x17e020);
            float alpha;std::memcpy(&alpha,&cpu.nCOP1[0],4);
            for(unsigned i=0;i<4;++i) {
                float value;std::memcpy(&value,input.data()+at+4+i*4,4);
                value=value*(i==3?alpha:255.f);
                std::memcpy(&cpu.nCOP1[12],&value,4);call(0x17dfe8);
                result.push_back(cpu.nCOP1[0]);
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void xclVertical(const std::vector<uint8>& asset,const std::vector<uint8>& queries,const char* output) {
        if(asset.size()<69||asset.size()>0x400000||queries.size()%48)throw std::runtime_error("XCL contact fixture bounds");
        constexpr uint32 base=0x900000,record=0xe00000,previous=0xe00100,desired=0xe00110;
        std::memcpy(ee->m_ram+base,asset.data(),asset.size());
        call(0x189028,{base,uint32(asset.size())});call(0x188f90,{base});
        put(0x1feb3c,0);std::vector<uint32> result;
        for(unsigned at=0;at<queries.size();at+=48) {
            std::memset(ee->m_ram+record,0,0xb0);
            std::memcpy(ee->m_ram+previous,queries.data()+at,32);
            std::memcpy(ee->m_ram+record,queries.data()+at+32,4);
            uint32 threshold,mask,kind;
            std::memcpy(&threshold,queries.data()+at+36,4);std::memcpy(&mask,queries.data()+at+40,4);std::memcpy(&kind,queries.data()+at+44,4);
            if(threshold>16384||kind>1)throw std::runtime_error("XCL contact settings");
            put(0x1feb34,threshold);put(0x1feb28,mask);
            call(kind?0x18a640:0x18a0e0,{previous,desired,0,record});
            std::array<uint32,16> row{};
            row[0]=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];row[1]=get(record+0x28);
            if(row[0]) {
                row[2]=(get(record+0x40)>>(kind?16:0))&0xffff;
                row[3]=get(record+0x8c)-base;row[4]=get(record+0x88);row[5]=get(record+0x84);
                for(unsigned j=0;j<4;++j)row[6+j]=get(record+0x30+j*4);
                for(unsigned j=0;j<3;++j)row[10+j]=get(record+(kind?0x60:0x50)+j*4);
                row[13]=get(record+0x90)-base;row[14]=get(record+0x94)-base;row[15]=get(record+0x98);
            }
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void xclWall(const std::vector<uint8>& asset,const std::vector<uint8>& queries,const char* output) {
        if(asset.size()<69||asset.size()>0x400000||queries.size()%76)throw std::runtime_error("XCL wall fixture bounds");
        constexpr uint32 base=0x900000,record=0xe00000,previous=0xe00100,desired=0xe00110,capsule=0xe00200;
        std::memcpy(ee->m_ram+base,asset.data(),asset.size());
        call(0x189028,{base,uint32(asset.size())});call(0x188f90,{base});
        put(0x1feb40,65536);put(0x1feb44,0xf00000);put(0x1feb3c,0);
        std::vector<uint32> result;
        for(unsigned at=0;at<queries.size();at+=76) {
            std::memset(ee->m_ram+record,0,0xb0);
            std::memcpy(ee->m_ram+previous,queries.data()+at,32);
            std::memcpy(ee->m_ram+capsule,queries.data()+at+32,36);
            uint32 threshold,mask;std::memcpy(&threshold,queries.data()+at+68,4);std::memcpy(&mask,queries.data()+at+72,4);
            if(threshold>16384)throw std::runtime_error("XCL wall threshold");
            put(0x1feb34,threshold);put(0x1feb28,mask);
            call(0x18bb10,{previous,desired,capsule,record});
            std::array<uint32,10> row{};row[0]=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];row[1]=get(record+0x28);
            if(row[0]) {
                row[2]=get(record+0x44)&0xffff;
                for(unsigned j=0;j<4;++j)row[3+j]=get(record+0x30+j*4);
                for(unsigned j=0;j<3;++j)row[7+j]=get(record+0x70+j*4);
            }
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void xclCamera(const std::vector<uint8>& asset,const std::vector<uint8>& queries,const char* output) {
        if(asset.size()<69||asset.size()>0x400000||queries.empty()||queries.size()%48||queries.size()>48*10000)
            throw std::runtime_error("Camera collision fixture bounds");
        constexpr uint32 base=0x900000,record=0xe00000,previous=0xe00100,desired=0xe00110,sphere=0xe00200,shape=0xe00300;
        std::memcpy(ee->m_ram+base,asset.data(),asset.size());
        call(0x189028,{base,uint32(asset.size())});call(0x188f90,{base});
        put(0x1feb40,65536);put(0x1feb44,0xf00000);put(0x1feb3c,0);
        put(0x1fe318,0x3f000000);put(0x1fe320,2);put(0x1feb30,0x3efff17d);
        std::vector<uint32> result;
        for(unsigned at=0;at<queries.size();at+=48) {
            auto word=[&](unsigned n){uint32 v;std::memcpy(&v,queries.data()+at+4*n,4);return v;};
            const auto mode=word(8);
            if(mode>2||word(11)>16384||(mode==2&&(word(9)!=0x40900000||word(11)!=8190)))throw std::runtime_error("Camera collision fixture settings");
            std::memset(ee->m_ram+record,0,0xb0);std::memcpy(ee->m_ram+previous,queries.data()+at,32);
            for(unsigned i=0;i<3;++i)put(sphere+4*i,0);put(sphere+12,0x3f800000);put(sphere+16,word(9));
            put(0x1feb34,word(11));put(0x1feb28,word(10));
            if(mode==2) {
                put(record+4,1);put(record+8,shape);put(record+12,1);put(shape,0x39);put(shape+4,sphere);
                call(0x186fa0,{previous,desired,0,record});
            } else if(mode==1)call(0x18b020,{previous,desired,sphere,record});
            else call(0x1882a0,{previous,desired,record});
            std::array<uint32,22> row{};row[0]=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];row[1]=get(record+0x28);
            row[2]=get(record+0x40)&0xffff;row[3]=get(record+0x40)>>16;row[4]=get(record+0x44)&0xffff;
            if(row[0])for(unsigned i=0;i<4;++i)row[5+i]=get(record+0x30+4*i);
            for(unsigned kind=0;kind<3;++kind)if(row[1]&(1u<<kind))
                for(unsigned i=0;i<3;++i)row[9+kind*3+i]=get(record+0x50+kind*16+4*i);
            for(unsigned i=0;i<4;++i)row[18+i]=get(desired+4*i);
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void xclMove(const std::vector<uint8>& asset,const std::vector<uint8>& queries,const char* output) {
        if(asset.size()<69||asset.size()>0x400000||queries.size()%112)throw std::runtime_error("XCL movement fixture bounds");
        constexpr uint32 base=0x900000,record=0xe00000,previous=0xe00100,desired=0xe00110,capsule=0xe00200,shapes=0xe00300,point=0xe00320;
        std::memcpy(ee->m_ram+base,asset.data(),asset.size());
        call(0x189028,{base,uint32(asset.size())});call(0x188f90,{base});
        put(0x1feb40,65536);put(0x1feb44,0xf00000);put(0x1feb3c,0);
        std::vector<uint32> result;
        for(unsigned at=0;at<queries.size();at+=112) {
            auto word=[&](unsigned offset){uint32 v;std::memcpy(&v,queries.data()+at+offset,4);return v;};
            if(word(68)>16384||word(100)>64||(word(104)&7)!=0||(word(104)&0x20)||(word(108)&7)!=2)
                throw std::runtime_error("XCL movement fixture settings");
            std::memset(ee->m_ram+record,0,0xb0);
            std::memcpy(ee->m_ram+previous,queries.data()+at,32);
            std::memcpy(ee->m_ram+capsule,queries.data()+at+32,36);
            std::memcpy(ee->m_ram+point,queries.data()+at+76,16);
            put(record,word(92));put(record+4,2);put(record+8,shapes);
            put(shapes,word(104));put(shapes+4,point);put(shapes+8,word(108));put(shapes+12,capsule);
            put(0x1feb34,word(68));put(0x1feb28,word(72));put(0x1fe318,word(96));put(0x1fe320,word(100));
            call(0x187270,{previous,desired,0,record});
            std::array<uint32,18> row{};row[0]=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];row[1]=get(record+0x28);
            row[2]=get(record+0x40)&0xffff;row[3]=get(record+0x40)>>16;row[4]=get(record+0x44)&0xffff;
            if(row[0])for(unsigned j=0;j<4;++j)row[5+j]=get(record+0x30+j*4);
            for(unsigned kind=0;kind<3;++kind)if(row[1]&(1u<<kind))
                for(unsigned j=0;j<3;++j)row[9+kind*3+j]=get(record+0x50+kind*16+j*4);
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void capsulePair(const std::vector<uint8>& queries,const char* output) {
        if(queries.size()%128)throw std::runtime_error("Capsule pair fixture extent");
        constexpr uint32 record=0xe00000,previous=0xe00100,desired=0xe00110,position=0xe00120,moving=0xe00200,other=0xe00300;
        std::vector<uint32> result;
        for(unsigned at=0;at<queries.size();at+=128) {
            std::memset(ee->m_ram+record,0,0xb0);
            std::memcpy(ee->m_ram+previous,queries.data()+at,48);
            std::memcpy(ee->m_ram+moving,queries.data()+at+48,36);
            std::memcpy(ee->m_ram+other,queries.data()+at+84,36);
            std::memcpy(ee->m_ram+record,queries.data()+at+120,4);
            std::memcpy(ee->m_ram+0x1feb30,queries.data()+at+124,4);
            call(0x18dbf8,{previous,desired,moving,position,other,record});
            std::array<uint32,14> row{};row[0]=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];row[1]=get(record+0x28);
            if(row[0])for(unsigned j=0;j<3;++j)row[2+j]=get(record+0x30+j*4);
            for(unsigned kind=0;kind<3;++kind)if(row[1]&(1u<<kind))
                for(unsigned j=0;j<3;++j)row[5+kind*3+j]=get(record+0x50+kind*16+j*4);
            result.insert(result.end(),row.begin(),row.end());
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void collisionWorld(const std::vector<uint8>& asset,const std::vector<uint8>& queries,const char* output) {
        if(asset.size()<69||asset.size()>0x400000||queries.size()%396)throw std::runtime_error("Collision world fixture bounds");
        constexpr uint32 base=0x900000,records=0xe00000,desired=0xe01000;
        std::memcpy(ee->m_ram+base,asset.data(),asset.size());call(0x189028,{base,uint32(asset.size())});call(0x188f90,{base});
        put(0x1feb40,65536);put(0x1feb44,0xf00000);put(0x1feb3c,0);std::vector<uint32> result;
        for(unsigned at=0;at<queries.size();at+=396) {
            auto word=[&](unsigned off){uint32 v;std::memcpy(&v,queries.data()+at+off,4);return v;};
            const auto moving=word(0);if(moving>=3||word(4)!=3)throw std::runtime_error("Collision world body count");
            std::memset(ee->m_ram+records,0,0x1000);std::memset(ee->m_ram+0x1fe328,0,256*8);
            put(0x1feb28,word(8));put(0x1feb34,word(12));put(0x1fe320,word(16));put(0x1fe31c,word(20));
            put(0x1fe318,word(24));put(0x1feb30,word(28));std::memcpy(ee->m_ram+desired,queries.data()+at+32,16);
            for(unsigned i=0;i<3;++i) {
                const auto off=48+i*116,record=records+i*0x200,previous=record+0x100,position=record+0x110,
                           point=record+0x120,capsule=record+0x130,shapes=record+0x160;
                std::memcpy(ee->m_ram+previous,queries.data()+at+off,84);
                put(record,word(off+88));put(record+4,2);put(record+8,shapes);put(record+0xc,word(off+92));
                put(record+0x14,word(off+96));put(record+0x18,word(off+84));put(record+0x1c,previous);put(record+0x20,position);
                put(record+0x9c,word(off+100));put(shapes,word(off+104));put(shapes+4,point);
                put(shapes+8,word(off+108));put(shapes+12,capsule);
                put(0x1fe328+i*8,word(off+112));put(0x1fe32c+i*8,record);
            }
            const auto record=records+moving*0x200;call(0x186fa0,{record+0x100,desired,0,record});
            result.push_back(ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0]);
            for(unsigned j=0;j<4;++j)result.push_back(get(desired+j*4));
            for(unsigned i=0;i<3;++i) {
                const auto r=records+i*0x200,flags=get(r+0x28);
                for(unsigned j=0;j<4;++j)result.push_back(get(r+0x110+j*4));result.push_back(flags);
                result.push_back(get(r+0x40)&0xffff);result.push_back(get(r+0x40)>>16);result.push_back(get(r+0x44)&0xffff);
                const auto target=get(r+0x80);
                if(target&&(target<records||target>=records+3*0x200||(target-records)%0x200))throw std::runtime_error("Collision target outside fixture");
                result.push_back(target?(target-records)/0x200+1:0);
                for(unsigned j=0;j<4;++j)result.push_back((flags&15)?get(r+0x30+j*4):0);
                for(unsigned kind=0;kind<3;++kind)for(unsigned j=0;j<3;++j)
                    result.push_back((flags&(1u<<kind))?get(r+0x50+kind*16+j*4):0);
            }
        }
        writeFile(output,result.data(),result.size()*4);
    }
    void eventWrite(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        if(ram.size()!=32*1024*1024 || input.empty() || input.size()%8 || input.size()>1024)throw std::runtime_error("Event write fixture bounds");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                        ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        std::memset(ee->m_ram+0xa00000,0xa5,0x1000);
        std::memset(ee->m_ram+0xb00000,0x5a,0x800);
        put(0x24f788,0xa00000);put(0x2d9bf4,0xb00000);
        std::vector<uint8> result;
        for(unsigned at=0;at<input.size();at+=8) {
            uint32 selector,value;std::memcpy(&selector,input.data()+at,4);std::memcpy(&value,input.data()+at+4,4);
            const auto bank=selector>>16,index=selector&0xffff;
            if(bank>4 || (bank<2&&index>=0x800) || (bank==2&&index>=0x200) ||
               (bank==3&&index>=0x100) || (bank==4&&(index>=0x200||index==6)))throw std::runtime_error("Unsupported event write fixture");
            put(0x900000,selector);
            // Original 0x47 handler, including its logging call, no patches.
            call(0x130270,{0,0,value,0x900000});
            const auto next=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0)-0x900000;
            const auto* p=reinterpret_cast<const uint8*>(&next);result.insert(result.end(),p,p+4);
            result.insert(result.end(),ee->m_ram+0xa00000,ee->m_ram+0xa01000);
            for(unsigned i=0;i<0x800;++i)if(ee->m_ram[0xb00000+i]!=0x5a)throw std::runtime_error("Unexpected field-state mutation");
        }
        writeFile(output,result.data(),result.size());
    }
    void gameNewStats(const std::vector<uint8>& bkdata,const std::vector<uint8>& items,
                      const std::vector<uint8>& input,const char* output) {
        // Research-only owned tables and synthetic storage. Execute the actual
        // 0x1251d8 callees without its unrelated diagnostic logger; no patches.
        constexpr uint32 rowSize=0x1020,root=0xa00000,bk=0xb00000,item=0xc00000;
        if(input.empty()||input.size()%rowSize||input.size()>16*rowSize)
            throw std::runtime_error("New-game stats fixture bounds");
        if(hash(bkdata)!="973964019ddb24d033b0e10f4f3e2cf72fafdee9bdb25ee143ee87c832b0aff4"||
           hash(items)!="d3c0af83a4f47a8e5960f9181bdd72649692b80dafeb387395307130804c7cce")
            throw std::runtime_error("New-game stats requires pinned owned BKDATA/ITEM tables");
        // 0x178a60 simply loads current slot from 0x31fa60. Use bounded slot0,
        // not an OS/thread stub, and restore every affected pointer/cache global.
        const uint32 globals[]={0x24f788,0x24f78c,0x24f790,0x31fa60,
                                0x1faf88,0x24f758,0x24f760,0x24f768,0x24f770,0x24f778};
        std::vector<uint32> previous;
        for(auto address:globals)previous.push_back(get(address));
        auto restore=[&] {for(unsigned i=0;i<previous.size();++i)put(globals[i],previous[i]);};
        std::vector<uint8> result;
        try {
            std::memset(ee->m_ram+bk-16,0x69,bkdata.size()+32);
            std::memcpy(ee->m_ram+bk,bkdata.data(),bkdata.size());
            std::memset(ee->m_ram+item-16,0x96,items.size()+32);
            std::memcpy(ee->m_ram+item,items.data(),items.size());
            put(0x24f788,root);put(0x24f78c,bk);put(0x24f790,item);put(0x31fa60,0);
            call(0x178a60,{});
            if(ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0]!=0)
                throw std::runtime_error("Unexpected original XDB context slot");
            auto lookup=[&](uint32 address,uint32 size,uint32 name,uint32 recordBytes,uint32 required) {
                call(0x1249c8,{address});
                if(ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0]!=0)
                    throw std::runtime_error("Original XDB selection failed");
                call(0x124b58,{name});
                const auto records=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];
                const uint64 bytes=uint64(get(0x24f770))*4;
                if(records<address||uint64(records)+bytes>uint64(address)+size||
                   bytes<uint64(recordBytes)*required||bytes%recordBytes)
                    throw std::runtime_error("Original XDB record bounds");
                call(0x124ca8,{0});
                const auto strings=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];
                if(strings<address||uint64(strings)>=uint64(address)+size)
                    throw std::runtime_error("Original XDB string bounds");
            };
            // 0x125400 selects "unit" through BKDATA; 0x125360 selects
            // "item" through ITEM. Original reset uses unit IDs170..176.
            lookup(bk,uint32(bkdata.size()),0x223898,0x6c,176);
            lookup(item,uint32(items.size()),0x223890,0x3c,240);
            auto tablesUnchanged=[&] {
                if(std::memcmp(ee->m_ram+bk,bkdata.data(),bkdata.size())||
                   std::memcmp(ee->m_ram+item,items.data(),items.size()))
                    throw std::runtime_error("New-game stats mutated owned table data");
                for(unsigned i=0;i<16;++i)
                    if(ee->m_ram[bk-16+i]!=0x69||ee->m_ram[bk+bkdata.size()+i]!=0x69||
                       ee->m_ram[item-16+i]!=0x96||ee->m_ram[item+items.size()+i]!=0x96)
                        throw std::runtime_error("New-game stats crossed table canary");
            };
            for(unsigned at=0;at<input.size();at+=rowSize) {
                std::memcpy(ee->m_ram+root-16,input.data()+at,rowSize);
                auto snapshot=[&] {
                    if(get(0x24f788)!=root||get(0x24f78c)!=bk||get(0x24f790)!=item||get(0x31fa60)!=0)
                        throw std::runtime_error("New-game stats changed owner/context pointer");
                    if(std::memcmp(ee->m_ram+root-16,input.data()+at,16)||
                       std::memcmp(ee->m_ram+root+0x1000,input.data()+at+0x1010,16))
                        throw std::runtime_error("New-game stats crossed shared-variable canary");
                    tablesUnchanged();
                    result.insert(result.end(),ee->m_ram+root-16,ee->m_ram+root-16+rowSize);
                };
                call(0x124f10,{});snapshot();
                call(0x126370,{});snapshot();
                call(0x125e10,{0,131,174,240});snapshot();
            }
        }catch(...) {restore();throw;}
        restore();
        writeFile(output,result.data(),result.size());
    }
    void gameVariablesReset(const std::vector<uint8>& input,const char* output) {
        constexpr unsigned rowSize=0x1020,root=0xa00000;
        if(input.empty()||input.size()%rowSize||input.size()>16*rowSize)
            throw std::runtime_error("Game variable reset fixture bounds");
        std::vector<uint8> result;
        for(unsigned at=0;at<input.size();at+=rowSize) {
            std::memcpy(ee->m_ram+root-16,input.data()+at,rowSize);
            put(0x24f788,root);
            call(0x124f10,{});
            uint32 pointer;std::memcpy(&pointer,ee->m_ram+0x24f788,4);
            if(pointer!=root)throw std::runtime_error("Game variable reset changed storage pointer");
            result.insert(result.end(),ee->m_ram+root-16,ee->m_ram+root-16+rowSize);
        }
        writeFile(output,result.data(),result.size());
    }
    void battleStateInit(const std::vector<uint8>& input,const char* output) {
        // Only original 0x1511d8 / 0x151798; never the resource/scheduler
        // constructor 0x149fd8. No game code or default-global replacement.
        constexpr unsigned root=0xa00000,rootSize=0x81d0,guardedSize=rootSize+64,
                           recordSize=16+guardedSize;
        if(input.empty()||input.size()%recordSize||input.size()>recordSize*16)
            throw std::runtime_error("Battle initialization fixture extent");
        std::vector<uint8> result;
        for(unsigned at=0;at<input.size();at+=recordSize) {
            if(std::memcmp(input.data()+at,ee->m_ram+0x236f70,16))
                throw std::runtime_error("Battle fixture defaults differ from original ELF global");
            std::memcpy(ee->m_ram+root-32,input.data()+at+16,guardedSize);
            put(0x1fce60,root);
            auto snapshot=[&] {
                if(get(0x1fce60)!=root||std::memcmp(input.data()+at,ee->m_ram+0x236f70,16))
                    throw std::runtime_error("Unexpected battle initializer global mutation");
                if(std::memcmp(ee->m_ram+root-32,input.data()+at+16,32)||
                   std::memcmp(ee->m_ram+root+rootSize,input.data()+at+48+rootSize,32))
                    throw std::runtime_error("Battle initializer crossed root canary");
                result.insert(result.end(),ee->m_ram+root-32,ee->m_ram+root+rootSize+32);
            };
            call(0x1511d8,{root+0x50});snapshot();
            call(0x151798,{root+0x80a4});snapshot();
        }
        writeFile(output,result.data(),result.size());
    }
    void eventShorts(const std::vector<uint8>& input,const char* output) {
        if(input.empty()||input.size()%12||input.size()>12*1024)
            throw std::runtime_error("Event short fixture bounds");
        std::memset(ee->m_ram+0xa00000,0xa5,0x1000);put(0x24f788,0xa00000);
        std::vector<uint8> result;
        for(unsigned at=0;at<input.size();at+=12) {
            uint32 row[3];std::memcpy(row,input.data()+at,12);
            const auto op=row[0],index=row[1]&0xffff,bank=row[1]>>16;
            if(op>2||bank<8||bank>15||index>=40)
                throw std::runtime_error("Unsupported event short fixture");
            if(op)call(op==1?0x125918:0x1259d0,{index,bank,row[2]});
            call(0x125888,{index,bank});
            const auto value=ee->m_EE.m_State.nGPR[CMIPS::V0].nV[0];
            for(unsigned i=0;i<4;++i)result.push_back(value>>(8*i));
            result.insert(result.end(),ee->m_ram+0xa00000,ee->m_ram+0xa01000);
        }
        writeFile(output,result.data(),result.size());
    }
    void eventSetup(const std::vector<uint8>& script,const std::vector<uint8>& variables,
                    const std::vector<uint8>& state,const char* output,bool strings=false) {
        if(script.size()<36 || script.size()>0x100000 || variables.size()!=0x1000 || state.size()!=0x800)
            throw std::runtime_error("Event setup fixture bounds");
        std::memcpy(ee->m_ram+0x900000,script.data(),script.size());
        std::memcpy(ee->m_ram+0xa00000,variables.data(),variables.size());
        std::memcpy(ee->m_ram+0xb00000,state.data(),state.size());
        std::memset(ee->m_ram+0x800000,0,192*24);
        put(0x337614,0x800000);put(0x24f788,0xa00000);put(0x2d9bf4,0xb00000);
        put(0x33793c,0x130378);put(0x212ef0+(0x50-0x20)*4,0x130bb8);
        put(0x212ef0+(0x59-0x20)*4,0x131048);
        if(strings){put(0x1c7910,0x03e00008);put(0x1c7914,0);}
        // Inputs are restricted by the caller to the recovered setup operations.
        call(0x1b5bd8,{0,0x900000});
        if(get(0x800000)&0x100)throw std::runtime_error("Event setup did not terminate");
        writeFile(output,ee->m_ram+0xb00000,state.size());
    }
    void mapTransforms(const std::vector<uint8>& bytes,const char* output) {
        // Independent bounded reader: do not reuse the native XMP parser under test.
        if(bytes.size()<64 || bytes.size()>4*1024*1024) throw std::runtime_error("Map oracle input bounds");
        auto word=[&](size_t at){uint32 v;if(at>bytes.size()-4)throw std::runtime_error("Map oracle field bounds");
            std::memcpy(&v,bytes.data()+at,4);return v;};
        const uint32 objects=word(8),count=word(12),defs=word(40),defCount=word(44);
        if(defCount>640 || uint64(objects)+uint64(count)*80>bytes.size() || uint64(defs)+uint64(defCount)*148>bytes.size())
            throw std::runtime_error("Map oracle table bounds");
        std::memcpy(ee->m_ram+0x900000,bytes.data(),bytes.size());
        constexpr uint32 fragment=0x700000,size=0x1bbe40-0x1bbd28;
        std::memcpy(ee->m_ram+fragment,ee->m_ram+0x1bbd28,size);
        put(fragment+size,0x08000000|(stop>>2));put(fragment+size+4,0);
        std::vector<uint8> result;
        for(uint32 i=0;i<count;++i) {
            if(word(objects+i*80+8)>=defCount) throw std::runtime_error("Map oracle definition bounds");
            put(0x1ff1490,0x900000+defs);put(0x1ff1494,0x800000);
            std::memset(ee->m_ram+0x800000,0,640*4);
            auto& cpu=ee->m_EE.m_State;
            cpu.nGPR[CMIPS::S2].nD0=0x900000+objects+i*80;
            cpu.nGPR[CMIPS::S7].nD0=0;
            cpu.nHI[0]=cpu.nHI[1]=0;
            call(fragment);
            result.insert(result.end(),ee->m_ram+0x1ff0040,ee->m_ram+0x1ff0070);
            const auto flags=uint32(cpu.nGPR[CMIPS::A1].nD0);
            for(unsigned j=0;j<4;++j)result.push_back(flags>>(j*8));
        }
        writeFile(output,result.data(),result.size());
    }
    void instanceMatrices(const std::vector<uint8>& bytes,const char* output,bool camera=false) {
        if(bytes.empty() || bytes.size()%48 || bytes.size()>48*65536)throw std::runtime_error("Invalid instance channel fixture");
        call(0x1b1d88);loadMicro();
        constexpr uint32 fragment=0x700000,size=0x1ba140-0x1b9ff8;
        std::memcpy(ee->m_ram+fragment,ee->m_ram+0x1b9ff8,size);
        put(fragment+size,0x08000000|(stop>>2));put(fragment+size+4,0);
        std::vector<uint8> result;
        for(size_t at=0;at<bytes.size();at+=48) {
            if(camera) {
                std::memcpy(ee->m_ram+0x3321f0,bytes.data()+at,48);put(0x3323a0,1);
                call(0x183990);
                result.insert(result.end(),ee->m_ram+0x3322a0,ee->m_ram+0x3322e0);continue;
            }
            std::memcpy(ee->m_ram+0x800000,bytes.data()+at,48);
            std::memcpy(ee->m_ram+0x1ff0060,bytes.data()+at+16,16);
            auto& cpu=ee->m_EE.m_State;cpu.nGPR[CMIPS::S1].nD0=0x800000;cpu.nGPR[CMIPS::S6].nD0=0x800020;
            call(fragment);
            result.insert(result.end(),ee->m_ram+0x1ff0070,ee->m_ram+0x1ff00b0);
        }
        writeFile(output,result.data(),result.size());
    }
    struct TextureFixture {
        std::vector<uint32> packets;
        std::vector<uint64> tex0;
    };
    TextureFixture textureFixture(const char* modelPath,const char* xtaPath) {
        put(0x1fe2ac,0x820000);
        const auto model=okage::read_xmd(readFile(modelPath));
        const auto text=readFile(xtaPath);
        const auto xta=okage::read_xta(std::string_view(reinterpret_cast<const char*>(text.data()),text.size()));
        if (model.models.size()!=1 || model.models[0].blocks.size()>128) throw std::runtime_error("Texture fixture model bounds");
        TextureFixture result;
        auto names=model.textures; names.insert(names.end(),xta.variants.begin(),xta.variants.end());
        if (names.size()>512 || xta.groups.size()>8) throw std::runtime_error("Texture fixture table bounds");
        put(0x3a7b00,0x8c0000);
        for (size_t i=0;i<names.size();++i) {
            auto name=names[i]; if (std::filesystem::path(name).extension().empty()) name+=".xim";
            const auto path=std::filesystem::path(modelPath).parent_path()/name;
            const auto image=okage::decode_xim(readFile(path.c_str()));
            // Real XIM dimensions/formats; isolated deterministic VRAM addresses.
            const uint64 imageInfo=uint64(0x100+i*16)|(uint64((image.width+63)/64)<<14)|
                (uint64(image.pixel_format|(image.width<<6)|(image.height<<18))<<32);
            const uint64 paletteInfo=uint64(0x3000+i*4)|(uint64(image.palette_format)<<32);
            uint16 index=i; std::memcpy(ee->m_ram+0x3a5b00+i*2,&index,2);
            put64(0x8c0000+i*48,imageInfo); put64(0x8c0008+i*48,paletteInfo);
            result.tex0.push_back((imageInfo&0x3fff)|((imageInfo>>14&63)<<14)|
                ((imageInfo>>32&63)<<20)|((paletteInfo&0x3fff)<<37)|
                ((paletteInfo>>32&63)<<51)|0x20000006a8000000ULL);
        }
        put(0x820008,0x860000); put(0x860000,model.models[0].blocks.size());
        size_t state=0;
        for (size_t b=0;b<model.models[0].blocks.size();++b) {
            const auto& block=model.models[0].blocks[b];
            const uint32 outer=0x861000+b*32;
            put(0x860004+b*4,outer);
            put(outer,7|(uint32(block.textures.size())<<16)); put(outer+12,0x870000+state*16); put(outer+16,1);
            for (const auto& texture:block.textures) {
                if (state>=240 || texture.texture<0 || size_t(texture.texture)>=model.textures.size() || texture.palette>0)
                    throw std::runtime_error("Unvalidated texture fixture state");
                const uint32 marker=0x870000+state*16, packet=0x880000+state*64;
                put(marker,0x0001000d); put(marker+4,uint16(texture.texture)); put(marker+8,packet);
                // Three register records, with TEX0 between two sentinel records.
                // This is the recovered prepared-record layout, not a GS submission.
                put(packet+4,3); put64(packet+16,0x1122334455667788ULL); put(packet+24,8);
                put64(packet+28,result.tex0.at(texture.texture)); put(packet+36,6);
                put64(packet+40,0xaabbccddeeff0011ULL); put(packet+48,0x42);
                result.packets.push_back(packet); ++state;
            }
        }
        put(0x213018,0x8f0000); put(0x8f0008,model.textures.size());
        put(0x8f000c,xta.groups.size()); put(0x8f0010,0x8f0100);
        for (size_t group=0;group<xta.groups.size();++group) {
            const uint32 list=0x8e0000+group*0x1000;
            put(0x8f0100+group*4,list); put(0x337b54,list);
            call(0x1b72a8,{0,uint32(xta.groups[group])});
            if (get(0x337b54)>list+0x1000) throw std::runtime_error("Texture fixture patch list overflow");
        }
        call(0x1b7e10); // Install the original callback, including opcode 0x34.
        return result;
    }
    void textureCallbacks(const char* modelPath,const char* xtaPath,const char* output) {
        const auto fixture=textureFixture(modelPath,xtaPath);
        const auto model=okage::read_xmd(readFile(modelPath));
        const auto variants=fixture.tex0.size()-model.textures.size();
        std::vector<int32> dump;
        for (uint32 group=0;group<get(0x8f000c);++group) for (uint32 variant=0;variant<=variants;++variant) {
            call(0x1b7528,{0}); call(0x1b7610,{0,group,variant});
            const auto selected=textureSelection(fixture);
            dump.push_back(group); dump.push_back(variant); dump.insert(dump.end(),selected.begin(),selected.end());
        }
        writeFile(output,dump.data(),dump.size()*4);
        std::cout<<"Validated original texture callback records and sentinel registers\n";
    }
    std::vector<int32> textureSelection(const TextureFixture& fixture) const {
        std::vector<int32> result;
        for (auto packet:fixture.packets) {
            if (get64(packet+16)!=0x1122334455667788ULL || get(packet+24)!=8 || get(packet+36)!=6 ||
                get64(packet+40)!=0xaabbccddeeff0011ULL || get(packet+48)!=0x42)
                throw std::runtime_error("Texture callback altered unrelated register data");
            const auto value=get64(packet+28);
            const auto found=std::find(fixture.tex0.begin(),fixture.tex0.end(),value);
            if (found==fixture.tex0.end()) throw std::runtime_error("Texture callback produced unexpected TEX0 bits");
            result.push_back(std::distance(fixture.tex0.begin(),found));
        }
        return result;
    }
    void loadMicro() {
        std::memcpy(ee->m_microMem0,ee->m_ram+0x21ab00,0x660);
        put(0x01ff0000,stop); put(0x01ff0004,0);
        call(0x1b1eb4);
    }
    void uv(const std::vector<uint8>& cases,const char* output,bool clamp=false) {
        // Each fixture: width/height u32, U/V mode i32, region float4, UV float2.
        if (cases.empty() || cases.size()%40 || cases.size()>40*65536)
            throw std::runtime_error("Invalid UV fixture records");
        put(0x3301a4,0x800000); put(0x330144,0x810000);
        put(0x81000c,1); put(0x810010,0);
        put(0x3a5b00,0); put(0x3a7b00,0x820000);
        put(0x3309b0,1); put(0x3309b8,0);
        // Existing UV packet context: original state selector can reuse it.
        put(0x3321e0,clamp ? 13 : 10); put(0x3321e8,0); put(0x3321ec,0);
        put(0x900000,(8U<<24)|20); put(0x900004,0); put(0x900008,1<<16);
        std::vector<uint8> result;
        for (size_t at=0;at<cases.size();at+=40) {
            uint32 fields[4]; std::memcpy(fields,cases.data()+at,16);
            if (!fields[0] || !fields[1] || fields[0]>4095 || fields[1]>4095 ||
                int32(fields[2])<-32768 || int32(fields[2])>32767 ||
                int32(fields[3])<-32768 || int32(fields[3])>32767)
                throw std::runtime_error("UV fixture fields out of range");
            put64(0x820000,uint64(19|(fields[0]<<6)|(fields[1]<<18))<<32);
            put(0x800000,0); put(0x800004,(fields[2]&0xffff)|(fields[3]<<16));
            std::memcpy(ee->m_ram+0x800008,cases.data()+at+16,16);
            std::memcpy(ee->m_ram+0x90000c,cases.data()+at+32,8);
            put(0x2138b4,0x1800010); put(0x333bf8,1); put(0x330164,0);
            std::memset(ee->m_ram+0x1800000,0xcd,48);
            if (clamp) {
                call(0x1af978); // Original register accumulator initialization.
                call(0x17be50,{0});
                if (get(0x202dd8)!=2 || get64(0x333c10)!=6 || get64(0x333c20)!=8)
                    throw std::runtime_error("Original texture register packet mismatch");
                result.insert(result.end(),ee->m_ram+0x333c28,ee->m_ram+0x333c30);
                continue;
            }
            call(0x17e8e0,{0x900000});
            if (get(0x2138b4)!=0x180001c || get(0x1800010)!=0x740101aa || get(0x330164)!=1)
                throw std::runtime_error("Original UV packet contract mismatch");
            for (uint32 i=0;i<48;++i)
                if ((i<16 || i>=28) && ee->m_ram[0x1800000+i]!=0xcd)
                    throw std::runtime_error("Original UV output exceeded fixture");
            result.insert(result.end(),ee->m_ram+0x1800014,ee->m_ram+0x180001c);
        }
        writeFile(output,result.data(),result.size());
        std::cout<<"Executed original "<<(clamp ? "texture clamp" : "UV conversion")<<" for "<<cases.size()/40<<" records\n";
    }
    void instanceAlpha(const std::vector<uint8>& cases,const char* output) {
        if(cases.empty()||cases.size()%8||cases.size()>8*65536)
            throw std::runtime_error("Instance alpha fixture extent");
        std::memcpy(ee->m_microMem1,ee->m_ram+0x215900,0x3d98);
        auto& cpu=ee->m_VU1;
        auto vector=[&](unsigned at,std::array<float,4> value) {
            std::memcpy(ee->m_vuMem1+at*16,value.data(),16);
        };
        auto run=[&](unsigned entry) {
            put(0x1900000,0x0400000a);put(0x1900004,0x14000000|entry);
            put(0x1900008,0);put(0x190000c,0);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||cpu.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Instance alpha VU entry did not complete");
        };
        std::vector<uint8> result;
        for(size_t at=0;at<cases.size();at+=8) {
            float source,ambient;std::memcpy(&source,cases.data()+at,4);std::memcpy(&ambient,cases.data()+at+4,4);
            run(0);
            vector(0,{.4f,.6f,1,0});vector(1,{.2f,.3f,.4f,0});
            vector(2,{.1f,.2f,.3f,0});vector(3,{1,1,1,ambient});run(0x4f);
            vector(0xd,{1,0,0,0});vector(0xe,{0,1,0,0});vector(0xf,{0,0,1,0});
            for(unsigned i=0;i<5;++i)vector(0xe8+i,{0,0,1,0});
            const uint32 lightHeader[]={3,0,0,0};std::memcpy(ee->m_vuMem1,lightHeader,16);
            cpu.m_State.nCOP2VI[11]=0;run(0x280);
            result.insert(result.end(),ee->m_vuMem1+0x20b*16+12,ee->m_vuMem1+0x20b*16+16);
            const uint32 header[]={3,0x30004000|(0x4dU<<15)|0x4dU,0x41f,0};
            std::memcpy(ee->m_vuMem1,header,16);
            for(unsigned i=0;i<3;++i) {
                const uint32 corner[]={i,i,i,i};std::memcpy(ee->m_vuMem1+(i+1)*16,corner,16);
                vector(0x87+i,{i==0?16.f:80.f,i==2?96.f:32.f,.25f,1});
                vector(0x149+i,{128,128,128,source});
            }
            cpu.m_State.nCOP2VI[12]=0x3ff;cpu.m_State.nCOP2VI[13]=0x300;
            cpu.m_State.nCOP2VI[14]=0;run(0x3b6);
            if(cpu.m_State.nCOP2VI[13]!=0x30a)throw std::runtime_error("Instance alpha triangle missing");
            result.insert(result.end(),ee->m_vuMem1+0x302*16+12,ee->m_vuMem1+0x302*16+16);
        }
        writeFile(output,result.data(),result.size());
        std::cout<<"Original VU1 instance alpha records: "<<cases.size()/8<<'\n';
    }
    void mapPacket(const std::vector<uint8>& ram,const std::vector<uint8>& asset,const char* output) {
        constexpr uint32 instance=0x5a0258,descriptor=0x3c53a0;
        uint32 list=0xb83ab0,source=0xb83440,sourceQwc=94,selectedBlock=0;
        constexpr uint32 packetArena=0x1b00000,poseArena=0x1c00000,fixture=0x1e00000,arenaSize=0x10000;
        constexpr uint32 world=fixture+0x40,sp=0x1ff0000,worldEnd=0x1ba178;
        const auto require=[](bool yes,const char* what){if(!yes)throw std::runtime_error(what);};
        require(ram.size()==32*1024*1024&&hash(ram)=="74131100a37fc00572db7eb4e3f6dcdd7e5e8f75512a2c1d3b84ab0805513cc5",
            "Map packet requires the provenance-pinned jam-talk RAM fixture");
        require(!std::filesystem::exists(output)&&!std::filesystem::is_symlink(output),"Map packet requires fresh output");
        const auto model=okage::read_xmd(asset);
        require(model.models.size()==1&&model.models[0].blocks.size()==59,"Map packet source model extent");
        // Before restoring runtime data, retain the pinned executable/microcode.
        const std::array<std::pair<uint32,uint32>,8> codeRanges={{{0x184bd8,0x160},{0x184d38,0x338},
            {0x186618,0x1a8},{0x185fb0,0x668},{0x1b9ff8,worldEnd-0x1b9ff8},{0x1ad1f8,0x70},{0x215900,0x3d98},{0x21ab00,0x660}}};
        std::vector<std::vector<uint8>> code;
        for(auto [a,n]:codeRanges)code.emplace_back(ee->m_ram+a,ee->m_ram+a+n);
        std::memcpy(ee->m_ram,ram.data(),ram.size());
        for(unsigned i=0;i<code.size();++i)require(!std::memcmp(ee->m_ram+codeRanges[i].first,code[i].data(),code[i].size()),
            "Retained map executable differs from pinned ELF");
        require(get(0x21301c)==0x5a01e0&&get(0x1fe2ac)==0x3c52d0&&get(instance)>>16==4,
            "Retained map instance/model binding changed");
        require(get(descriptor+8)==0xb83230&&get(descriptor+24)==0x184d38&&get(0xb83234)==list,
            "Retained map descriptor/list changed");
        require(get(list)==2&&get(list+4)==1&&get(list+16)==0x005e0007&&get(list+24)==source&&get(list+32)==1,
            "Retained map compiled block changed");
        const std::vector<uint8> viewBefore(ee->m_ram+0x3322a0,ee->m_ram+0x3322e0);
        require(get(0x337e1c)!=0x83,"Map world block requires ordinary stage path");
        callIterationLimit=10000;
        put(stop,0x08000000|(stop>>2));put(stop+4,0);loadMicro();
        std::memcpy(ee->m_microMem1,ee->m_ram+0x215900,0x3d98);
        auto sink=std::make_unique<MapPacketGs>();auto* gs=sink.get();
        researchGs=std::move(sink);researchGs->Initialize();researchGs->Reset();ee->m_gs=researchGs.get();gs->begin();
        auto& cpu=ee->m_EE;
        const auto channel=get(instance+4)&0xffff;
        call(0x1b43c0,{channel,fixture});call(0x1b43c0,{channel+1,fixture+16});call(0x1b43c0,{channel+2,fixture+32});
        std::memcpy(ee->m_ram+sp+0x60,ee->m_ram+fixture,16);put(fixture+28,0x3f800000);
        cpu.m_State.nGPR[CMIPS::S1].nD0=fixture+16;cpu.m_State.nGPR[CMIPS::S6].nD0=fixture+32;
        cpu.m_State.nGPR[CMIPS::S2].nD0=instance;
        cpu.ToggleBreakpoint(worldEnd);expectedStop=worldEnd;
        try {call(0x1b9ff8);}catch(...){cpu.ToggleBreakpoint(worldEnd);expectedStop=stop;throw;}
        cpu.ToggleBreakpoint(worldEnd);expectedStop=stop;
        std::memcpy(ee->m_ram+world,ee->m_ram+sp+0x70,64);
        std::memset(ee->m_ram+poseArena,0xcd,arenaSize);
        call(0x1caa20,{poseArena});
        std::vector<std::array<uint32,7>> selections;
        cpu.ToggleBreakpoint(0x184d38);
        boundedInstructionProbe=[&](uint32 pc){
            if(pc!=0x184d38)return false;
            require(selections.size()<59,"Map callback selection bound");
            std::array<uint32,7> args{};
            for(unsigned i=0;i<args.size();++i)args[i]=cpu.m_State.nGPR[CMIPS::A0+i].nV[0];
            selections.push_back(args);return true;
        };
        try {call(0x185fb0,{4,0,world,0,0,0,0});}
        catch(...){cpu.ToggleBreakpoint(0x184d38);boundedInstructionProbe={};throw;}
        cpu.ToggleBreakpoint(0x184d38);boundedInstructionProbe={};
        const auto batch=cpu.m_State.nGPR[CMIPS::V0].nV[0],poseEnd=get(0x3a0aa0);
        require(!selections.empty(),"Original map selected no blocks");
        struct Source {uint32 block,list,address,qwc;std::vector<uint8> before,listBefore;};
        std::vector<Source> sources;
        const auto poses=selections.front()[3],nodes=get(get(descriptor+4));
        require(nodes==60&&poses>=poseArena&&poses+nodes*64<poseArena+arenaSize-16&&
            poseEnd<poseArena+arenaSize-16,"Map pose/returned packet output bounds");
        for(const auto& args:selections){
            const auto index=args[1],entry=args[2];
            require(args[0]==4&&index<59&&entry==get(get(descriptor+8)+4+index*4),
                "Original map selection is not a retained opaque block");
            require(args[3]==poses&&args[4]==0&&args[5]==0&&args[6]==0,"Map selected callback optional/pose state");
            require(entry<ram.size()-48&&get(entry)==2&&(get(entry+16)&0xffff)==7&&get(entry+32)==1,
                "Selected map list is not a single matrix/source batch");
            Source item{index,entry,get(entry+24),get(entry+16)>>16,{},{}};
            if(!(item.qwc>0&&item.qwc<=8192&&uint64(item.address)+item.qwc*16<ram.size()))
                throw std::runtime_error("Selected map source extent: block="+std::to_string(index)+" qwc="+std::to_string(item.qwc));
            item.before.assign(ee->m_ram+item.address,ee->m_ram+item.address+item.qwc*16);
            item.listBefore.assign(ee->m_ram+entry,ee->m_ram+entry+48);
            for(const auto& polygon:model.models[0].blocks[index].polygons)for(const auto& corner:polygon.corners)
                require(corner[1]==-1,"Selected map source unexpectedly has directional normals");
            sources.push_back(std::move(item));
        }
        const auto selectionArgs=selections.front();
        selectedBlock=selectionArgs[1];list=selectionArgs[2];source=sources.front().address;sourceQwc=sources.front().qwc;
        const auto& sourceBefore=sources.front().before;
        const auto& block=model.models[0].blocks[selectedBlock];
        // Explicit harness allocation arena, not replacement lighting/geometry.
        std::memset(ee->m_ram+packetArena,0xcd,arenaSize);put(0x3a0aa4,0);call(0x1caa20,{packetArena});
        call(0x186618);const auto preamble=cpu.m_State.nGPR[CMIPS::V0].nV[0];
        call(0x186790);const auto footer=cpu.m_State.nGPR[CMIPS::V0].nV[0],arenaEnd=get(0x3a0aa0);
        require(arenaEnd>=packetArena&&arenaEnd<packetArena+arenaSize-16,"Map generated packet arena bounds");
        for(unsigned i=0;i<16;++i)require(ee->m_ram[packetArena+arenaSize-16+i]==0xcd&&
            ee->m_ram[poseArena+arenaSize-16+i]==0xcd,"Map packet/pose arena canary");
        require(!std::memcmp(viewBefore.data(),ee->m_ram+0x3322a0,64),"Map helper changed retained view matrix");
        std::ostringstream report;report<<"{\n  \"schema_version\":1,\n  \"scope\":\"Original retained model4 exact185fb0 selected callback order and returned chain; preamble/world/pose/list/footer/VIF, no GS raster proof\",\n";
        report<<"  \"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\",\n";
        report<<"  \"ram_sha256\":\""<<hash(ram)<<"\",\n  \"source_xmd_sha256\":\""<<hash(asset)
              <<"\",\n  \"source_packet_sha256\":\""<<hash(sourceBefore)<<"\",\n";
        auto words=[&](const void* p,unsigned bytes){require(bytes%4==0,"Map report word extent");report<<'[';
            for(unsigned i=0;i<bytes;i+=4){uint32 w;std::memcpy(&w,static_cast<const uint8*>(p)+i,4);report<<(i?",":"")<<w;}report<<']';};
        auto ramWords=[&](const char* name,uint32 a,unsigned bytes){report<<"  \""<<name<<"\":";words(ee->m_ram+a,bytes);report<<",\n";};
        report<<"  \"selected_sources\":[";
        for(unsigned i=0;i<sources.size();++i){const auto& item=sources[i];report<<(i?",":"")<<"{\"callback_operands\":";
            words(selections[i].data(),28);report<<",\"list_words\":";words(item.listBefore.data(),48);
            report<<",\"payload_address\":"<<item.address<<",\"payload_words\":";words(item.before.data(),item.before.size());
            report<<",\"payload_sha256\":\""<<hash(item.before)<<"\",\"native_source_corners\":[";
            bool sep=false;const auto& polygons=model.models[0].blocks[item.block].polygons;
            for(unsigned p=0;p<polygons.size();++p){for(unsigned c=0;c<polygons[p].corners.size();++c){
                report<<(sep?",":"")<<'['<<p<<','<<c;for(auto index:polygons[p].corners[c])report<<','<<index;
                report<<']';sep=true;
            }}report<<"]}";
        }report<<"],\n";
        report<<"  \"selected_block\":"<<selectedBlock<<",\n  \"original_callback_operands\":";words(selectionArgs.data(),selectionArgs.size()*4);report<<",\n";
        ramWords("instance_words",instance,60);ramWords("descriptor_words",descriptor,52);ramWords("channel_words",fixture,48);
        ramWords("world_matrix_words",world,64);ramWords("view_matrix_words",0x3322a0,64);ramWords("pose_matrix_words",poses,nodes*64);
        ramWords("light_state_words",0x3322e0,0x150);ramWords("compiled_list_words",list,48);ramWords("compiled_payload_words",source,sourceQwc*16);
        report<<"  \"native_source_corners\":[";bool comma=false;
        for(unsigned p=0;p<block.polygons.size();++p){
          for(unsigned c=0;c<block.polygons[p].corners.size();++c){
            const auto& corner=block.polygons[p].corners[c];report<<(comma?",":"")<<'['<<p<<','<<c;
            for(auto index:corner)report<<','<<index;
            report<<']';comma=true;
          }
        }
        report<<"],\n  \"dma_segments\":[";
        struct Raw {unsigned segment,index,address;int sourceBlock;std::vector<uint32> words;};std::vector<Raw> raw;
        struct Cache {unsigned segment,block;std::vector<uint8> words;};std::vector<Cache> caches;
        int activeSourceBlock=-1;
        unsigned segmentCount=0,observedPackets=0,totalQwords=0;
        auto capture=[&]{
            const auto& packets=gs->packets();require(packets.size()<=4096,"Map GS packet observation limit");
            for(;observedPackets<packets.size();++observedPackets){
                const auto& metadata=packets[observedPackets].metadata;
                require(metadata.pathIndex==1,"Map observer expected VU XGKICK path1");
                Raw r{segmentCount,observedPackets,metadata.vuMemPacketAddress,activeSourceBlock,{}};
                require(r.address<0x4000&&r.address%16==0,"Map GIF packet address");
                const uint8* kickMemory=observedPackets+1<packets.size()?
                    packets[observedPackets+1].metadata.vuMem1:ee->m_vuMem1;
                unsigned a=r.address;bool end=false;
                for(unsigned tags=0;tags<128&&!end;++tags){
                    uint64 tag;std::memcpy(&tag,kickMemory+a,8);const auto loops=tag&0x7fff,mode=tag>>58&3;
                    unsigned regs=tag>>60;if(!regs)regs=16;
                    require(mode<=1,"Map raw GIF observer encountered unsupported image/disabled tag");
                    const unsigned qwc=1+(mode==0?loops*regs:(loops*regs+1)/2);
                    require(qwc<=1024&&r.words.size()+qwc*4<=4096,"Map raw GIF packet extent");
                    for(unsigned q=0;q<qwc;++q)for(unsigned j=0;j<4;++j){uint32 word;
                        std::memcpy(&word,kickMemory+((a+q*16+j*4)&0x3fff),4);r.words.push_back(word);}
                    a=(a+qwc*16)&0x3fff;end=(tag&0x8000)!=0;
                }
                require(end,"Map raw GIF packet missing EOP");
                std::vector<CGSHandler::RegisterWrite> decoded;
                for(unsigned at=0;at<r.words.size();){
                    const auto* t=r.words.data()+at;
                    const uint64 tag=uint64(t[0])|(uint64(t[1])<<32), desc=uint64(t[2])|(uint64(t[3])<<32);
                    const unsigned loops=tag&0x7fff,mode=tag>>58&3;
                    unsigned regs=tag>>60;if(!regs)regs=16;
                    at+=4;uint32 q=0x3f800000;
                    if(mode==0&&(tag&(uint64(1)<<46)))decoded.emplace_back(0,(tag>>47)&0x7ff);
                    for(unsigned n=0;n<loops*regs;++n){
                        unsigned reg=(desc>>((n%regs)*4))&15;
                        const auto* w=r.words.data()+at;
                        uint64 value=uint64(w[0])|(uint64(w[1])<<32);
                        if(mode==0){
                            switch(reg){
                            case 0:value=w[0];break;
                            case 1:value=(w[0]&255)|((w[1]&255)<<8)|((w[2]&255)<<16)|((w[3]&255)<<24)|(uint64(q)<<32);break;
                            case 2:q=w[2];break;
                            case 3:value=(w[0]&0x7fff)|((w[1]&0x7fff)<<16);break;
                            case 4:value=(w[0]&0xffff)|((w[1]&0xffff)<<16)|(uint64(w[2]&0x0ffffff0)<<28)|(uint64(w[3]&0xff0)<<52);reg=(w[3]&0x8000)?12:4;break;
                            case 5:value=(w[0]&0xffff)|((w[1]&0xffff)<<16)|(uint64(w[2])<<32);reg=(w[3]&0x8000)?13:5;break;
                            case 6:case 7:case 8:case 9:case 13:break;
                            case 10:value=(uint64(w[3])>>4)<<56;break;
                            case 14:reg=w[2]&255;break;
                            case 15:break;
                            default:throw std::runtime_error("Map raw GIF unsupported packed register");
                            }
                            at+=4;
                        }else at+=2;
                        if(reg!=15)decoded.emplace_back(reg,value);
                    }
                    if(mode==1&&((loops*regs)&1))at+=2;
                }
                require(decoded==packets[observedPackets].registerWrites,
                    "Map raw GIF buffer differs from actual GS writes (possible within-MSCAL overwrite)");
                raw.push_back(std::move(r));
            }
        };
        auto feed=[&](uint32 address,unsigned qwc,bool tag){
            require(qwc<=8192&&address<ram.size()&&uint64(address)+qwc*16<=ram.size(),"Map VIF DMA bounds");
            totalQwords+=qwc;require(totalQwords<=65536,"Map total VIF budget");
            for(unsigned i=0;i<qwc;++i){
                const auto consumed=ee->m_vpu1->GetVif().ReceiveDMA(address+i*16,1,1,tag);
                if(consumed!=1||!ee->m_vpu1->IsVuReady()){
                    std::ostringstream error;error<<"Map VIF bounded feed failed: segment="<<segmentCount
                        <<" address="<<std::hex<<(address+i*16)<<" tag="<<tag<<" consumed="<<consumed
                        <<" ready="<<ee->m_vpu1->IsVuReady()<<" VU_PC="<<ee->m_VU1.m_State.nPC
                        <<" VIF_STAT="<<ee->m_vpu1->GetVif().GetRegister(CVif::VIF1_STAT)
                        <<" VIF_CODE="<<ee->m_vpu1->GetVif().GetRegister(CVif::VIF1_CODE);
                    throw std::runtime_error(error.str());
                }
                researchGs->ProcessWriteBuffer(nullptr);researchGs->FlushWriteBuffer();gs->drain();capture();
            }
        };
        // Retain generated inputs even if a later strict execution guard fails.
        writeFile((std::string(output)+".generated.bin").c_str(),ee->m_ram+packetArena,arenaEnd-packetArena);
        writeFile((std::string(output)+".model-generated.bin").c_str(),ee->m_ram+poseArena,poseEnd-poseArena);
        auto replay=[&](uint32 head,unsigned phase){
            const auto arenaBegin=phase==1?poseArena:packetArena,arenaLimit=phase==1?poseEnd:arenaEnd;
            uint32 cursor=head;bool terminal=false;
            for(unsigned tags=0;tags<512&&!terminal;++tags){
                require(cursor>=arenaBegin&&cursor+16<=arenaLimit+16&&cursor%16==0,"Map generated DMA tag pointer");
                const auto lo=get(cursor),id=lo>>28&7,qwc=lo&0xffff,ref=get(cursor+4)&0x1fffffff;
                const bool inlineData=id==1||id==2||id==6||id==7;
                require(inlineData||id==0||id==3,"Map generated DMA tag class unsupported");
                const auto data=inlineData?cursor+16:ref;
                const Source* sourceItem=nullptr;
                for(const auto& item:sources)if(data==item.address&&qwc==item.qwc){sourceItem=&item;break;}
                require((inlineData&&uint64(data)+qwc*16<=arenaLimit+16)||
                    (!inlineData&&(sourceItem||(data>=0x332220&&uint64(data)+qwc*16<=0x332480)||
                        (data>=poses&&uint64(data)+qwc*16<=poses+nodes*64))),"Map DMA payload escaped retained/generated bounds");
                if(sourceItem)activeSourceBlock=sourceItem->block;
                report<<(segmentCount?",":"")<<"{\"phase\":"<<phase<<",\"tag_address\":"<<cursor<<",\"payload_address\":"<<data<<",\"tag_words\":";
                words(ee->m_ram+cursor,16);report<<",\"payload_words\":";words(ee->m_ram+data,qwc*16);report<<'}';
                feed(data,qwc,false);
                if(sourceItem)caches.push_back({segmentCount,sourceItem->block,{ee->m_vuMem1,ee->m_vuMem1+0x4000}});
                ++segmentCount;
                terminal=id==0||id==6||id==7;cursor=id==2?ref:cursor+16+(inlineData?qwc*16:0);
            }
            require(terminal,"Map DMA chain did not terminate within bounds");
        };
        replay(preamble,0);
        const std::vector<uint8> preambleVu(ee->m_vuMem1,ee->m_vuMem1+0x4000);
        replay(batch,1);
        const std::vector<uint8> batchVu(ee->m_vuMem1,ee->m_vuMem1+0x4000);
        replay(footer,2);report<<"],\n";
        report<<"  \"vu_memory_batch_words\":";words(batchVu.data(),batchVu.size());report<<",\n";
        report<<"  \"vu_memory_preamble_words\":";words(preambleVu.data(),preambleVu.size());report<<",\n";
        unsigned rgbaWrites=0;
        for(const auto& packet:gs->packets())for(const auto& write:packet.registerWrites)if(write.first==1)++rgbaWrites;
        require(rgbaWrites>0,"Original-selected model produced no observed RGBA writes");
        report<<"  \"rgba_write_count\":"<<rgbaWrites<<",\n";
        report<<"  \"source_cache_observations\":[";
        for(unsigned i=0;i<caches.size();++i){const auto& c=caches[i];report<<(i?",":"")<<"{\"segment\":"<<c.segment<<",\"source_block\":"<<c.block<<",\"vu_words\":";words(c.words.data(),c.words.size());report<<'}';}
        report<<"],\n";
        report<<"  \"vu_memory_after_words\":";words(ee->m_vuMem1,0x4000);report<<",\n  \"raw_gif_observations\":[";
        for(unsigned i=0;i<raw.size();++i){const auto& r=raw[i];report<<(i?",":"")<<"{\"segment\":"<<r.segment<<",\"gs_packet_index\":"<<r.index<<",\"triggering_source_block\":"<<r.sourceBlock<<",\"vu_byte_address\":"<<r.address<<",\"words\":";words(r.words.data(),r.words.size()*4);report<<'}';}
        report<<"],\n  \"gs_packets\":[";
        for(unsigned i=0;i<gs->packets().size();++i){const auto& p=gs->packets()[i];report<<(i?",":"")<<"{\"path\":"<<p.metadata.pathIndex<<",\"vu_byte_address\":"<<p.metadata.vuMemPacketAddress<<",\"register_writes\":[";
            for(unsigned j=0;j<p.registerWrites.size();++j){const auto& w=p.registerWrites[j];report<<(j?",":"")<<'['<<unsigned(w.first)<<','<<uint32(w.second)<<','<<uint32(w.second>>32)<<']';}report<<"]}";}
        report<<"],\n  \"gs_registers_final_words\":";words(researchGs->GetRegisters(),CGSHandler::REGISTER_MAX*8);report<<",\n";
        for(unsigned i=0;i<code.size();++i)require(!std::memcmp(ee->m_ram+codeRanges[i].first,code[i].data(),code[i].size()),"Map observer modified original executable");
        require(!std::memcmp(ee->m_microMem1,ee->m_ram+0x215900,0x3d98),"Map observer modified original VU");
        for(const auto& item:sources)require(!std::memcmp(item.before.data(),ee->m_ram+item.address,item.before.size())&&
            !std::memcmp(item.listBefore.data(),ee->m_ram+item.list,48),"Map observer modified original source packet/list");
        report<<"  \"source_and_code_preserved\":true,\n  \"dma_tag_transfer\":false,\n  \"raw_gif_matches_gs_writes\":true,\n  \"mapping_scope\":\"Source cache observations and native corner IDs are exact per original block; a GS packet may span preceding blocks, so triggering_source_block is not a per-vertex identity claim\",\n  \"pixel_claim\":false\n}\n";
        const auto result=report.str();writeFile(output,result.data(),result.size());
        std::cout<<"Original retained map packet observed: "<<segmentCount<<" DMA segments, "<<gs->packets().size()<<" GS packets; no raster claim\n";
    }
    void ordinaryLighting(const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=100*4,top=0x350;
        if(input.empty()||input.size()%stride||input.size()>stride*2048)
            throw std::runtime_error("Ordinary lighting fixture extent");
        std::memcpy(ee->m_microMem1,ee->m_ram+0x215900,0x3d98);
        auto& cpu=ee->m_VU1;
        auto run=[&](unsigned entry,unsigned count=0) {
            put(0x1900000,0x03000000|top);put(0x1900004,0x02000000);
            put(0x1900008,0x04000000|count);put(0x190000c,0x14000000|entry);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||cpu.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Ordinary lighting VU entry did not complete");
        };
        auto vector=[&](unsigned at,std::array<float,4> value) {
            std::memcpy(ee->m_vuMem1+at*16,value.data(),16);
        };
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=stride) {
            std::array<uint32,100> f;std::memcpy(f.data(),input.data()+at,stride);
            if(f[1]>3||f[2]||f[3])throw std::runtime_error("Ordinary lighting fixture header");
            for(unsigned i=4;i<f.size();++i)
                if((f[i]&0x7f800000)==0x7f800000)
                    throw std::runtime_error("Nonfinite ordinary lighting fixture");
            // Execute actual EE light setup. Caller setters force direction W0.
            std::memcpy(ee->m_ram+0x80000,f.data()+44,48);
            for(unsigned i=0;i<3;++i)put(0x80000+i*16+12,0);
            std::memcpy(ee->m_ram+0x80100,f.data()+56,64);
            call(0x1e28f0,{0x80200,0x80000,0x80010,0x80020});
            call(0x1e2308,{0x80300,0x80100});
            call(0x1e2240,{0x80400,0x80200,0x80300});
            std::array<uint32,68> record{};
            std::copy_n(f.data(),4,record.data());
            std::memcpy(record.data()+4,ee->m_ram+0x80400,64);
            std::memset(ee->m_vuMem1,0,0x4000);run(0);
            std::memcpy(ee->m_vuMem1+top*16,f.data()+4,64);run(0x3d);
            std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x80400,48);run(0x47);
            std::memcpy(ee->m_vuMem1+top*16,f.data()+72,64);run(0x4f);
            // Prefetch slots are explicit finite values, outside the observed3.
            for(unsigned i=0;i<6;++i) {
                vector(0xe8+i,{0,0,1,0});vector(top+i+1,{0,0,1,0});
            }
            const uint32 header[]={3,0,0,0};
            std::memcpy(ee->m_vuMem1+top*16,header,16);
            std::memcpy(ee->m_vuMem1+(top+1)*16,f.data()+20,48);
            std::memcpy(ee->m_vuMem1+0xe8*16,f.data()+32,48);
            cpu.m_State.nCOP2VI[11]=0;
            if(f[1]==3)run(0x214);
            else {
                if(f[1]==1)run(0x252);
                if(f[1]==2)run(0x268);
                if(f[1]==0)std::memcpy(ee->m_vuMem1+0xe8*16,f.data()+20,48);
                std::memcpy(record.data()+20,ee->m_vuMem1+0xe8*16,48);
                run(0x280);
            }
            std::memcpy(record.data()+32,ee->m_vuMem1+0xe8*16,48);
            std::memcpy(record.data()+44,ee->m_vuMem1+0x20b*16,48);
            const uint32 primHeader[]={3,0x30004000|(0x4dU<<15)|0x4dU,0x41f,0};
            std::memcpy(ee->m_vuMem1+top*16,primHeader,16);
            for(unsigned i=0;i<3;++i) {
                const uint32 corner[]={i,i,i,i};
                std::memcpy(ee->m_vuMem1+(top+i+1)*16,corner,16);
                vector(0x87+i,{i==0?16.f:80.f,i==2?96.f:32.f,.25f,1});
                std::memcpy(ee->m_vuMem1+(0x149+i)*16,f.data()+88+i*4,16);
            }
            std::memset(ee->m_vuMem1+0x2ff*16,0xcd,12*16);
            cpu.m_State.nCOP2VI[11]=0;cpu.m_State.nCOP2VI[12]=0x3ff;
            cpu.m_State.nCOP2VI[13]=0x300;cpu.m_State.nCOP2VI[14]=0;
            run(0x3b6,10);
            if(cpu.m_State.nCOP2VI[13]!=0x30a||cpu.m_State.nPC!=0x2100)
                throw std::runtime_error("Ordinary lighting triangle missing");
            for(unsigned i=0;i<16;++i)
                if(ee->m_vuMem1[0x2ff*16+i]!=0xcd||ee->m_vuMem1[0x30a*16+i]!=0xcd)
                    throw std::runtime_error("Ordinary lighting triangle exceeded fixture");
            for(unsigned i=0;i<3;++i)
                std::memcpy(record.data()+56+i*4,ee->m_vuMem1+(0x302+i*3)*16,16);
            result.insert(result.end(),record.begin(),record.end());
        }
        writeFile(output,result.data(),result.size()*4);
        std::cout<<"Original ordinary lighting records: "<<input.size()/stride<<'\n';
    }
    void ordinaryDepth(const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=29*4,top=0x350,outputBase=0x300,qwords=10;
        if(input.empty()||input.size()%stride||input.size()>stride*64)
            throw std::runtime_error("Ordinary depth fixture extent");
        std::memcpy(ee->m_microMem1,ee->m_ram+0x215900,0x3d98);
        auto& cpu=ee->m_VU1;
        auto run=[&](unsigned entry,unsigned count=0) {
            put(0x1900000,0x03000000|top);put(0x1900004,0x02000000);
            put(0x1900008,0x04000000|count);put(0x190000c,0x14000000|entry);
            ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
            if(!ee->m_vpu1->IsVuReady()||cpu.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT)
                throw std::runtime_error("Ordinary depth VU entry did not complete");
        };
        auto vector=[&](unsigned at,std::array<float,4> value) {
            std::memcpy(ee->m_vuMem1+at*16,value.data(),16);
        };
        std::vector<uint32> result;
        for(unsigned at=0;at<input.size();at+=stride) {
            std::array<uint32,29> f;std::memcpy(f.data(),input.data()+at,stride);
            if(f[1]>3)throw std::runtime_error("Ordinary depth fixture mode");
            for(unsigned i=2;i<f.size();++i)
                if((f[i]&0x7f800000)==0x7f800000)
                    throw std::runtime_error("Nonfinite ordinary depth fixture");
            const bool textured=f[1]&1,direct=f[1]&2;
            for(unsigned i=0;i<8;++i)ee->m_EE.m_State.nCOP1[12+i]=f[2+i];
            for(unsigned i=0;i<7;++i)put(0x1ff0000+i*8,f[10+i]);
            call(0x1acc10,{0x80000});
            // Also initializes the projection-loop prefetch beyond vertex3.
            std::memset(ee->m_vuMem1,0,0x4000);run(0);
            std::memcpy(ee->m_vuMem1+top*16,ee->m_ram+0x80000,128);run(0x59);
            if(direct) {
                std::memcpy(ee->m_vuMem1+0x87*16,f.data()+17,48);
                cpu.m_State.nCOP2VI[11]=0;
            } else {
                std::memcpy(ee->m_vuMem1+0x26*16,f.data()+17,48);
                const uint32 header[]={3,0,1,0};
                std::memcpy(ee->m_vuMem1+top*16,header,16);run(0x1c1);
                if(cpu.m_State.nCOP2VI[11]!=0)
                    throw std::runtime_error("Ordinary depth probe requires unclipped camera vertices");
            }
            std::array<uint32,115> record{};
            record[0]=f[0];record[1]=f[1];record[2]=cpu.m_State.nCOP2VI[11];
            std::memcpy(record.data()+4,ee->m_ram+0x80000,128);
            if(!direct)std::memcpy(record.data()+36,ee->m_vuMem1+0x26*16,48);
            std::memcpy(record.data()+48,ee->m_vuMem1+0x87*16,48);
            for(unsigned i=0;i<3;++i) {
                if(!direct)std::memcpy(record.data()+60+i,ee->m_vuMem1+(0x1aa+i)*16+12,4);
                const uint32 corner[]={i,i,i,i};
                std::memcpy(ee->m_vuMem1+(top+i+1)*16,corner,16);
                vector(0x149+i,{128,128,128,128});vector(0x20b+i,{1,1,1,1});
                vector(0x1aa+i,{.125f*float(i+1),.125f*float(i+2),1,0});
            }
            const uint32 prim=textured?0x5dU:0x4dU;
            const uint32 header[]={3,0x30004000|(prim<<15)|prim,textured?0x412U:0x41fU,0};
            std::memcpy(ee->m_vuMem1+top*16,header,16);
            std::memset(ee->m_vuMem1+(outputBase-1)*16,0xcd,(qwords+2)*16);
            cpu.m_State.nCOP2VI[11]=0;cpu.m_State.nCOP2VI[12]=0x3ff;
            cpu.m_State.nCOP2VI[13]=outputBase;cpu.m_State.nCOP2VI[14]=0;
            run(0x3b6,qwords);
            record[3]=cpu.m_State.nCOP2VI[13]-outputBase;
            if(record[3]!=qwords||cpu.m_State.nPC!=0x2100)
                throw std::runtime_error("Ordinary depth triangle missing or wrong exit; require positive screen winding");
            for(unsigned i=0;i<16;++i)
                if(ee->m_vuMem1[(outputBase-1)*16+i]!=0xcd||ee->m_vuMem1[(outputBase+qwords)*16+i]!=0xcd)
                    throw std::runtime_error("Ordinary depth output exceeded fixture");
            std::memcpy(record.data()+63,ee->m_vuMem1+outputBase*16,qwords*16);
            for(unsigned i=0;i<3;++i) {
                const auto* packed=record.data()+63+(3+3*i)*4;
                auto* decoded=record.data()+103+i*4;
                decoded[0]=packed[0]&0xffff;decoded[1]=packed[1]&0xffff;
                decoded[2]=(packed[2]&0x0ffffff0)>>4;decoded[3]=(packed[3]&0xff0)>>4;
            }
            result.insert(result.end(),record.begin(),record.end());
        }
        writeFile(output,result.data(),result.size()*4);
        std::cout<<"Original ordinary receiver cache/packing records: "<<input.size()/stride<<'\n';
    }
    void primitive(unsigned count,bool textured,bool reverse,bool clipChecks,const char* output) {
        if (count!=3 && count!=4) throw std::runtime_error("Primitive fixture requires 3 or 4 vertices");
        std::memcpy(ee->m_microMem1,ee->m_ram+0x215900,0x3d98);
        auto& cpu=ee->m_VU1;
        const uint32 outputBase=0x300, qwords=count*3+1;
        cpu.m_State.nCOP2VI[11]=clipChecks ? 1 : 0;
        cpu.m_State.nCOP2VI[12]=0x3ff; cpu.m_State.nCOP2VI[13]=outputBase;
        cpu.m_State.nCOP2VI[14]=0; // Select the positive signed-area branch.
        auto vector=[&](unsigned reg,std::array<float,4> value) {std::memcpy(&cpu.m_State.nCOP2[reg],value.data(),16);};
        vector(1,{1,1,1,1}); vector(22,{0,0,0,0}); vector(23,{0,0,0,1});
        vector(24,{0,0,0,0}); vector(25,{0,0,100,0.5f});
        const uint32 header[]={count,0x30004000|((textured ? 0x5dU : 0x4dU)<<15)|(textured ? 0x5dU : 0x4dU),textured ? 0x412U : 0x41fU,0};
        std::memcpy(ee->m_vuMem1,header,16);
        const std::array<std::array<float,4>,4> positions={{{16,32,0.25f,1},{80,32,0.5f,1},{80,96,1,1},{16,96,2,1}}};
        for (unsigned i=0;i<count;++i) {
            const unsigned index=reverse && i ? count-i : i;
            const uint32 corner[]={index,index,index,index};
            std::memcpy(ee->m_vuMem1+(i+1)*16,corner,16);
            std::memcpy(ee->m_vuMem1+(0x87+i)*16,positions[i].data(),16);
            const std::array<float,4> uv={float(i+1)*0.125f,float(i+2)*0.125f,1,0};
            const std::array<float,4> color={float(i+1),float(i+2),float(i+3),float(i+4)};
            const std::array<float,4> light={2,3,4,5};
            std::memcpy(ee->m_vuMem1+(0x1aa+i)*16,uv.data(),16);
            std::memcpy(ee->m_vuMem1+(0x149+i)*16,color.data(),16);
            std::memcpy(ee->m_vuMem1+(0x20b+i)*16,light.data(),16);
        }
        std::memset(ee->m_vuMem1+(outputBase-1)*16,0xcd,(qwords+2)*16);
        const uint32 after=count==3 ? (clipChecks ? 0x1df8 : 0x2100) : (clipChecks ? 0x28d0 : 0x2c50);
        const uint32 entry=count==3 ? 0x3b6 : 0x511;
        put(0x1900000,0x04000000|qwords); put(0x1900004,0x14000000|entry);
        put(0x1900008,0); put(0x190000c,0);
        ee->m_vpu1->GetVif().ReceiveDMA(0x1900000,1,1,false);
        if (!ee->m_vpu1->IsVuReady() || cpu.m_State.nHasException!=MIPS_EXCEPTION_VU_EBIT || cpu.m_State.nPC!=after) {
            std::ostringstream error;
            error<<"Primitive fixture missed exit: pc=0x"<<std::hex<<cpu.m_State.nPC
                 <<" exception="<<cpu.m_State.nHasException<<" VI10="<<cpu.m_State.nCOP2VI[10]
                 <<" VI13="<<cpu.m_State.nCOP2VI[13]<<" ITOP="<<ee->m_vpu1->GetVif().GetITOP();
            throw std::runtime_error(error.str());
        }
        const uint32 emitted=cpu.m_State.nCOP2VI[13]-outputBase;
        if (emitted!=0 && emitted!=qwords) throw std::runtime_error("Unexpected primitive output size");
        for (unsigned i=0;i<16;++i)
            if (ee->m_vuMem1[(outputBase-1)*16+i]!=0xcd || ee->m_vuMem1[(outputBase+qwords)*16+i]!=0xcd)
                throw std::runtime_error("Primitive output exceeded fixture");
        std::vector<uint8> dump(4+qwords*16);
        std::memcpy(dump.data(),&emitted,4); std::memcpy(dump.data()+4,ee->m_vuMem1+outputBase*16,qwords*16);
        writeFile(output,dump.data(),dump.size());
        std::cout<<"Original VU1 primitive emitted "<<emitted<<" qwords at exit PC 0x"<<std::hex<<after<<'\n';
    }
    void animation(const std::vector<uint8>& bytes,unsigned channels,unsigned entry,unsigned frames,const char* output,const char* modelPath=nullptr,const char* xtaPath=nullptr) {
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
        uint32 bones=0,hierarchy=0,boneCount=0,nodeCount=0;
        const bool scaleChannels=get(0x900014)!=0;
        if (modelPath) {
            const auto modelBytes=readFile(modelPath);
            const auto model=okage::read_xmd(modelBytes);
            if (model.models.size()!=1 || modelBytes.size()>0x100000)
                throw std::runtime_error("Animation pose oracle requires one bounded model");
            boneCount=model.models[0].bones.size(); nodeCount=model.models[0].hierarchy.size();
            if (channels!=4+boneCount*(scaleChannels?3:2) || nodeCount>4096 ||
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
        TextureFixture textures;
        if (xtaPath) {
            if (!modelPath) throw std::runtime_error("Texture animation oracle needs XMD");
            textures=textureFixture(modelPath,xtaPath);
            if ((uint64(frames)+1)*(8+16*channels+64*nodeCount+textures.packets.size()*4)>32*1024*1024)
                throw std::runtime_error("Texture animation dump exceeds bounds");
        }
        call(0x1b5dd0,{0,entry});
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
                //0x1b7820 selects the scale/rotation/translation copier for
                // scale-aware motion context (three channels per bone).
                call(scaleChannels?0x1b4560:0x1b4480,{4,boneCount,bones+16});
                call(0x184a20,{hierarchy,bones+16,0x1800000,0});
                append(ee->m_ram+0x1800000,nodeCount*64);
            }
            if (xtaPath) {
                const auto selection=textureSelection(textures); append(selection.data(),selection.size()*4);
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
        if(std::string(argv[1])=="iop-pitch" && argc==5) {
            const auto lib=readFile(argv[2]),input=readFile(argv[3]);
            if(hash(lib)!="71596ef4029f672205e1523dedbd8dacfe5dfa50132c8a747b81289cd24f1db6" || input.empty() || input.size()%16)
                throw std::runtime_error("Pitch reference input/hash");
            auto iop=std::make_unique<Iop::CSubSystem>(true);iop->Reset();
            // Isolated function at original module address0, tables included, no external calls.
            uint32 loadSize;std::memcpy(&loadSize,lib.data()+100,4);
            if(loadSize>lib.size()-176)throw std::runtime_error("LIBSD LOAD extent");
            std::memcpy(iop->m_ram,lib.data()+176,loadSize);
            constexpr uint32 stop=0x1ff000;const uint32 jump=0x08000000|(stop>>2);
            std::memcpy(iop->m_ram+stop,&jump,4);std::memset(iop->m_ram+stop+4,0,4);
            auto& cpu=iop->m_cpu;cpu.ToggleBreakpoint(stop);std::vector<uint32> output;
            for(std::size_t at=0;at<input.size();at+=16){
                cpu.m_State.nPC=0x33a0;cpu.m_State.nHasException=0;cpu.m_State.nDelayedJumpAddr=MIPS_INVALID_PC;
                cpu.m_State.nGPR[CMIPS::RA].nD0=stop;cpu.m_State.nGPR[CMIPS::SP].nD0=0x1fe000;
                for(unsigned n=0;n<4;++n){uint32 value;std::memcpy(&value,input.data()+at+4*n,4);cpu.m_State.nGPR[CMIPS::A0+n].nD0=value;}
                bool done=false;
                for(unsigned step=0;step<100;++step){cpu.m_executor->Execute(1000);if(cpu.m_executor->MustBreak()){done=true;break;}if(cpu.m_State.nHasException)throw std::runtime_error("IOP pitch exception");}
                if(!done || cpu.m_State.nPC!=stop)throw std::runtime_error("IOP pitch did not return");
                output.push_back(cpu.m_State.nGPR[CMIPS::V0].nD0);
            }
            writeFile(argv[4],output.data(),output.size()*4);return 0;
        }
        if(std::string(argv[1])=="iop-music-channels"&&argc==5) {
            const auto driver=readFile(argv[2]),input=readFile(argv[3]);
            if(hash(driver)!="7fe8b5a33c2f8700d1f964cf5ce2f66942f8f97d952e86e6dc0ad5d4f74becb2"||
               input.empty()||input.size()%16||input.size()>16*512)throw std::runtime_error("IOP channel fixture/hash");
            auto iop=std::make_unique<Iop::CSubSystem>(true);iop->Reset();
            uint32 size;std::memcpy(&size,driver.data()+100,4);
            if(size>driver.size()-176||size>=0x180000)throw std::runtime_error("RX driver LOAD extent");
            std::memcpy(iop->m_ram,driver.data()+176,size);
            auto put=[&](uint32 a,uint32 v){std::memcpy(iop->m_ram+a,&v,4);};
            auto get=[&](uint32 a){uint32 v;std::memcpy(&v,iop->m_ram+a,4);return v;};
            constexpr uint32 stop=0x1ff000,slot=0x1bf2c,payload=0x180000;
            put(stop,0x08000000|(stop>>2));put(stop+4,0);
            auto& cpu=iop->m_cpu;cpu.ToggleBreakpoint(stop);
            auto call=[&](uint32 address,std::initializer_list<uint32> args){
                cpu.m_State.nPC=address;cpu.m_State.nHasException=0;cpu.m_State.nDelayedJumpAddr=MIPS_INVALID_PC;
                cpu.m_State.nGPR[CMIPS::RA].nD0=stop;cpu.m_State.nGPR[CMIPS::SP].nD0=0x1fe000;
                unsigned reg=CMIPS::A0;for(auto v:args)cpu.m_State.nGPR[reg++].nD0=v;
                for(unsigned step=0;step<1000;++step){
                    cpu.m_executor->Execute(1000);
                    if(cpu.m_State.nHasException==MIPS_EXCEPTION_SYSCALL) {
                        const auto epc=cpu.m_State.nCOP0[CCOP_SCU::EPC],pc=cpu.m_State.nPC;
                        if(epc!=0xbd20&&epc!=0xbd28)throw std::runtime_error("Unexpected IOP channel import");
                        iop->m_bios->HandleException();
                        if(cpu.m_State.nHasException||cpu.m_State.nPC!=pc)throw std::runtime_error("IOP channel import rescheduled");
                    } else if(cpu.m_State.nHasException){std::ostringstream error;error<<"IOP channel exception "<<cpu.m_State.nHasException<<" at "<<std::hex<<cpu.m_State.nPC;throw std::runtime_error(error.str());}
                    if(cpu.m_executor->MustBreak()){
                        if(cpu.m_State.nPC!=stop)throw std::runtime_error("IOP channel unexpected breakpoint");
                        return;
                    }
                }
                throw std::runtime_error("IOP channel instruction budget");
            };
            std::vector<uint32> output;
            auto capture=[&]{for(unsigned i=0;i<16;++i){const auto at=slot+0x2c+i*0x28;
                output.push_back(iop->m_ram[at]);output.push_back(get(at+0x14));}};
            for(unsigned at=0;at<input.size();at+=16) {
                std::array<uint32,4> r;std::memcpy(r.data(),input.data()+at,16);
                if(r[0]>1||r[1]>15||r[2]>128||r[3]>128)throw std::runtime_error("IOP channel case scope");
                std::memset(iop->m_ram+slot,0,0x2d0);put(slot+8,0x10000|r[0]);put(0x2192c,slot);put(0xd334,0);
                for(unsigned i=0;i<16;++i)put(slot+0x2c+i*0x28+0x14,0x00800080);
                put(payload,3);put(payload+4,r[2]|(r[3]<<16));call(0x56f8,{0,r[1],payload});capture();
                put(0xd334,1);call(0x4578,{});capture();
            }
            writeFile(argv[4],output.data(),output.size()*4);return 0;
        }
        Reference reference(readFile(argv[2]));
        const std::string command=argv[1];
        if(command=="material-colors" && argc==5) {
            reference.materialColors(readFile(argv[3]),argv[4]);
        } else if(command=="relationship" && argc==6) {
            reference.relationship(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="comparison-options" && argc==6) {
            reference.comparisonOptions(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="message-init" && argc==6) {
            reference.messageInit(readFile(argv[3]),std::stoul(argv[4]),argv[5]);
        } else if(command=="effect-tint" && argc==5) {
            reference.effectTint(readFile(argv[3]),argv[4]);
        } else if(command=="shadow24-effect-geometry" && argc==7) {
            reference.mapEffectGeometry(readFile(argv[3]),readFile(argv[5]),argv[6],24,readFile(argv[4]));
        } else if((command=="effect-template-load"||command=="effect-template-geometry") && argc==7) {
            reference.effectTemplateLoad(readFile(argv[3]),readFile(argv[4]),std::stoul(argv[5],nullptr,0),argv[6],command=="effect-template-geometry");
        } else if(command=="effect-noise" && argc==6) {
            reference.effectNoise(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="procedural-effect-shape" && argc==6) {
            reference.effectShape(readFile(argv[3]),readFile(argv[4]),argv[5],true);
        } else if(command=="effect-shape" && argc==6) {
            reference.effectShape(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if((command=="map-effect-geometry"||command=="torch-effect-geometry"||command=="flame16-effect-geometry"||command=="summon33-effect-geometry"||command=="summon115-effect-geometry"||command=="cylinder26-effect-geometry"||command=="dark6-effect-geometry"||command=="church14-effect-geometry") && argc==6) {
            reference.mapEffectGeometry(readFile(argv[3]),readFile(argv[4]),argv[5],command=="church14-effect-geometry"?14:command=="dark6-effect-geometry"?6:command=="cylinder26-effect-geometry"?26:command=="summon33-effect-geometry"?33:command=="summon115-effect-geometry"?115:command=="flame16-effect-geometry"?16:command=="torch-effect-geometry"?91:25);
        } else if(command=="effect-reference-probe" && argc==6) {
            reference.effectReferenceProbe(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="stream-prepare" && argc==6) {
            reference.streamPrepare(readFile(argv[3]),std::stoul(argv[4]),argv[5]);
        } else if(command=="exterior-camera-step" && argc==6) {
            reference.exteriorCameraStep(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="enemy-step" && argc==6) {
            reference.enemyStep(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="camera-angles" && argc==4) {
            reference.call(0x1b1ed0);writeFile(argv[3],reference.ee->m_ram+0x335490,8192);
        } else if(command=="camera-look-at" && argc==6) {
            reference.cameraLookAt(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="stream-bank" && argc==5) {
            reference.streamBank(readFile(argv[3]),argv[4]);
        } else if(command=="instance-visibility" && argc==5) {
            reference.instanceVisibility(readFile(argv[3]),argv[4]);
        } else if(command=="scene-entry-fade" && argc==4) {
            reference.sceneEntryFade(argv[3]);
        } else if(command=="scene-fade" && argc==5) {
            reference.sceneFade(readFile(argv[3]),argv[4]);
        } else if(command=="jingle-routing" && argc==4) {
            reference.jingleRouting(argv[3]);
        } else if(command=="music-channels" && argc==5) {
            reference.musicChannels(readFile(argv[3]),argv[4]);
        } else if(command=="music-fade" && argc==5) {
            reference.musicFade(readFile(argv[3]),argv[4]);
        } else if(command=="dialogue-opacity" && argc==5) {
            reference.dialogueOpacity(readFile(argv[3]),argv[4]);
        } else if(command=="text-glyphs" && argc==6) {
            reference.textGlyphs(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="font-metrics" && argc==5) {
            reference.fontMetrics(readFile(argv[3]),argv[4]);
        } else if(command=="text-clock" && argc==5) {
            reference.textClock(readFile(argv[3]),argv[4]);
        } else if(command=="name-expansion" && argc==7) {
            reference.nameExpansion(readFile(argv[3]),argv[4],argv[5],argv[6]);
        } else if(command=="text-controls" && argc==5) {
            reference.textControls(readFile(argv[3]),argv[4]);
        } else if(command=="pad-repeat" && argc==5) {
            reference.padRepeat(readFile(argv[3]),argv[4]);
        } else if(command=="choice-options" && argc==5) {
            reference.choiceOptions(readFile(argv[3]),argv[4]);
        } else if(command=="xcl-probe" && argc==6) {
            reference.xclProbe(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="xcl-vertical" && argc==6) {
            reference.xclVertical(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="xcl-wall" && argc==6) {
            reference.xclWall(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="xcl-camera" && argc==6) {
            reference.xclCamera(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="xcl-move" && argc==6) {
            reference.xclMove(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="capsule-pair" && argc==5) {
            reference.capsulePair(readFile(argv[3]),argv[4]);
        } else if(command=="collision-world" && argc==6) {
            reference.collisionWorld(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="new-game-state" && argc==7) {
            reference.gameNewStats(readFile(argv[3]),readFile(argv[4]),readFile(argv[5]),argv[6]);
        } else if(command=="game-vars-reset" && argc==5) {
            reference.gameVariablesReset(readFile(argv[3]),argv[4]);
        } else if(command=="battle-state-init" && argc==5) {
            reference.battleStateInit(readFile(argv[3]),argv[4]);
        } else if(command=="event-shorts" && argc==5) {
            reference.eventShorts(readFile(argv[3]),argv[4]);
        } else if(command=="event-write" && argc==6) {
            reference.eventWrite(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="map-effect-probe" && argc==7) {
            reference.mapEffectProbe(readFile(argv[3]),readFile(argv[4]),std::stoul(argv[5]),argv[6]);
        } else if(command=="effect-probe" && argc==8) {
            reference.effectProbe(readFile(argv[3]),std::stoul(argv[4]),std::stoul(argv[5]),std::stoul(argv[6]),argv[7]);
        } else if(command=="pme-choice" && argc==8) {
            reference.pmeChoice(readFile(argv[3]),std::stoi(argv[4]),std::stoul(argv[5]),std::stoul(argv[6]),argv[7]);
        } else if(command=="pme-name" && argc==7) {
            reference.pmeMessage(readFile(argv[3]),std::stoi(argv[4]),argv[6],false,argv[5]);
        } else if((command=="pme-message" || command=="pme-indices") && argc==6) {
            reference.pmeMessage(readFile(argv[3]),std::stoi(argv[4]),argv[5],command=="pme-indices");
        } else if(command=="spu-sample" && argc==5) {
            reference.spuSample(readFile(argv[3]),argv[4]);
        } else if(command=="dialogue-wait" && argc==5) {
            reference.dialogueWait(readFile(argv[3]),argv[4]);
        } else if((command=="event-setup"||command=="event-setup-strings") && argc==7) {
            reference.eventSetup(readFile(argv[3]),readFile(argv[4]),readFile(argv[5]),argv[6],command=="event-setup-strings");
        } else if ((command=="instance-matrices" || command=="camera-matrices") && argc==5) {
            reference.instanceMatrices(readFile(argv[3]),argv[4],command=="camera-matrices");
        } else if (command=="map-transforms" && argc==5) {
            reference.mapTransforms(readFile(argv[3]),argv[4]);
        } else if ((command=="uv" || command=="texture-clamp") && argc==5) {
            reference.uv(readFile(argv[3]),argv[4],command=="texture-clamp");
        } else if (command=="shadow-visibility" && argc==5) {
            reference.shadowVisibility(readFile(argv[3]),argv[4]);
        } else if (command=="shadow-bounds" && argc==6) {
            reference.shadowBounds(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if (command=="shadow-assembled" && argc==7) {
            reference.shadowAssembled(readFile(argv[3]),readFile(argv[4]),readFile(argv[5]),argv[6]);
        } else if (command=="shadow-motion" && argc==10) {
            reference.shadowMotion(readFile(argv[3]),readFile(argv[4]),readFile(argv[5]),argv[3],argv[6],std::stoul(argv[7]),std::stoul(argv[8]),argv[9]);
        } else if (command=="shadow-batches" && argc==5) {
            reference.shadowBatches(readFile(argv[3]),argv[4]);
        } else if (command=="shadow-bind" && argc==6) {
            reference.shadowBind(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if (command=="shadow-cache" && argc==5) {
            reference.shadowCache(readFile(argv[3]),argv[4]);
        } else if (command=="shadow-prism" && argc==5) {
            reference.shadowPrism(readFile(argv[3]),argv[4]);
        } else if (command=="shadow-raster" && argc==5) {
            reference.shadowPrism(readFile(argv[3]),argv[4],true);
        } else if (command=="shadow-projection" && argc==5) {
            reference.shadowProjection(readFile(argv[3]),argv[4]);
        } else if (command=="shadow-update" && argc==6) {
            reference.shadowUpdate(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if (command=="instance-alpha" && argc==5) {
            reference.instanceAlpha(readFile(argv[3]),argv[4]);
        } else if (command=="dialogue-shadow" && argc==6) {
            reference.dialogueShadow(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="map-packet" && argc==6) {
            reference.mapPacket(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if (command=="ordinary-lighting" && argc==5) {
            reference.ordinaryLighting(readFile(argv[3]),argv[4]);
        } else if (command=="ordinary-depth" && argc==5) {
            reference.ordinaryDepth(readFile(argv[3]),argv[4]);
        } else if (command=="primitive" && argc==8) {
            reference.primitive(std::stoul(argv[3]),std::stoul(argv[4])!=0,std::stoul(argv[5])!=0,std::stoul(argv[6])!=0,argv[7]);
        } else if (command=="texture-callback" && argc==6) {
            reference.textureCallbacks(argv[3],argv[4],argv[5]);
        } else if (command=="animation" && (argc>=8 && argc<=10)) {
            reference.animation(readFile(argv[3]),std::stoul(argv[4]),std::stoul(argv[5]),std::stoul(argv[6]),argv[7],argc>=9 ? argv[8] : nullptr,argc==10 ? argv[9] : nullptr);
        } else if (command=="sine" && argc==4) {
            reference.call(0x001b1d88);
            writeFile(argv[3],reference.ee->m_ram+0x00202ee0,16384*4);
            std::cout<<"Executed retail sine initializer; wrote 16384 float entries\n";
        } else if ((command=="vu-asm" || command=="vu1-asm" || command=="shadow-vu1-asm") && argc==4) {
            const bool vu1=command!="vu-asm",shadow=command=="shadow-vu1-asm";
            const uint32 length=vu1?(shadow?0x1358:0x3d98):0x660;
            if (vu1) std::memcpy(reference.ee->m_microMem1,reference.ee->m_ram+(shadow?0x219700:0x215900),length);
            else reference.loadMicro();
            auto& cpu=vu1 ? reference.ee->m_VU1 : reference.ee->m_VU0;
            const auto* memory=vu1 ? reference.ee->m_microMem1 : reference.ee->m_microMem0;
            std::ostringstream listing;
            for (uint32 at=0;at<length;at+=4) {
                uint32 word; std::memcpy(&word,memory+at,4);
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
