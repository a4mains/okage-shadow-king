#pragma once
#include <cstdint>
#include <memory>
#include <span>
namespace okage {
struct AudioOutputStats {
    std::uint64_t submitted_frames=0,source_frames_enqueued=0,silence_frames_enqueued=0;
    std::uint64_t ring_frames=0,refills=0,startup_refills=0,starved_refills=0;
    std::uint64_t producer_overruns=0,enqueue_failures=0;
    std::int32_t enqueue_status=0;
    bool started=false;
};
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    void submit(std::span<const std::int16_t> stereo);
    AudioOutputStats stats() const;
    std::uint64_t delivered_frames() const;
    std::uint64_t underrun_frames() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
