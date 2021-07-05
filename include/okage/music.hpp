#pragma once
#include "okage/bank.hpp"
#include "okage/voice.hpp"
#include <memory>
#include <optional>
#include <functional>

namespace okage {
struct MusicChannelLevels {
    // EE0x2d9a08 is retained across stops; fresh BSS is zero, unlike the
    // sequencer's initial per-channel gains of128.
    std::array<std::uint32_t,16> values{};
    bool dirty=false;
    void request(int first,int last,unsigned level);
    void cancel(){dirty=false;}
    std::optional<std::array<std::uint32_t,16>> take(bool active);
};
struct MusicVolumeRamp {
    std::int32_t current=0x800000,target=0x800000,step=0;
    bool stopped=false;
    void request(unsigned level,unsigned rate);
    void tick();
    unsigned level() const{return unsigned(current)>>16;}
};
class DryMusicPlayer {
public:
    struct VoicePool;
    static std::shared_ptr<VoicePool> make_pool();
    DryMusicPlayer(Bytes sequence,MusicBank bank,std::optional<MusicBank> second={},
                   std::shared_ptr<VoicePool> pool={});
    ~DryMusicPlayer();
    DryMusicPlayer(const DryMusicPlayer&)=delete;
    DryMusicPlayer& operator=(const DryMusicPlayer&)=delete;
    void render(std::span<std::int16_t> interleaved_stereo);
    void volume(unsigned left,unsigned right);
    void channel_volumes(const std::array<std::uint32_t,16>& levels);
    unsigned driver_ticks() const{return ticks_;}
    unsigned peak_voices() const{return peak_;}
    unsigned stolen_voices() const{return stolen_;}
    std::uint64_t sample_loops() const;
    bool finished() const;
    void driver_tick();
    static void render_pool(VoicePool&,std::span<std::int16_t>,const std::function<void()>&);
private:
    struct Voice {
        DryMusicPlayer* owner;
        unsigned channel,key,velocity,age=0,silent_ticks=0;
        const MusicBank* bank;
        const MusicTone* tone;
        SampleVoice sample;
        Voice(DryMusicPlayer* o,unsigned c,unsigned k,unsigned v,const MusicBank& source,const MusicTone& t,Bytes b):
            owner(o),channel(c),key(k),velocity(v),bank(&source),tone(&t),sample(b,t.adsr1,t.adsr2){}
    };
    std::array<std::optional<MusicBank>,2> banks_;
    MusicSequence sequence_;
    std::optional<std::array<std::uint32_t,16>> pending_channels_;
    std::shared_ptr<VoicePool> pool_;
    unsigned ticks_=0,peak_=0,stolen_=0,left_=128,right_=128;
    std::uint64_t retired_loops_=0;
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
// Reviewed table0x1fb530 entries plus the six names loaded by0x12cbb8.
MusicResourceNames music_resource_names(int id);
std::optional<unsigned> music_jingle_index(int id);
class MusicRouter {
public:
    using Loader=std::function<MusicResource(int)>;
    MusicVolumeRamp volume;
    MusicChannelLevels channels;
    void install(unsigned slot,int id,MusicResource resource);
    void install_jingle(unsigned slot,MusicResource resource);
    void enter(int primary,int secondary,const Loader& load);
    // Event task0x12fec8 stops the ordinary score, then0x12d898 ensures slot1.
    // Completion scheduling belongs to the caller, not this resource operation.
    void load_secondary(int id,const Loader& load);
    void select(int id,bool fade=false);
    void stop();
    void render(std::span<std::int16_t> stereo);
    int loaded_id(unsigned slot) const{return ids_.at(slot);}
    int current_id() const{return current_;}
    int active_slot() const{return active_slot_;}
    const DryMusicPlayer* player() const{return player_.get();}
    unsigned active_jingles() const{return jingles_.size();}
private:
    std::array<std::optional<MusicResource>,2> slots_;
    std::array<std::optional<MusicResource>,6> jingle_slots_;
    std::array<int,2> ids_={-1,-1};
    int current_=-1,active_slot_=-1;
    std::unique_ptr<DryMusicPlayer> player_;
    std::shared_ptr<DryMusicPlayer::VoicePool> pool_=DryMusicPlayer::make_pool();
    std::vector<std::unique_ptr<DryMusicPlayer>> jingles_;
    unsigned driver_gain_=128;
    void unload(unsigned slot);
    void ensure(unsigned slot,int id,const Loader& load);
};
}
