// Research-only EE/VU reference. Never linked into the native game or asset tools.
#include "AppConfig.h"
#include "ee/Ee_SubSystem.h"
#include "ee/EeExecutor.h"
#include "ee/Vif.h"
#include "iop/Iop_SubSystem.h"
#include "iop/IopBios.h"
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
        Reference reference(readFile(argv[2]));
        const std::string command=argv[1];
        if (command=="map-transforms" && argc==5) {
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
