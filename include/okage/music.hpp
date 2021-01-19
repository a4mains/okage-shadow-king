#pragma once
#include "okage/bank.hpp"
#include "okage/voice.hpp"
#include <memory>

namespace okage {
struct MusicVolumeRamp {
    std::int32_t current=0x800000,target=0x800000,step=0;
    bool stopped=false;
    void request(unsigned level,unsigned rate);
    void tick();
    unsigned level() const{return unsigned(current)>>16;}
};
class DryMusicPlayer {
public:
    DryMusicPlayer(Bytes sequence,MusicBank bank):bank_(std::move(bank)),sequence_(sequence){}
    DryMusicPlayer(const DryMusicPlayer&)=delete;
    DryMusicPlayer& operator=(const DryMusicPlayer&)=delete;
    void render(std::span<std::int16_t> interleaved_stereo);
    void volume(unsigned left,unsigned right);
    unsigned driver_ticks() const{return ticks_;}
    unsigned peak_voices() const{return peak_;}
    unsigned stolen_voices() const{return stolen_;}
    std::uint64_t sample_loops() const;
private:
    struct Voice {
        unsigned channel,key,velocity,age=0,silent_ticks=0;
        const MusicTone* tone;
        SampleVoice sample;
        Voice(unsigned c,unsigned k,unsigned v,const MusicTone& t,Bytes b):channel(c),key(k),velocity(v),tone(&t),sample(b,t.adsr1,t.adsr2){}
    };
    MusicBank bank_;
    MusicSequence sequence_;
    std::array<std::unique_ptr<Voice>,48> voices_;
    unsigned ticks_=0,within_tick_=0,next_voice_=0,peak_=0,stolen_=0,left_=128,right_=128;
    std::uint64_t retired_loops_=0;
    void driver_tick();
    void update(Voice&);
    void note(const MusicEvent&);
};
}
