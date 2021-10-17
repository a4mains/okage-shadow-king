#include "okage/music.hpp"
#include <algorithm>
#include <stdexcept>

namespace okage {
void MusicChannelLevels::request(int first,int last,unsigned level) {
    if(first<0||first>15||last<0||last>15||level>128)
        throw std::runtime_error("Unreviewed music channel range/level");
    for(int i=first;i<=last;++i)values[i]=level;
    dirty=true;
}
std::optional<std::array<std::uint32_t,16>> MusicChannelLevels::take(bool active) {
    const bool send=dirty&&active;dirty=false;
    if(send)return values;
    return {};
}
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

struct DryMusicPlayer::VoicePool {
    std::array<std::unique_ptr<Voice>,48> voices;
    unsigned next_voice=0,within_tick=0;
};
std::shared_ptr<DryMusicPlayer::VoicePool> DryMusicPlayer::make_pool() {
    return std::make_shared<VoicePool>();
}
DryMusicPlayer::DryMusicPlayer(Bytes sequence,MusicBank bank,std::optional<MusicBank> second,
                               std::shared_ptr<VoicePool> pool):
    banks_{std::move(bank),std::move(second)},sequence_(sequence),pool_(pool?std::move(pool):make_pool()){}
DryMusicPlayer::~DryMusicPlayer() {
    for(auto& voice:pool_->voices)if(voice&&voice->owner==this)voice.reset();
}
bool DryMusicPlayer::finished() const {
    return sequence_.ended()&&std::none_of(pool_->voices.begin(),pool_->voices.end(),
        [this](const auto& voice){return voice&&voice->owner==this;});
}
void DryMusicPlayer::volume(unsigned left,unsigned right){
    if(left>128 || right>128)throw std::runtime_error("Music global volume range");
    left_=left;right_=right;for(auto& v:pool_->voices)if(v&&v->owner==this)update(*v);
}
void DryMusicPlayer::channel_volumes(const std::array<std::uint32_t,16>& levels) {
    if(std::any_of(levels.begin(),levels.end(),[](auto value){return value>128;}))
        throw std::runtime_error("Unreviewed music channel gain");
    pending_channels_=levels;
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
        for(unsigned i=0;i<48;++i){const auto index=(pool_->next_voice+i)%48;if(!pool_->voices[index]){chosen=index;pool_->next_voice=(index+1)%48;break;}}
        if(chosen==48){
            // Original0x3998 selects highest numeric priority, then greatest age.
            chosen=0;for(unsigned i=1;i<48;++i){const auto& a=*pool_->voices[i];const auto& b=*pool_->voices[chosen];
                if(a.tone->priority>b.tone->priority || (a.tone->priority==b.tone->priority && a.age>b.age))chosen=i;}
            pool_->voices[chosen]->owner->retired_loops_+=pool_->voices[chosen]->sample.loop_count;++stolen_;
        }
        auto v=std::make_unique<Voice>(this,e.channel,e.key,e.value,bank,tone,bank.sample_blocks.at(tone.sample_offset));
        // New notes receive final channel parameters below, after all this tick's events.
        pool_->voices[chosen]=std::move(v);
    }
}
void DryMusicPlayer::driver_tick(){
    ++ticks_;
    if(pending_channels_) {
        for(unsigned i=0;i<16;++i)sequence_.channels[i].gain((*pending_channels_)[i],(*pending_channels_)[i]);
        pending_channels_.reset();
    }
    for(const auto& e:sequence_.tick()){
        if(e.kind==MusicEvent::Kind::note_on)note(e);
        else if(e.kind==MusicEvent::Kind::note_off){for(auto& v:pool_->voices)if(v && v->owner==this && v->channel==e.channel && v->key==e.key)v->sample.release();}
    }
    unsigned count=0;for(auto& v:pool_->voices)if(v&&v->owner==this){update(*v);if(!v->sample.ended())++count;}
    peak_=std::max(peak_,count);
}
std::uint64_t DryMusicPlayer::sample_loops() const{
    auto count=retired_loops_;for(const auto& v:pool_->voices)if(v&&v->owner==this)count+=v->sample.loop_count;return count;
}
void DryMusicPlayer::render_pool(VoicePool& pool,std::span<std::int16_t> stereo,const std::function<void()>& tick){
    if(stereo.size()%2)throw std::runtime_error("Stereo frame extent");
    for(std::size_t i=0;i<stereo.size();i+=2){
        if(!pool.within_tick) {
            // Age/retire once for the shared hardware pool, not per sequence.
            for(auto& v:pool.voices)if(v){
                ++v->age;
                if(v->sample.ended()){
                    if(++v->silent_ticks>=9){v->owner->retired_loops_+=v->sample.loop_count;v.reset();}
                }else v->silent_ticks=0;
            }
            tick();
        }
        std::array<std::int32_t,2> sum{};
        for(auto& v:pool.voices)if(v){const auto sample=v->sample.tick();if(v->tone->flags&1){sum[0]+=sample[0];sum[1]+=sample[1];}}
        stereo[i]=std::clamp(sum[0],-32768,32767);stereo[i+1]=std::clamp(sum[1],-32768,32767);
        pool.within_tick=(pool.within_tick+1)%800;
    }
}
void DryMusicPlayer::render(std::span<std::int16_t> stereo){
    render_pool(*pool_,stereo,[this]{driver_tick();});
}
std::optional<unsigned> music_jingle_index(int id) {
    if(id>=20&&id<=23)return unsigned(id-20);
    if(id==33||id==34)return unsigned(id-29);
    return {};
}
MusicResourceNames music_resource_names(int id) {
    if(const auto jingle=music_jingle_index(id)) {
        static constexpr const char* names[]={"JGL_FLG","JGL_STN","JGL_GET","JGL_SPL","JGL_FVC","JGL_LUP"};
        return {names[*jingle],"JGL_BSC0",""};
    }
    switch(id) {
        case 2:return {"CTY_TNB","CTY_TNB0",""};
        case 10:return {"FLD_NMA","FLD_NMA0",""};
        case 26:return {"SNT_DNM","SNT_DNM0","SNT_DNM2"};
        // USA ELF table0x1fb530 entry36; ordinary sequence with one bank.
        case 36:return {"EVN_TSR","EVN_TSR0",""};
        // USA ELF table0x1fb530 entry38; EVN_THL1.XV is also loaded.
        case 38:return {"EVN_THL","EVN_THL0",""};
        // USA ELF table0x1fb530 entry39; ordinary sequence with one bank.
        case 39:return {"EVN_TQS","EVN_TQS0",""};
        // USA ELF table0x1fb530 entry46; ordinary sequence with one bank.
        case 46:return {"EVS_BMJ","EVS_BMJ0",""};
        // USA ELF table0x1fb530 entry47; ordinary sequence with one bank.
        case 47:return {"EVS_STM","EVS_STM0",""};
        // USA ELF table0x1fb530 entry50; ordinary loader0x12d158.
        case 50:return {"EVS_NRD","EVS_NRD0",""};
        case 51:return {"EVS_MSH","EVS_MSH0",""};
        default:throw std::runtime_error("Unreviewed music resource ID "+std::to_string(id));
    }
}
namespace {
void require_sequence_music(int id) {
    if((id>=20&&id<=23)||id==33||id==34)
        throw std::runtime_error("Jingle cannot occupy an ordinary score slot (music ID "+std::to_string(id)+")");
    if(id==25||id==27||id==30||id==32)
        throw std::runtime_error("Alternate music playback is unrecovered");
}
}
void MusicRouter::stop() {
    // 0x12d470 stops the ordinary score and cancels pending writes. Jingle
    // instances and the last forwarded hardware gains are independent.
    player_.reset();current_=active_slot_=-1;
    channels.cancel();
    volume.current=volume.target=0;volume.stopped=true;
}
void MusicRouter::unload(unsigned slot) {
    if(slot>=slots_.size())throw std::runtime_error("Music sequence slot bounds");
    if(active_slot_==int(slot))stop();
    slots_[slot].reset();ids_[slot]=-1;
}
void MusicRouter::install_jingle(unsigned slot,MusicResource resource) {
    if(slot>=jingle_slots_.size()||resource.second||!resource.alternate.empty())
        throw std::runtime_error("Invalid shared-bank jingle resource");
    MusicSequence check(resource.sequence);
    jingle_slots_[slot]=std::move(resource);
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
void MusicRouter::load_secondary(int id,const Loader& load) {
    if(id<=0)throw std::runtime_error("Event secondary music requires an explicit positive ID");
    require_sequence_music(id);
    stop();
    ensure(1,id,load);
}
void MusicRouter::select(int id,bool fade) {
    if(const auto jingle=music_jingle_index(id)) {
        if(!jingle_slots_[*jingle])throw std::runtime_error("Jingle sequence is not loaded");
        const auto& resource=*jingle_slots_[*jingle];
        jingles_.push_back(std::make_unique<DryMusicPlayer>(resource.sequence,resource.bank,std::nullopt,pool_));
        // 0x12e0b0 neither stops the ordinary score nor assigns its current ID.
        driver_gain_=128;volume.current=volume.target=0x800000;volume.stopped=false;
        if(fade)volume.request(128,8);
        return;
    }
    require_sequence_music(id);
    // 0x12d580 chooses the last matching loaded sequence. A missing ordinary
    // ID stops playback; it does not implicitly load a resource or keep music.
    if(current_!=id) {
        stop();int selected=-1;
        for(unsigned i=0;i<slots_.size();++i)if(slots_[i]&&ids_[i]==id)selected=int(i);
        if(selected!=-1) {
            const auto& resource=*slots_[selected];
            player_=std::make_unique<DryMusicPlayer>(resource.sequence,resource.bank,resource.second,pool_);
            current_=id;active_slot_=selected;
            driver_gain_=fade?0:128;volume.current=volume.target=driver_gain_*65536;volume.stopped=false;
        }
    }
    // 0x130a30 applies this request even when selection did not restart music.
    if(fade)volume.request(128,8);
}
void MusicRouter::render(std::span<std::int16_t> stereo) {
    if(stereo.size()%2)throw std::runtime_error("Stereo frame extent");
    const bool send_volume=volume.current!=volume.target;
    volume.tick();
    if(send_volume)driver_gain_=volume.level();
    if(volume.stopped&&player_)stop();
    if(const auto levels=channels.take(bool(player_)))player_->channel_volumes(*levels);
    if(player_)player_->volume(driver_gain_,driver_gain_);
    for(auto& jingle:jingles_)jingle->volume(driver_gain_,driver_gain_);
    DryMusicPlayer::render_pool(*pool_,stereo,[this]{
        if(player_)player_->driver_tick();
        for(auto& jingle:jingles_)jingle->driver_tick();
    });
    std::erase_if(jingles_,[](const auto& jingle){return jingle->finished();});
}
}
