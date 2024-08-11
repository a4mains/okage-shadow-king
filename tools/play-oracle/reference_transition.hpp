#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <filesystem>
#include <fstream>
class TransitionTrace {
    std::ofstream output;unsigned count=0;
public:
    static constexpr std::array<uint32,7> breakpoints={0x133a98,0x1305e0,0x131d28,0x1bb7e0,0x124930,0x101118,0x134c18};
    explicit TransitionTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing transition trace");
        output.open(path,std::ios::binary);if(!output)throw std::runtime_error("Cannot create transition trace");
    }
    void finish() const {if(!count)throw std::runtime_error("Transition capture observed no callbacks");}
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        auto valid=[](uint32 at,unsigned size){return at>=0x100000&&at<=PS2::EE_RAM_SIZE-size;};
        auto word=[&](uint32 at){if(at>PS2::EE_RAM_SIZE-4)throw std::runtime_error("Transition trace bounds");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;};
        if(++count>200000)throw std::runtime_error("Transition observation limit");
        std::array<uint32,128> row={frame,cpu.nPC,uint32(cpu.nGPR[CMIPS::RA].nD0)};
        for(unsigned i=0;i<4;++i)row[3+i]=uint32(cpu.nGPR[CMIPS::A0+i].nD0);
        const auto field=word(0x2d9bf4),variables=word(0x24f788);row[7]=field;
        if(valid(field,0x800)) {
            row[8]=1;
            for(unsigned i=0;i<8;++i)row[9+i]=word(field+0xd4+i*4);
            row[17]=word(field+0x48);row[18]=word(field+0x70);row[19]=word(field+0x408);
            const auto game=word(field+0x14);if(valid(game,0x60))for(unsigned i=0;i<8;++i)row[20+i]=word(game+0x40+i*4);
            const auto player=word(field+0x44);
            if(cpu.nPC==0x133a98 && (row[17]==0||row[17]==5||row[17]==14) && valid(player,0x284)) {
                row[28]=1;
                for(unsigned i=0;i<8;++i)row[48+i]=word(player+0x22c+i*4);
                const auto instance=word(0x337b64),objects=word(0x21301c),channels=word(0x33760c);
                if(instance<2048&&valid(objects,instance*0x3c+0x3c)) {
                    const auto first=word(objects+instance*0x3c+4)&0xffff,base=channels+first*0x90+0x20;
                    if(first<5120&&valid(base,0xa0))for(unsigned i=0;i<4;++i){row[40+i]=word(base+i*4);row[44+i]=word(base+0x90+i*4);}
                }
                row[29]=word(player+0x27c);
                const auto handle=row[29]&0xfff,table=word(0x1f849c);
                if(row[29]&&handle<84&&valid(table,84*4)) {
                    const auto effect=word(table+4*handle);
                    if(valid(effect,0x80)) {
                        row[30]=1;row[31]=word(effect+4);row[32]=word(effect+0x64);
                        for(unsigned i=0;i<20;++i)row[64+i]=word(effect+0x10+i*4);
                        const auto elements=word(effect+0xc);
                        if(word(effect+8)==2&&valid(elements,0x100))for(unsigned i=0;i<2;++i) {
                            row[84+i*4]=word(elements+i*0x80+0x5c);
                            row[85+i*4]=word(elements+i*0x80+0x68);
                            row[86+i*4]=word(elements+i*0x80+0x6c);
                        }
                    }
                }
            }
        }
        row[33]=word(0x33958c);row[34]=word(0x339590);
        const auto ctx=word(0x337614);
        if(valid(ctx,24)){row[35]=word(ctx);row[36]=word(ctx+20)-word(ctx+8);}
        if(valid(variables,0x1000))for(unsigned i=0;i<16;++i)row[112+i]=word(variables+0x510+i*4);
        if(!output.write(reinterpret_cast<const char*>(row.data()),sizeof(row)))throw std::runtime_error("Cannot write transition trace");
    }
};
