#pragma once
#include "okage/assets.hpp"
#include "okage/voice.hpp"
#include <array>
#include <memory>

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
    static constexpr std::uint16_t adsr1=0x8000,adsr2=0x8026;
};
// Recovered mono ADPCM, no MARK/BRCH or loop behavior. Unsupported forms throw.
StreamTone stream_tone(const StreamBank&,int index,float speed=1,unsigned volume=128);
class StreamPlayer {
public:
    explicit StreamPlayer(StreamBank bank):bank_(std::move(bank)){}
    StreamPlayer(const StreamPlayer&)=delete;
    StreamPlayer& operator=(const StreamPlayer&)=delete;
    void play(int index,float speed,unsigned volume);
    void mix(std::span<std::int32_t> stereo);
    void stop(){voices_.clear();}
    unsigned active() const{return voices_.size();}
    unsigned records() const{return bank_.records.size();}
private:
    StreamBank bank_;
    std::vector<std::unique_ptr<SampleVoice>> voices_;
};
}
