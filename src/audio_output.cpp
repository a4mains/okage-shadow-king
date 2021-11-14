#include "okage/audio_output.hpp"
#include "okage/audio_ring.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

namespace okage {
struct AudioOutput::Impl {
    static constexpr unsigned block=AudioPcmRing::block;
    AudioQueueRef queue=nullptr;
    std::array<AudioQueueBufferRef,3> buffers{};
    AudioPcmRing ring;
    std::atomic<OSStatus> failure{0};
    std::atomic<std::uint64_t> enqueue_failures{0};
    std::uint64_t startup_refills=0; // Producer thread only.
    bool started=false;
    static void callback(void* context,AudioQueueRef queue,AudioQueueBufferRef buffer){
        auto& s=*static_cast<Impl*>(context);
        s.ring.refill(std::span<std::int16_t,block*2>(static_cast<std::int16_t*>(buffer->mAudioData),block*2));
        buffer->mAudioDataByteSize=block*4;
        const auto result=AudioQueueEnqueueBuffer(queue,buffer,0,nullptr);
        if(result){s.enqueue_failures.fetch_add(1,std::memory_order_relaxed);s.failure.store(result,std::memory_order_relaxed);}
    }
    ~Impl(){if(queue){AudioQueueStop(queue,true);AudioQueueDispose(queue,true);}}
};
static void check_audio(OSStatus result){if(result)throw std::runtime_error("macOS audio error "+std::to_string(result));}
AudioOutput::AudioOutput():impl_(std::make_unique<Impl>()){
    AudioStreamBasicDescription format{};format.mSampleRate=48000;format.mFormatID=kAudioFormatLinearPCM;
    format.mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked;format.mBytesPerPacket=4;
    format.mFramesPerPacket=1;format.mBytesPerFrame=4;format.mChannelsPerFrame=2;format.mBitsPerChannel=16;
    check_audio(AudioQueueNewOutput(&format,&Impl::callback,impl_.get(),nullptr,nullptr,0,&impl_->queue));
    for(auto& buffer:impl_->buffers)check_audio(AudioQueueAllocateBuffer(impl_->queue,Impl::block*4,&buffer));
}
AudioOutput::~AudioOutput()=default;
void AudioOutput::submit(std::span<const std::int16_t> stereo){
    auto& s=*impl_;check_audio(s.failure.load());s.ring.submit(stereo);
    if(!s.started && s.ring.stats().ring_frames>=Impl::block*s.buffers.size()){
        for(auto buffer:s.buffers){Impl::callback(&s,s.queue,buffer);++s.startup_refills;}
        check_audio(s.failure.load());check_audio(AudioQueueStart(s.queue,nullptr));s.started=true;
    }
}
AudioOutputStats AudioOutput::stats() const {
    const auto& s=*impl_;auto result=s.ring.stats();
    result.startup_refills=s.startup_refills;result.started=s.started;
    result.enqueue_failures=s.enqueue_failures.load(std::memory_order_relaxed);
    result.enqueue_status=s.failure.load(std::memory_order_relaxed);return result;
}
std::uint64_t AudioOutput::delivered_frames() const{return stats().source_frames_enqueued;}
std::uint64_t AudioOutput::underrun_frames() const{return stats().silence_frames_enqueued;}
}
