#pragma once
#include <cstdint>
#include <memory>
#include <span>
namespace okage {
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    void submit(std::span<const std::int16_t> stereo);
    std::uint64_t delivered_frames() const;
    std::uint64_t underrun_frames() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
