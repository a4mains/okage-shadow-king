#include "okage/music.hpp"
#include <algorithm>
#include <stdexcept>

namespace okage {
void MusicVolumeRamp::request(unsigned level,unsigned rate){
    if(level>128 || rate>32767)throw std::runtime_error("Music volume ramp range");
    target=level*65536;step=rate*4096;stopped=false;
}
void MusicVolumeRamp::tick(){
    // Original0x12ce20 clamps at0/128, not at target. Preserve overshoot behavior.
    if(current==target)return;
    if(target<current){current-=step;if(current<1){current=target=0;stopped=true;}}
    else current=std::min(current+step,0x800000);
}

void DryMusicPlayer::volume(unsigned left,unsigned right){
    if(left>128 || right>128)throw std::runtime_error("Music global volume range");
    left_=left;right_=right;for(auto& v:voices_)if(v)update(*v);
}
void DryMusicPlayer::update(Voice& v){
    const auto& channel=sequence_.channels[v.channel];
    if(channel.flags || channel.modulation)throw std::runtime_error("Music pedal/mono/portamento/modulation playback not recovered");
    const auto p=music_tone_parameters(bank_,*v.tone,v.key,v.velocity,channel,left_,right_);
    const auto pitch=std::uint16_t((music_note_pitch(p.center,p.fine,p.key,p.key_fine)*std::int32_t(p.pitch_scale))>>12);
    v.sample.parameters(pitch,p.left,p.right);
}
void DryMusicPlayer::note(const MusicEvent& e){
    if(e.parameters.bank!=0)throw std::runtime_error("Music bank switching not recovered");
    const auto& program=bank_.programs.at(e.parameters.program);
    for(const auto& tone:program){
        if(e.key<tone.key_low || e.key>tone.key_high)continue;
        if(tone.flags&~3u)throw std::runtime_error("Music noise/pitch-modulated tone not recovered");
        unsigned chosen=48;
        for(unsigned i=0;i<48;++i){const auto index=(next_voice_+i)%48;if(!voices_[index]){chosen=index;next_voice_=(index+1)%48;break;}}
        if(chosen==48){
            // Original0x3998 selects highest numeric priority, then greatest age.
            chosen=0;for(unsigned i=1;i<48;++i){const auto& a=*voices_[i];const auto& b=*voices_[chosen];
                if(a.tone->priority>b.tone->priority || (a.tone->priority==b.tone->priority && a.age>b.age))chosen=i;}
            retired_loops_+=voices_[chosen]->sample.loop_count;++stolen_;
        }
        auto v=std::make_unique<Voice>(e.channel,e.key,e.value,tone,bank_.sample_blocks.at(tone.sample_offset));
        // New notes receive final channel parameters below, after all this tick's events.
        voices_[chosen]=std::move(v);
    }
}
void DryMusicPlayer::driver_tick(){
    ++ticks_;
    // The driver retains a silent hardware voice for nine updates before freeing it.
    for(auto& v:voices_)if(v){
        ++v->age;
        if(v->sample.ended()){
            if(++v->silent_ticks>=9){retired_loops_+=v->sample.loop_count;v.reset();}
        }else v->silent_ticks=0;
    }
    for(const auto& e:sequence_.tick()){
        if(e.kind==MusicEvent::Kind::note_on)note(e);
        else if(e.kind==MusicEvent::Kind::note_off){for(auto& v:voices_)if(v && v->channel==e.channel && v->key==e.key)v->sample.release();}
    }
    unsigned count=0;for(auto& v:voices_)if(v){update(*v);if(!v->sample.ended())++count;}
    peak_=std::max(peak_,count);
}
std::uint64_t DryMusicPlayer::sample_loops() const{
    auto count=retired_loops_;for(const auto& v:voices_)if(v)count+=v->sample.loop_count;return count;
}
void DryMusicPlayer::render(std::span<std::int16_t> stereo){
    if(stereo.size()%2)throw std::runtime_error("Stereo frame extent");
    for(std::size_t i=0;i<stereo.size();i+=2){
        if(!within_tick_)driver_tick();
        std::array<std::int32_t,2> sum{};
        for(auto& v:voices_)if(v){const auto sample=v->sample.tick();if(v->tone->flags&1){sum[0]+=sample[0];sum[1]+=sample[1];}}
        stereo[i]=std::clamp(sum[0],-32768,32767);stereo[i+1]=std::clamp(sum[1],-32768,32767);
        within_tick_=(within_tick_+1)%800;
    }
}
}
