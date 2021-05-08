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
    bool streamTransferProbe=false;
    static constexpr uint32 stop=0x007ff000;
    uint32 expectedStop=stop;
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
        std::ostringstream error; error<<"Oracle instruction budget exceeded at "<<std::hex<<cpu.m_State.nPC;
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
    void mapEffectGeometry(const std::vector<uint8>& ram,const std::vector<uint8>& input,const char* output) {
        constexpr unsigned stride=198*4,sp=0x1ff0000;
        if(ram.size()!=32*1024*1024||input.empty()||input.size()%stride||input.size()>stride*64)
            throw std::runtime_error("Effect geometry fixture bounds");
        std::vector<uint32> result,packets;
        for(unsigned at=0;at<input.size();at+=stride) {
            ee->m_EE.m_executor->Reset();
            std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
            loadMicro();const auto table=get(0x1f849c),job=get(table+8);
            if(get(job)!=25||get(job+8)!=8)throw std::runtime_error("Expected captured map effect25");
            std::memcpy(ee->m_ram+job+0x10,input.data()+at+4,64);
            call(0x1842c8);const auto camera=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            std::memcpy(ee->m_ram+camera,input.data()+at+68,16);
            std::memcpy(ee->m_ram+0x80000,input.data()+at+84,64);
            std::memcpy(ee->m_ram+0x1f8484,input.data()+at+148,4);
            put(0x1f8490,get(job+0x78));put(sp+0xe0,0x80000);put(sp+0xf4,sp+0xa0);
            auto& cpu=ee->m_EE;
            cpu.m_State.nGPR[CMIPS::S3].nD0=8;cpu.m_State.nGPR[CMIPS::S6].nD0=2;
            cpu.m_State.nGPR[CMIPS::FP].nD0=0x202ee0;
            auto block=[&](uint32 begin,uint32 end) {
                expectedStop=end;cpu.ToggleBreakpoint(end);call(begin);
                cpu.ToggleBreakpoint(end);expectedStop=stop;
            };
            block(0x1024e0,0x1027b8);
            // Caller scratch destinations from0x1027b8..0x1027c0.
            put(sp+0xf8,sp+0x10);cpu.m_State.nGPR[CMIPS::S7].nD0=sp+0x90;
            uint32 identity;std::memcpy(&identity,input.data()+at,4);result.push_back(identity);
            for(unsigned i=0;i<8;++i) {
                const auto e=get(job+12)+i*128,r=get(e+0x78),g=get(e+0x64);
                const auto source=input.data()+at+152+i*80;
                std::array<uint32,4> meta;std::memcpy(meta.data(),source+64,16);
                unsigned texture=0;for(;texture<31;++texture)if(get(get(0x24e780)+texture*4)==(get(e+0x5c)&65535))break;
                if(meta[0]!=(i<3?24u:29u)||meta[1]!=get(e+0x54)||meta[2]!=get(e+0x58)||meta[3]!=texture)
                    throw std::runtime_error("Effect geometry input metadata mismatch");
                std::memcpy(ee->m_ram+r,source,64);
                cpu.m_State.nGPR[CMIPS::S1].nD0=e;cpu.m_State.nGPR[CMIPS::S2].nD0=r;
                cpu.m_State.nGPR[CMIPS::S4].nD0=i;
                block(0x102978,0x1aeb10);
                block(0x102a20,0x1ad3c8);
                for(unsigned k=0;k<16;++k)result.push_back(get(sp+0x10+k*4));
                for(unsigned k=0;k<4;++k)result.push_back(get(sp+0x90+k*4));
                if(at==0) {
                    packets.insert(packets.end(),{i,meta[0],get(e+0x68),get(e+0x6c)});
                    for(unsigned k=0;k<6;++k)packets.push_back(get(r+0x40+k*4));
                    for(unsigned offset:{4u,8u,12u,16u}) {
                        const auto p=get(g+offset),bytes=(get(p)&65535)*16+32;
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
    void mapEffectProbe(const std::vector<uint8>& ram,const std::vector<uint8>& input,unsigned frames,const char* output) {
        if(ram.size()!=32*1024*1024||input.size()!=80||frames>1000)
            throw std::runtime_error("Map effect probe fixture bounds");
        std::array<uint32,20> request;std::memcpy(request.data(),input.data(),80);
        if((request[0]!=17&&request[0]!=19&&request[0]!=25&&request[0]!=34)||request[2]>0xfff||request[3]!=0x100)
            throw std::runtime_error("Map effect probe request scope");
        std::memcpy(ee->m_ram,ram.data(),ram.size());put(stop,0x08000000|(stop>>2));put(stop+4,0);
        effectOs=std::make_unique<CPS2OS>(ee->m_EE,ee->m_ram,ee->m_bios,ee->m_spr,
                                        ee->m_gs,ee->m_sif,*dynamic_cast<CIopBios*>(iop->m_bios.get()));
        loadMicro();
        const auto table=get(0x1f849c),random=get(0x2151b4)+0x58;
        if(table>ram.size()-212*4||random>ram.size()-4)throw std::runtime_error("Map effect fixture pointers");
        for(unsigned i=0;i<84;++i)put(table+4*i,0xffffffff);
        put(random,request[1]);std::memcpy(ee->m_ram+0x80000,request.data()+4,64);
        if(request[0]==17)call(0x1012f8,{17,0x80000});
        else call(0x101440,{request[0],0x80000,0x80010,0x80020});
        const auto handle=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
        if(handle!=0)throw std::runtime_error("Map effect allocation order");
        put(0x1f85a4,1);
        std::vector<uint32> result;
        auto capture=[&](unsigned step){
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
                if(texture==get(0x1f8498))throw std::runtime_error("Map effect texture identity");
                result.insert(result.end(),{texture,get(element+0x68),get(element+0x6c),get(render+0x58)});
            }
        };
        capture(0);
        if(request[0]!=17) {
            call(0x130d88,{0,request[2],request[3],0x80030});
            const auto next=uint32(ee->m_EE.m_State.nGPR[CMIPS::V0].nD0);
            if(next!=(request[2]<0xf00?0x80030u:0x8003cu))throw std::runtime_error("Map effect service payload extent");
        }
        capture(1);
        for(unsigned frame=0;frame<frames;++frame){call(0x1131d0,{handle});capture(frame+2);}
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
        if(command=="relationship" && argc==6) {
            reference.relationship(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="comparison-options" && argc==6) {
            reference.comparisonOptions(readFile(argv[3]),readFile(argv[4]),argv[5]);
        } else if(command=="message-init" && argc==6) {
            reference.messageInit(readFile(argv[3]),std::stoul(argv[4]),argv[5]);
        } else if(command=="effect-tint" && argc==5) {
            reference.effectTint(readFile(argv[3]),argv[4]);
        } else if(command=="map-effect-geometry" && argc==6) {
            reference.mapEffectGeometry(readFile(argv[3]),readFile(argv[4]),argv[5]);
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
