#pragma once
#include "ee/Ee_SubSystem.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

class SceneTrace {
    std::filesystem::path directory;
    std::ofstream manifest;
    unsigned currentModel=0,draws=0;
    bool hasModel=false;
    unsigned pendingNodes=0,pendingDraw=0;
public:
    explicit SceneTrace(const std::filesystem::path& path):directory(path) {
        if(std::filesystem::exists(path) || std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing scene trace directory");
        std::filesystem::create_directories(path);manifest.open(path/"draws.tsv");
        if(!manifest)throw std::runtime_error("Cannot create scene trace manifest");
        manifest<<"draw\tvblank\tmodel\tcaller\tbones\tnodes\tbone_address\thierarchy_address\tscale_mode\n";
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;const auto pc=cpu.nPC;
        if(pc==0x18579c || pc==0x186058) {
            const uint32 at=uint32(cpu.nGPR[CMIPS::V0].nD0)&0x1fffffff;
            if(!pendingNodes || at>=PS2::EE_RAM_SIZE || pendingNodes*64>PS2::EE_RAM_SIZE-at)
                throw std::runtime_error("Unexpected scene matrix return");
            std::ostringstream name;name<<std::setw(4)<<std::setfill('0')<<pendingDraw<<".retail-pose.bin";
            std::ofstream out(directory/name.str(),std::ios::binary);
            if(!out.write(reinterpret_cast<const char*>(ee.m_ram+at),pendingNodes*64))throw std::runtime_error("Cannot write original pose snapshot");
            pendingNodes=0;return;
        }
        if(pc==0x1856f8 || pc==0x185fb0) {currentModel=uint32(cpu.nGPR[CMIPS::A0].nD0);hasModel=true;return;}
        const auto caller=uint32(cpu.nGPR[CMIPS::RA].nD0);
        if(pc!=0x184bd8 || !hasModel || (caller!=0x18579c && caller!=0x186058) || currentModel>=4096 || draws>=1024)
            throw std::runtime_error("Unexpected scene trace call or limit");
        auto address=[](uint32 value){return value&0x1fffffff;};
        auto range=[&](uint32 at,uint32 size){if(at>=PS2::EE_RAM_SIZE || size>PS2::EE_RAM_SIZE-at)throw std::runtime_error("Scene trace RAM bounds");};
        auto word=[&](uint32 at){range(at,4);uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        const auto model=address(word(0x1fe2ac))+currentModel*0x34;
        const auto bones=word(address(word(model)));
        const auto hierarchy=address(uint32(cpu.nGPR[CMIPS::A0].nD0));
        const auto channels=address(uint32(cpu.nGPR[CMIPS::A1].nD0));
        const auto world=address(uint32(cpu.nGPR[CMIPS::A2].nD0));
        const auto nodes=word(hierarchy);
        if(!bones || !nodes || bones>4096 || nodes>4096)throw std::runtime_error("Scene trace model bounds");
        std::ostringstream prefix;prefix<<std::setw(4)<<std::setfill('0')<<draws;
        auto dump=[&](const char* suffix,uint32 at,uint32 size){
            range(at,size);std::ofstream out(directory/(prefix.str()+suffix),std::ios::binary);
            if(!out.write(reinterpret_cast<const char*>(ee.m_ram+at),size))throw std::runtime_error("Cannot write scene trace buffer");
        };
        dump(".world.bin",world,64);dump(".view.bin",0x3322a0,64);
        dump(".projection.bin",0x332220,128);dump(".camera.bin",0x3321f0,48);
        dump(".projection-parameters.bin",0x1fe2b4,64);
        dump(".bones.bin",channels,bones*48);dump(".hierarchy.bin",hierarchy,8+nodes*8);
        manifest<<draws<<'\t'<<frame<<'\t'<<currentModel<<'\t'<<std::hex<<caller<<std::dec<<'\t'<<bones<<'\t'<<nodes
                <<'\t'<<std::hex<<channels<<'\t'<<hierarchy<<std::dec<<'\t'<<word(0x1fe2f4)<<'\n';
        manifest.flush();if(!manifest)throw std::runtime_error("Cannot write scene trace row");
        pendingDraw=draws;pendingNodes=nodes;++draws;hasModel=false;
    }
};
