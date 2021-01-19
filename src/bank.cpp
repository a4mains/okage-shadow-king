#include "okage/bank.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace okage {
namespace {
unsigned u16(Bytes b,std::size_t at){if(at>b.size() || b.size()-at<2)throw std::runtime_error("Truncated XVAB halfword");return b[at]|(b[at+1]<<8);}
unsigned u32(Bytes b,std::size_t at){return u16(b,at)|(u16(b,at+2)<<16);}
std::int16_t s16(Bytes b,std::size_t at){return __builtin_bit_cast(std::int16_t, (std::uint16_t(u16(b,at))));}
struct Chunk{unsigned at,size;};
}
MusicBank read_music_bank(Bytes b){
    if(b.size()<12 || b.size()>4*1024*1024 || u32(b,0)!=0x42415658 || u32(b,4)!=0x101)throw std::runtime_error("Unsupported XVAB");
    const auto end=u32(b,8);if(end<12 || end>b.size())throw std::runtime_error("XVAB metadata extent");
    std::map<unsigned,Chunk> chunks;
    for(unsigned at=12;at<end;){
        if(end-at<8)throw std::runtime_error("Truncated XVAB chunk");
        const auto id=u32(b,at),size=u32(b,at+4);
        if(size>end-at-8 || !chunks.emplace(id,Chunk{at+8,size}).second)throw std::runtime_error("XVAB chunk extent/duplicate");
        at+=size+8;
    }
    auto chunk=[&](unsigned id)->Chunk {auto it=chunks.find(id);if(it==chunks.end())throw std::runtime_error("Missing XVAB chunk");return it->second;};
    const auto head=chunk(0x44414548),table=chunk(0x46494754),program=chunk(0x47504754),tone=chunk(0x50534754);
    const auto vel=chunk(0x544c4556),pan=chunk(0x544e4150);
    const auto body=(end+15)&~15u;
    if(head.size!=28 || b[head.at]!=0 || body>b.size() || b.size()-body!=u32(b,head.at+4) || (b.size()-body)%16)
        throw std::runtime_error("XVAB mapped header/body extent");
    if(u32(b,head.at+8)!=table.at || table.size<4 || table.size!=4+4*u16(b,table.at) ||
       u32(b,head.at+16)!=vel.at || u32(b,head.at+20)!=pan.at || vel.size<4 || pan.size<4 ||
       vel.size!=4+128*u16(b,vel.at) || pan.size!=4+256*u16(b,pan.at))throw std::runtime_error("XVAB table extent");
    MusicBank bank;
    for(unsigned i=0;i<u16(b,vel.at);++i){std::array<std::uint8_t,128> v;std::copy_n(b.begin()+vel.at+4+i*128,128,v.begin());bank.velocities.push_back(v);}
    for(unsigned i=0;i<u16(b,pan.at);++i){std::array<std::uint8_t,256> p;std::copy_n(b.begin()+pan.at+4+i*256,256,p.begin());bank.pans.push_back(p);}
    for(unsigned i=0;i<u16(b,table.at);++i){
        const auto p=u32(b,table.at+4+i*4);
        if(p<program.at || p>program.at+program.size || program.at+program.size-p<8)throw std::runtime_error("XVAB program bounds");
        const auto count=u16(b,p+2),first=u32(b,p+4);
        if(first<tone.at || first>tone.at+tone.size || count>(tone.at+tone.size-first)/36)throw std::runtime_error("XVAB tone bounds");
        std::vector<MusicTone> tones;
        for(unsigned j=0;j<count;++j){
            const auto at=first+j*36;MusicTone t;
            t.flags=b[at];t.priority=b[at+1];t.exclusive=b[at+2];t.key_low=b[at+4];t.key_high=b[at+5];t.center=b[at+6];t.fine=b[at+7];
            t.velocity_table=b[at+8];t.pan_table=b[at+9];t.left=__builtin_bit_cast(std::int8_t, (b[at+10]));t.right=__builtin_bit_cast(std::int8_t, (b[at+11]));
            t.pitch_scale=s16(b,at+12);t.adsr1=u16(b,at+16);t.adsr2=u16(b,at+18);t.sample_offset=u32(b,at+20);
            std::copy_n(b.begin()+at+24,12,t.modulation.begin());
            if(t.key_low>t.key_high || t.key_high>127 || t.center>127 || t.fine>127 || t.velocity_table>=bank.velocities.size() || t.pan_table>=bank.pans.size() ||
                t.sample_offset>=b.size()-body || t.sample_offset%16)throw std::runtime_error("XVAB tone fields/sample pointer");
            if(!bank.samples.contains(t.sample_offset)){
                auto sample=decode_spu_sample(b.subspan(body+t.sample_offset));
                bank.sample_blocks.emplace(t.sample_offset,std::vector<std::uint8_t>(b.begin()+body+t.sample_offset,b.begin()+body+t.sample_offset+sample.consumed));
                bank.samples.emplace(t.sample_offset,std::move(sample));
            }
            tones.push_back(t);
        }
        bank.programs.push_back(std::move(tones));
    }
    return bank;
}
std::uint16_t music_note_pitch(unsigned center,unsigned fine,unsigned key,unsigned key_fine){
    if(center>127 || fine>127 || key>127 || key_fine>127)throw std::runtime_error("Music pitch range");
    // LIBSD0x33a0: floored semitone/fraction tables and rounded octave division.
    // Generate the mathematical tables independently; no extracted binary table is shipped.
    static const auto tables=[] {
        std::array<std::uint32_t,140> t{};
        for(unsigned i=0;i<12;++i)t[i]=std::uint32_t(32768*std::exp2(double(i)/12));
        for(unsigned i=0;i<128;++i)t[12+i]=std::uint32_t(32768*std::exp2(double(i)/1536));
        return t;
    }();
    const auto fraction=fine+key_fine;
    int semitone=int(key)+int(fraction/128)-int(center),octave=semitone/12;
    semitone%=12;if(semitone<0){semitone+=12;--octave;}
    auto pitch=(tables[semitone]*tables[12+fraction%128])>>16;
    const auto shift=2-octave;if(shift>0)pitch=(pitch+(1u<<(shift-1)))>>shift;
    return pitch;
}
MusicToneParameters music_tone_parameters(const MusicBank& bank,const MusicTone& tone,unsigned key,unsigned velocity,const MusicChannel& c,unsigned gl,unsigned gr){
    if(key>127 || velocity>127 || c.volume>127 || c.expression>127 || c.pitch>127 || c.pan>127 || c.left>128 || c.right>128 || gl>128 || gr>128)throw std::runtime_error("Music note parameters out of range");
    const auto& vel=bank.velocities.at(tone.velocity_table);const auto& pan=bank.pans.at(tone.pan_table);
    const int volume=(c.volume*c.expression)>>7;
    auto side=[&](int gain,int global,int tone_volume,int pan_value){
        int v=(((volume*gain)>>7)*global)>>7; // Track gain remains original default128.
        return std::int16_t((((((v*tone_volume)>>7)*vel[velocity])>>7)*pan_value)>>7);
    };
    MusicToneParameters p;p.left=side(c.left,gl,tone.left,pan[c.pan]);p.right=side(c.right,gr,tone.right,pan[c.pan+128]);
    int pitch=int(key)*128;const auto bend=(int(c.pitch)-64)*64;
    if(bend)pitch+=(tone.modulation[bend<0 ? 9 : 10]*128*bend)>>12;
    pitch=std::clamp(pitch,0,0x3fff);p.key=pitch>>7;p.key_fine=pitch&127;p.center=tone.center;p.fine=tone.fine;p.pitch_scale=tone.pitch_scale;
    return p;
}
}
