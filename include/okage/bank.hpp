#pragma once
#include "okage/audio.hpp"
#include "okage/sequence.hpp"
#include <array>
#include <map>

namespace okage {
struct MusicTone {
    std::uint8_t flags=0,priority=0,exclusive=0,key_low=0,key_high=0,center=0,fine=0;
    std::uint8_t velocity_table=0,pan_table=0;
    std::int8_t left=0,right=0;
    std::int16_t pitch_scale=0;
    std::uint16_t adsr1=0,adsr2=0;
    std::uint32_t sample_offset=0;
    std::array<std::uint8_t,12> modulation{};
};
struct MusicBank {
    std::vector<std::vector<MusicTone>> programs;
    std::vector<std::array<std::uint8_t,128>> velocities;
    std::vector<std::array<std::uint8_t,256>> pans;
    std::map<std::uint32_t,DecodedSample> samples;
};
// Version0x101 XVAB in file-relative (unmapped) form. Does not register an IOP bank.
MusicBank read_music_bank(Bytes bytes);
std::uint16_t music_note_pitch(unsigned center,unsigned fine,unsigned key,unsigned key_fine);
struct MusicToneParameters {
    std::int16_t left=0,right=0;
    std::int16_t pitch_scale=4096;
    std::uint8_t center=0,fine=0,key=0,key_fine=0;
};
// Recovered music-volume and tone stages (RXSNDDRV0x48f4/0x83c0).
// Output volumes precede the SPU register conversion; pitch still needs Note2Pitch.
MusicToneParameters music_tone_parameters(const MusicBank&,const MusicTone&,unsigned key,unsigned velocity,const MusicChannel&,unsigned global_left=128,unsigned global_right=128);
}
