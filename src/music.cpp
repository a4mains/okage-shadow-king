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
    const auto p=music_tone_parameters(*v.bank,*v.tone,v.key,v.velocity,channel,left_,right_);
    const auto pitch=std::uint16_t((music_note_pitch(p.center,p.fine,p.key,p.key_fine)*std::int32_t(p.pitch_scale))>>12);
    v.sample.parameters(pitch,p.left,p.right);
}
void DryMusicPlayer::note(const MusicEvent& e){
    if(e.parameters.bank>=banks_.size()||!banks_[e.parameters.bank])
        throw std::runtime_error("Music note requests an unbound bank");
    const auto& bank=*banks_[e.parameters.bank];
    const auto& program=bank.programs.at(e.parameters.program);
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
        auto v=std::make_unique<Voice>(e.channel,e.key,e.value,bank,tone,bank.sample_blocks.at(tone.sample_offset));
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
MusicResourceNames music_resource_names(int id) {
    switch(id) {
        case 2:return {"CTY_TNB","CTY_TNB0",""};
        case 10:return {"FLD_NMA","FLD_NMA0",""};
        case 26:return {"SNT_DNM","SNT_DNM0","SNT_DNM2"};
        case 51:return {"EVS_MSH","EVS_MSH0",""};
        default:throw std::runtime_error("Unreviewed music resource ID "+std::to_string(id));
    }
}
namespace {
void require_sequence_music(int id) {
    if((id>=20&&id<=23)||id==33||id==34)
        throw std::runtime_error("Streamed music routing is unrecovered");
    if(id==25||id==27||id==30||id==32)
        throw std::runtime_error("Alternate music playback is unrecovered");
}
}
void MusicRouter::stop() {
    // 0x12d470 cancels pending starts/volume writes as well as active playback.
    player_.reset();current_=active_slot_=-1;
    volume.current=volume.target=0;volume.stopped=true;
}
void MusicRouter::unload(unsigned slot) {
    if(slot>=slots_.size())throw std::runtime_error("Music sequence slot bounds");
    if(active_slot_==int(slot))stop();
    slots_[slot].reset();ids_[slot]=-1;
}
void MusicRouter::install(unsigned slot,int id,MusicResource resource) {
    if(slot>=slots_.size()||id<0)throw std::runtime_error("Music resource binding bounds");
    require_sequence_music(id);
    // Validate preloaded sequences even when they are not the playing score.
    MusicSequence primary(resource.sequence);
    if(!resource.alternate.empty()){MusicSequence alternate(resource.alternate);}
    if(slot==0&&current_!=-1)stop();
    unload(slot);slots_[slot]=std::move(resource);ids_[slot]=id;
}
void MusicRouter::ensure(unsigned slot,int id,const Loader& load) {
    require_sequence_music(id);
    if(ids_.at(slot)==id)return;
    if(!load)throw std::runtime_error("Music resource loader is not bound");
    install(slot,id,load(id));
}
void MusicRouter::enter(int primary,int secondary,const Loader& load) {
    if(primary>0&&ids_[0]!=primary&&current_!=-1)volume.request(0,8);
    if(secondary>0&&ids_[1]!=secondary)unload(1);
    if(primary<0)stop();
    if(primary>0){ensure(0,primary,load);select(primary);}
    if(secondary>0)ensure(1,secondary,load);
}
void MusicRouter::select(int id,bool fade) {
    require_sequence_music(id);
    // 0x12d580 chooses the last matching loaded sequence. A missing ordinary
    // ID stops playback; it does not implicitly load a resource or keep music.
    if(current_!=id) {
        stop();int selected=-1;
        for(unsigned i=0;i<slots_.size();++i)if(slots_[i]&&ids_[i]==id)selected=int(i);
        if(selected!=-1) {
            const auto& resource=*slots_[selected];
            player_=std::make_unique<DryMusicPlayer>(resource.sequence,resource.bank,resource.second);
            current_=id;active_slot_=selected;
            volume.current=volume.target=(fade?0:128)*65536;volume.stopped=false;
        }
    }
    // 0x130a30 applies this request even when selection did not restart music.
    if(fade)volume.request(128,8);
}
void MusicRouter::render(std::span<std::int16_t> stereo) {
    if(stereo.size()%2)throw std::runtime_error("Stereo frame extent");
    volume.tick();
    if(volume.stopped&&player_)stop();
    if(player_){player_->volume(volume.level(),volume.level());player_->render(stereo);}
    else std::fill(stereo.begin(),stereo.end(),0);
}
}
