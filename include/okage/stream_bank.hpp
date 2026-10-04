#pragma once
#include "okage/assets.hpp"
#include "okage/voice.hpp"
#include <array>
#include <memory>
#include <optional>

namespace okage {
struct StreamBank {
    std::vector<std::uint8_t> bytes;
    // File-relative FORM, FTXT, MARK, BRCH and BODY payload offsets; zero if absent.
    std::array<std::uint32_t,5> chunks{};
    std::uint32_t body_size=0;
    std::vector<std::uint32_t> records;
    Bytes record(int index) const;
};
StreamBank read_stream_bank(Bytes);
struct StreamTone {
    std::uint32_t offset=0,size=0;
    unsigned sample_rate=0,pitch=0;
    int left=0,right=0;
    std::optional<std::uint32_t> loop_offset;
    static constexpr std::uint16_t adsr1=0x8000,adsr2=0x8026;
};
// Recovered mono ADPCM with an optional FORM content loop; no MARK/BRCH.
StreamTone stream_tone(const StreamBank&,int index,float speed=1,unsigned volume=128);
StreamTone stream_tone_signed(const StreamBank&,int index,float speed,int volume);
std::vector<std::uint8_t> prepare_stream_sample(Bytes,std::optional<std::uint32_t> loop_offset=std::nullopt);
class StreamPlayer {
public:
    explicit StreamPlayer(StreamBank bank);
    StreamPlayer(const StreamPlayer&)=delete;
    StreamPlayer& operator=(const StreamPlayer&)=delete;
    // Returns the request id 175398 assigns (12df80's voice handle).
    unsigned play(int index,float speed,unsigned volume);
    unsigned play_signed(int index,float speed,int volume);
    void mix(std::span<std::int32_t> stereo);
    // 12de90 -> 1756b0: stop and release one request; an inactive id is a no-op.
    void stop_voice(unsigned id);
    // 1756c0: release every active request, newest first.
    void stop();
    unsigned active() const{return voices_.size();}
    unsigned records() const{return bank_.records.size();}
private:
    struct Playing {
        std::vector<std::uint8_t> sample;
        SampleVoice voice;
        unsigned id=0;
        Playing(std::vector<std::uint8_t> data,const StreamTone& tone)
            :sample(std::move(data)),voice(sample,StreamTone::adsr1,StreamTone::adsr2){
            voice.parameters(tone.pitch,tone.left,tone.right);
        }
    };
    StreamBank bank_;
    // Active requests oldest-to-newest (the source list's head is the newest).
    std::vector<std::unique_ptr<Playing>> voices_;
    // 0x175a80: 256 request records, ids pushed 0..255; back() is the head.
    std::vector<unsigned> free_;
    unsigned start(const StreamTone&);
    void release_ended();
};
}
