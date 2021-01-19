#pragma once
#include "okage/audio.hpp"

namespace okage {
// Host software envelope and sample playback; no guest instruction execution.
class VoiceEnvelope {
public:
    enum class Phase {stopped,attack,decay,sustain,release};
    VoiceEnvelope(std::uint16_t adsr1,std::uint16_t adsr2):a_(adsr1),b_(adsr2){}
    int tick();
    void release(){if(phase_!=Phase::stopped){phase_=Phase::release;counter_=0;}}
    void stop(){phase_=Phase::stopped;level_=0;}
    int level() const{return level_;}
    Phase phase() const{return phase_;}
    unsigned counter() const{return counter_;}
private:
    std::uint16_t a_,b_;
    Phase phase_=Phase::attack;
    int level_=0;
    unsigned counter_=0;
};
using GaussianTable=std::array<std::array<std::int16_t,4>,256>;
const GaussianTable& gaussian_table();
std::int32_t interpolate_voice(std::span<const std::int16_t,4> samples,unsigned fraction);
class SampleVoice {
public:
    // The immutable compressed sample must outlive the voice.
    SampleVoice(Bytes blocks,std::uint16_t adsr1,std::uint16_t adsr2);
    std::array<std::int32_t,2> tick();
    void release(){envelope.release();}
    void parameters(unsigned pitch,int left_gain,int right_gain);
    bool ended() const{return envelope.phase()==VoiceEnvelope::Phase::stopped;}
    VoiceEnvelope envelope;
    std::uint64_t loop_count=0;
private:
    Bytes blocks_;
    AdpcmHistory history_;
    std::array<std::int16_t,28> decoded_{};
    std::array<std::int16_t,32> fifo_{};
    unsigned block_=0,repeat_=0,group_=0,read_=0,write_=0,fraction_=0,pitch_=4096;
    int left_=0,right_=0;
    void fill();
};
}
