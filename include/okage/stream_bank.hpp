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
    explicit StreamPlayer(StreamBank bank):bank_(std::move(bank)){}
    StreamPlayer(const StreamPlayer&)=delete;
    StreamPlayer& operator=(const StreamPlayer&)=delete;
    void play(int index,float speed,unsigned volume);
    void play_signed(int index,float speed,int volume);
    void mix(std::span<std::int32_t> stereo);
    void stop(){voices_.clear();}
    unsigned active() const{return voices_.size();}
    unsigned records() const{return bank_.records.size();}
private:
    struct Playing {
        std::vector<std::uint8_t> sample;
        SampleVoice voice;
        Playing(std::vector<std::uint8_t> data,const StreamTone& tone)
            :sample(std::move(data)),voice(sample,StreamTone::adsr1,StreamTone::adsr2){
            voice.parameters(tone.pitch,tone.left,tone.right);
        }
    };
    StreamBank bank_;
    std::vector<std::unique_ptr<Playing>> voices_;
    void start(const StreamTone&);
};
}
