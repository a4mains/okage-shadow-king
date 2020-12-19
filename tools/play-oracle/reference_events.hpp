#pragma once
#include "ee/Ee_SubSystem.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

class EventTrace {
    std::filesystem::path directory;
    std::ofstream manifest;
    std::map<uint32,unsigned> scripts;
    unsigned observations=0;
public:
    explicit EventTrace(const std::filesystem::path& path):directory(path) {
        if(std::filesystem::exists(path) || std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing event trace directory");
        std::filesystem::create_directories(path);
        manifest.open(path/"instructions.tsv");
        if(!manifest)throw std::runtime_error("Cannot create event trace manifest");
        manifest<<"observation\tvblank\tscript\tcontext\tgroup\tbase_group\toffset\tword\thandler\n";
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        if((cpu.nPC!=0x1b5888 && cpu.nPC!=0x1b58ac) || observations>=200000)
            throw std::runtime_error("Unexpected event trace call or limit");
        auto range=[](uint32 at,uint32 size){
            if(at>=PS2::EE_RAM_SIZE || size>PS2::EE_RAM_SIZE-at)
                throw std::runtime_error("Event trace RAM bounds");
        };
        auto word=[&](uint32 at){range(at,4);uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;};
        const auto context=word(0x337618);
        const auto asset=word(context+8);
        if(word(asset)!=0x544d58)throw std::runtime_error("Event trace XMT magic mismatch");
        const uint32 header=word(asset+16)==0xffffffff ? 32 : 16;
        const auto codeSize=word(asset+4);
        const uint64 size=uint64(header)+codeSize+word(asset+8)+word(asset+12);
        if(size>0x1000000 || codeSize<4)throw std::runtime_error("Event trace XMT size bounds");
        range(asset,uint32(size));
        const auto pc=uint32(cpu.nGPR[CMIPS::S1].nD0);
        if(pc<asset+header || uint64(pc)+4>uint64(asset)+header+codeSize || (pc&3))
            throw std::runtime_error("Event trace XMT PC bounds");
        const auto instruction=word(pc),op=instruction&255;
        if(op!=uint32(cpu.nGPR[CMIPS::A1].nD0) ||
           ((instruction>>8)&0xfff)!=uint32(cpu.nGPR[CMIPS::A2].nD0) ||
           (int32(instruction)>>20)!=int32(cpu.nGPR[CMIPS::S0].nD0))
            throw std::runtime_error("Event trace dispatcher word mismatch");
        if(!scripts.contains(asset)) {
            const unsigned id=scripts.size();scripts.emplace(asset,id);
            std::ofstream out(directory/(std::to_string(id)+".xmt"),std::ios::binary);
            if(!out.write(reinterpret_cast<const char*>(ee.m_ram+asset),size))
                throw std::runtime_error("Cannot write event source snapshot");
            for(const auto& [suffix,global,length]:{
                    std::tuple{".variables.bin",0x24f788u,0x1000u},
                    std::tuple{".field.bin",0x2d9bf4u,0x800u}}) {
                const auto at=word(global);range(at,length);
                if(at<0x100000)throw std::runtime_error("Event runtime structure not initialized");
                std::ofstream snapshot(directory/(std::to_string(id)+suffix),std::ios::binary);
                if(!snapshot.write(reinterpret_cast<const char*>(ee.m_ram+at),length))
                    throw std::runtime_error("Cannot write initial event state");
            }
        }
        if(op==0) {
            const auto at=word(0x2d9bf4);range(at,0x800);
            const auto name=std::to_string(scripts.at(asset))+".end-"+std::to_string(observations)+".field.bin";
            std::ofstream snapshot(directory/name,std::ios::binary);
            if(!snapshot.write(reinterpret_cast<const char*>(ee.m_ram+at),0x800))
                throw std::runtime_error("Cannot write completed event state");
        }
        uint32 handler=0;
        if(op>=0x20)handler=word(0x212ef0+(op-0x20)*4);
        else if(op>=7 && op<=11 && op!=8)handler=word(0x337930+(op==7 ? 0 : op-8)*4);
        manifest<<observations++<<'\t'<<frame<<'\t'<<scripts.at(asset)<<'\t'<<std::hex<<context
                <<std::dec<<'\t'<<int32(cpu.nGPR[CMIPS::S2].nD0)<<'\t'<<int32(cpu.nGPR[CMIPS::S5].nD0)
                <<'\t'<<std::hex<<pc-asset<<'\t'<<instruction<<'\t'<<handler<<std::dec<<'\n';
        if(!manifest)throw std::runtime_error("Cannot write event trace row");
    }
};
