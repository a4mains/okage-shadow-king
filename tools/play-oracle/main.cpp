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
#include "okage/model.hpp"
#include "okage/texture_animation.hpp"
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
    std::unique_ptr<CPS2OS> effectOs;
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
                if (cpu.m_State.nPC!=stop) {
                    std::ostringstream error;error<<"Unexpected oracle breakpoint at "<<std::hex<<cpu.m_State.nPC
                        <<", wanted "<<stop;throw std::runtime_error(error.str());
                }
                return;
            }
        }
        std::ostringstream error; error<<"Oracle instruction budget exceeded at "<<std::hex<<cpu.m_State.nPC;
        throw std::runtime_error(error.str());
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
        if(name) {
            if(std::strlen(name)>12)throw std::runtime_error("Name oracle fixture exceeds reserved name capacity");
            constexpr uint32 tag=0x7fd000,value=0x7fd100;
            std::memcpy(ee->m_ram+tag,"############",13);
            std::memcpy(ee->m_ram+value,name,std::strlen(name)+1);
            call(0x19a5c8,{1,tag,value});
        }
        // Existing live text handle 1 and bank 0 from the opening600 snapshot.
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
    void eventSetup(const std::vector<uint8>& script,const std::vector<uint8>& variables,
                    const std::vector<uint8>& state,const char* output) {
        if(script.size()<36 || script.size()>0x100000 || variables.size()!=0x1000 || state.size()!=0x800)
            throw std::runtime_error("Event setup fixture bounds");
        std::memcpy(ee->m_ram+0x900000,script.data(),script.size());
        std::memcpy(ee->m_ram+0xa00000,variables.data(),variables.size());
        std::memcpy(ee->m_ram+0xb00000,state.data(),state.size());
        std::memset(ee->m_ram+0x800000,0,192*24);
        put(0x337614,0x800000);put(0x24f788,0xa00000);put(0x2d9bf4,0xb00000);
        put(0x33793c,0x130378);put(0x212ef0+(0x50-0x20)*4,0x130bb8);
        put(0x212ef0+(0x59-0x20)*4,0x131048);
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
                call(0x1b4480,{4,boneCount,bones+16});
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
        Reference reference(readFile(argv[2]));
        const std::string command=argv[1];
        if(command=="camera-angles" && argc==4) {
            reference.call(0x1b1ed0);writeFile(argv[3],reference.ee->m_ram+0x335490,8192);
        } else if(command=="camera-look-at" && argc==6) {
            reference.cameraLookAt(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="stream-bank" && argc==5) {
            reference.streamBank(readFile(argv[3]),argv[4]);
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
        } else if(command=="text-controls" && argc==5) {
            reference.textControls(readFile(argv[3]),argv[4]);
        } else if(command=="pad-repeat" && argc==5) {
            reference.padRepeat(readFile(argv[3]),argv[4]);
        } else if(command=="choice-options" && argc==5) {
            reference.choiceOptions(readFile(argv[3]),argv[4]);
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
        } else if(command=="event-setup" && argc==7) {
            reference.eventSetup(readFile(argv[3]),readFile(argv[4]),readFile(argv[5]),argv[6]);
        } else if ((command=="instance-matrices" || command=="camera-matrices") && argc==5) {
            reference.instanceMatrices(readFile(argv[3]),argv[4],command=="camera-matrices");
        } else if (command=="map-transforms" && argc==5) {
            reference.mapTransforms(readFile(argv[3]),argv[4]);
        } else if ((command=="uv" || command=="texture-clamp") && argc==5) {
            reference.uv(readFile(argv[3]),argv[4],command=="texture-clamp");
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
        } else if ((command=="vu-asm" || command=="vu1-asm") && argc==4) {
            const bool vu1=command=="vu1-asm";
            if (vu1) std::memcpy(reference.ee->m_microMem1,reference.ee->m_ram+0x215900,0x3d98);
            else reference.loadMicro();
            auto& cpu=vu1 ? reference.ee->m_VU1 : reference.ee->m_VU0;
            const auto* memory=vu1 ? reference.ee->m_microMem1 : reference.ee->m_microMem0;
            std::ostringstream listing;
            for (uint32 at=0;at<(vu1 ? 0x3d98 : 0x660);at+=4) {
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
