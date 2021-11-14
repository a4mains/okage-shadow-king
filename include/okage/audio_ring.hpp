#pragma once
#include "okage/audio_output.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
namespace okage {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
class AudioPcmRing {
public:
    static constexpr unsigned capacity=16384,block=800;
    void submit(std::span<const std::int16_t> stereo) {
        if(stereo.size()%2)throw std::runtime_error("Audio output stereo extent");
        const auto end=written_.load(std::memory_order_relaxed),from=read_.load(std::memory_order_acquire);
        const auto count=stereo.size()/2;
        if(count>capacity-(end-from)){++overruns_;throw std::runtime_error("Native audio producer overrun");}
        for(std::size_t i=0;i<count;++i){ring_[((end+i)%capacity)*2]=stereo[i*2];ring_[((end+i)%capacity)*2+1]=stereo[i*2+1];}
        written_.store(end+count,std::memory_order_release);
    }
    void refill(std::span<std::int16_t,block*2> out) {
        const auto from=read_.load(std::memory_order_relaxed),end=written_.load(std::memory_order_acquire);
        const auto count=std::min<std::uint64_t>(block,end-from);
        for(unsigned i=0;i<count;++i){out[i*2]=ring_[((from+i)%capacity)*2];out[i*2+1]=ring_[((from+i)%capacity)*2+1];}
        std::fill(out.begin()+count*2,out.end(),0);
        read_.store(from+count,std::memory_order_release);
        delivered_.fetch_add(count,std::memory_order_relaxed);silence_.fetch_add(block-count,std::memory_order_relaxed);
        refills_.fetch_add(1,std::memory_order_relaxed);
        if(count<block)starved_.fetch_add(1,std::memory_order_relaxed);
    }
    AudioOutputStats stats() const {
        AudioOutputStats result;
        result.submitted_frames=written_.load(std::memory_order_relaxed);
        result.ring_frames=result.submitted_frames-read_.load(std::memory_order_acquire);
        result.source_frames_enqueued=delivered_.load(std::memory_order_relaxed);
        result.silence_frames_enqueued=silence_.load(std::memory_order_relaxed);
        result.refills=refills_.load(std::memory_order_relaxed);result.starved_refills=starved_.load(std::memory_order_relaxed);
        result.producer_overruns=overruns_;return result;
    }
private:
    std::array<std::int16_t,capacity*2> ring_{};
    std::atomic<std::uint64_t> written_{0},read_{0},delivered_{0},silence_{0},refills_{0},starved_{0};
    std::uint64_t overruns_=0;
};
}
