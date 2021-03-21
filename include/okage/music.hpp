#pragma once
#include "okage/bank.hpp"
#include "okage/voice.hpp"
#include <memory>
#include <optional>
#include <functional>

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
    DryMusicPlayer(Bytes sequence,MusicBank bank,std::optional<MusicBank> second={}):
        banks_{std::move(bank),std::move(second)},sequence_(sequence){}
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
        const MusicBank* bank;
        const MusicTone* tone;
        SampleVoice sample;
        Voice(unsigned c,unsigned k,unsigned v,const MusicBank& source,const MusicTone& t,Bytes b):
            channel(c),key(k),velocity(v),bank(&source),tone(&t),sample(b,t.adsr1,t.adsr2){}
    };
    std::array<std::optional<MusicBank>,2> banks_;
    MusicSequence sequence_;
    std::array<std::unique_ptr<Voice>,48> voices_;
    unsigned ticks_=0,within_tick_=0,next_voice_=0,peak_=0,stolen_=0,left_=128,right_=128;
    std::uint64_t retired_loops_=0;
    void driver_tick();
    void update(Voice&);
    void note(const MusicEvent&);
};
struct MusicResource {
    std::vector<std::uint8_t> sequence;
    MusicBank bank;
    std::optional<MusicBank> second;
    // Loaded with the secondary score; playback remains a separate service.
    std::vector<std::uint8_t> alternate;
};
struct MusicResourceNames {const char* sequence;const char* primary_bank;const char* alternate;};
// Reviewed entries from retail table0x1fb530, not a fabricated filename rule.
MusicResourceNames music_resource_names(int id);
class MusicRouter {
public:
    using Loader=std::function<MusicResource(int)>;
    MusicVolumeRamp volume;
    void install(unsigned slot,int id,MusicResource resource);
    void enter(int primary,int secondary,const Loader& load);
    void select(int id,bool fade=false);
    void stop();
    void render(std::span<std::int16_t> stereo);
    int loaded_id(unsigned slot) const{return ids_.at(slot);}
    int current_id() const{return current_;}
    int active_slot() const{return active_slot_;}
    const DryMusicPlayer* player() const{return player_.get();}
private:
    std::array<std::optional<MusicResource>,2> slots_;
    std::array<int,2> ids_={-1,-1};
    int current_=-1,active_slot_=-1;
    std::unique_ptr<DryMusicPlayer> player_;
    void unload(unsigned slot);
    void ensure(unsigned slot,int id,const Loader& load);
};
}
