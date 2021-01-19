#include "okage/audio_output.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

namespace okage {
struct AudioOutput::Impl {
    static constexpr unsigned capacity=16384,block=800;
    AudioQueueRef queue=nullptr;
    std::array<AudioQueueBufferRef,3> buffers{};
    std::array<std::int16_t,capacity*2> ring{};
    std::atomic<std::uint64_t> written{0},read{0},delivered{0},underruns{0};
    std::atomic<OSStatus> failure{0};
    bool started=false;
    static void callback(void* context,AudioQueueRef queue,AudioQueueBufferRef buffer){
        auto& s=*static_cast<Impl*>(context);
        const auto from=s.read.load(std::memory_order_relaxed),end=s.written.load(std::memory_order_acquire);
        const auto count=std::min<std::uint64_t>(block,end-from);auto* out=static_cast<std::int16_t*>(buffer->mAudioData);
        for(unsigned i=0;i<count;++i){out[i*2]=s.ring[((from+i)%capacity)*2];out[i*2+1]=s.ring[((from+i)%capacity)*2+1];}
        std::fill(out+count*2,out+block*2,0);buffer->mAudioDataByteSize=block*4;
        s.read.store(from+count,std::memory_order_release);s.delivered.fetch_add(count,std::memory_order_relaxed);
        s.underruns.fetch_add(block-count,std::memory_order_relaxed);
        const auto result=AudioQueueEnqueueBuffer(queue,buffer,0,nullptr);if(result)s.failure.store(result,std::memory_order_relaxed);
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
    if(stereo.size()%2)throw std::runtime_error("Audio output stereo extent");
    auto& s=*impl_;check_audio(s.failure.load());const auto end=s.written.load(std::memory_order_relaxed),from=s.read.load(std::memory_order_acquire);
    const auto count=stereo.size()/2;if(count>Impl::capacity-(end-from))throw std::runtime_error("Native audio producer overrun");
    for(std::size_t i=0;i<count;++i){s.ring[((end+i)%Impl::capacity)*2]=stereo[i*2];s.ring[((end+i)%Impl::capacity)*2+1]=stereo[i*2+1];}
    s.written.store(end+count,std::memory_order_release);
    if(!s.started && end+count-from>=Impl::block*s.buffers.size()){
        for(auto buffer:s.buffers)Impl::callback(&s,s.queue,buffer);
        check_audio(s.failure.load());check_audio(AudioQueueStart(s.queue,nullptr));s.started=true;
    }
}
std::uint64_t AudioOutput::delivered_frames() const{return impl_->delivered.load();}
std::uint64_t AudioOutput::underrun_frames() const{return impl_->underruns.load();}
}
