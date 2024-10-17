#include "okage/animation.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#include <string>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int previous=std::fegetround();
    Rounding() { if (std::fesetround(FE_TOWARDZERO)) throw std::runtime_error("Animation rounding unavailable"); }
    ~Rounding() { std::fesetround(previous); }
};
}
std::uint32_t Animation::word(std::size_t at) const {
    if (at>bytes_.size() || bytes_.size()-at<4) throw std::runtime_error("Truncated XMT word");
    return std::uint32_t(bytes_[at])|(std::uint32_t(bytes_[at+1])<<8)|
        (std::uint32_t(bytes_[at+2])<<16)|(std::uint32_t(bytes_[at+3])<<24);
}
float Animation::number(std::size_t at) const {
    const auto value=std::bit_cast<float>(word(at));
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite XMT sample");
    return value;
}
float Animation::sample(std::uint64_t index) const {
    const auto at=std::uint64_t(samples_)+std::uint64_t(index)*4;
    if (at+4>sample_end_) throw std::runtime_error("XMT sample index out of bounds");
    return number(at);
}
std::uint32_t Animation::relative(std::uint32_t at) const {
    if (at<code_ || std::uint64_t(at)+4>samples_) throw std::runtime_error("Truncated XMT relative offset");
    const auto target=std::int64_t(at)+std::bit_cast<std::int32_t>(word(at));
    if (target<code_ || target>=samples_ || (target&3)) throw std::runtime_error("XMT relative target out of code bounds");
    return static_cast<std::uint32_t>(target);
}
Animation::Animation(std::size_t count) {
    if(!count||count>5120)throw std::runtime_error("Invalid native channel count");
    channel_count_=count;channels_.resize(count);
}
Animation::Animation(Bytes bytes,std::size_t count):bytes_(bytes.begin(),bytes.end()) {
    if (!count || count>5120 || bytes.size()>0x700000 || word(0)!=0x544d58)
        throw std::runtime_error("Invalid XMT input/count");
    code_=word(16)==0xffffffff ? 32 : 16;
    if (code_!=32) throw std::runtime_error("Legacy XMT header not yet validated");
    const auto samples=std::uint64_t(code_)+word(4), end=samples+word(8);
    if (end+word(12)!=bytes.size() || (samples&3) || (end&3)) throw std::runtime_error("Invalid XMT section sizes");
    samples_=samples; sample_end_=end; wide_tracks_=word(24)!=0;
    scale_channels_=word(20)!=0;
    channel_count_=count;channels_.resize(count);
}
Animation::Animation(Animation&& other) noexcept
    :bytes_(std::move(other.bytes_)),channels_(std::move(other.channels_)),pool_(std::move(other.pool_)),
     channel_count_(other.channel_count_),pool_base_(other.pool_base_),code_(other.code_),samples_(other.samples_),
     sample_end_(other.sample_end_),pc_(other.pc_),flags_(other.flags_),wait_(other.wait_),wide_tracks_(other.wide_tracks_),
     scale_channels_(other.scale_channels_),texture_callback_(std::move(other.texture_callback_)),
     service_(std::move(other.service_)),channel_target_(other.channel_target_) {
    other.channel_count_=0;other.pool_base_=0;other.channel_target_=nullptr;
}
Animation& Animation::operator=(Animation&& other) noexcept {
    if(this==&other)return *this;
    if(pool_)pool_->release(pool_base_,channel_count_);
    bytes_=std::move(other.bytes_);channels_=std::move(other.channels_);pool_=std::move(other.pool_);
    channel_count_=other.channel_count_;pool_base_=other.pool_base_;code_=other.code_;samples_=other.samples_;
    sample_end_=other.sample_end_;pc_=other.pc_;flags_=other.flags_;wait_=other.wait_;wide_tracks_=other.wide_tracks_;
    scale_channels_=other.scale_channels_;texture_callback_=std::move(other.texture_callback_);
    service_=std::move(other.service_);channel_target_=other.channel_target_;
    other.channel_count_=0;other.pool_base_=0;other.channel_target_=nullptr;
    return *this;
}
Animation::~Animation() {if(pool_)pool_->release(pool_base_,channel_count_);}
Animation::Channel& Animation::channel(std::size_t index) {
    if(index>=channel_count_)throw std::out_of_range("Animation channel index");
    return pool_?pool_->slots_.at(pool_base_+index):channels_.at(index);
}
const Animation::Channel& Animation::channel(std::size_t index) const {
    if(index>=channel_count_)throw std::out_of_range("Animation channel index");
    return pool_?pool_->slots_.at(pool_base_+index):channels_.at(index);
}
Animation::Channel& Animation::script_channel(std::size_t index) {
    if(pool_) {
        if(index>=AnimationChannelPool::capacity-pool_base_)throw std::runtime_error("XMT shared channel pool overflow");
        return pool_->slots_[pool_base_+index];
    }
    if(index>=channel_count_)throw std::runtime_error("XMT channel out of bounds");
    return channels_[index];
}
const Animation::Channel& Animation::script_channel(std::size_t index) const {
    if(pool_) {
        if(index>=AnimationChannelPool::capacity-pool_base_)throw std::runtime_error("XMT shared channel pool overflow");
        return pool_->slots_[pool_base_+index];
    }
    if(index>=channel_count_)throw std::runtime_error("XMT channel out of bounds");
    return channels_[index];
}
void Animation::bind_channels(std::shared_ptr<AnimationChannelPool> pool) {
    if(!pool)throw std::runtime_error("Animation channel pool is null");
    if(pool_)throw std::runtime_error("Animation channels already bound to a pool");
    if(!channel_count_||channels_.size()!=channel_count_)throw std::runtime_error("Animation local channels unavailable");
    std::size_t base=AnimationChannelPool::capacity;
    for(std::size_t first=0;first+channel_count_<=AnimationChannelPool::capacity;++first) {
        bool free=true;
        for(std::size_t i=0;i<channel_count_;++i)if(pool->slots_[first+i].flags!=0){free=false;break;}
        if(free){base=first;break;}
    }
    if(base==AnimationChannelPool::capacity)throw std::runtime_error("Animation channel pool exhausted");
    for(std::size_t i=0;i<channel_count_;++i)pool->slots_[base+i]=channels_[i];
    pool_=std::move(pool);pool_base_=base;channels_.clear();
}
std::size_t AnimationChannelPool::free_channels() const {
    std::size_t count=0;for(const auto& slot:slots_)if(slot.flags==0)++count;return count;
}
AnimationChannelPool::AnimationChannelPool() {
    for(auto& slot:slots_) {
        slot.flags=0;slot.mode=0;slot.component={};slot.output={};slot.a={};slot.b={};slot.c={};
        slot.t={};slot.step={};slot.next_step={};slot.track=0;
    }
}
void AnimationChannelPool::release(std::size_t base,std::size_t count) noexcept {
    if(base>capacity||count>capacity-base)return;
    for(std::size_t i=0;i<count;++i)slots_[base+i].flags=0;
}
void Animation::start(std::size_t entry) {select(entry);Rounding rounding;run();}
void Animation::select(std::size_t entry) {
    const auto at=std::uint64_t(code_)+entry*8;
    if (entry>(samples_-code_)/8 || at+8>samples_) throw std::runtime_error("XMT entry out of bounds");
    pc_=static_cast<std::uint32_t>(at);wait_=0;flags_|=0x100;
}
void Animation::start_at(std::uint32_t at) {
    if(at<code_ || at+4ull>samples_ || (at&3))throw std::runtime_error("XMT start PC bounds");
    pc_=at;wait_=0;flags_|=0x100;
    Rounding rounding;run();
}
void Animation::run() {
    const auto resume_pc=pc_;
    for (unsigned instructions=0;instructions<100000;++instructions) {
        if (pc_<code_ || pc_+4>samples_ || (pc_&3)) throw std::runtime_error("XMT PC out of bounds");
        const auto w=word(pc_), op=w&255, a=(w>>8)&0xfff;
        const auto b=std::bit_cast<std::int32_t>(w)>>20;
        const auto p=pc_+4;
        if (op==0) { flags_&=~0x100; pc_=resume_pc; return; } // Retail stores PC only on yield.
        if (op==1) { pc_=relative(p); continue; }
        if (op==2) { wait_=static_cast<std::uint16_t>(b); pc_=p; return; }
        if (op==0x34) {
            if (b<0 || !texture_callback_) throw std::runtime_error("XMT texture callback requires a valid XTA binding");
            texture_callback_(static_cast<std::size_t>(b),a); pc_=p; continue;
        }
        if(op!=0x25 && op!=0x2e && op!=0x30 && op!=0x31 && op!=0x33 && op!=0x40 && op!=0x42 && op!=0x27 && op!=8 && service_) {
            const auto result=service_(op,a,b,pc_);
            if(result.next_pc<code_ || result.next_pc+4ull>samples_ || (result.next_pc&3))
                throw std::runtime_error("XMT service continuation bounds");
            pc_=result.next_pc;if(result.yield)return;continue;
        }
        const auto index=(op==0x25 || op==0x30 || op==0x2e || op==8 ? b : static_cast<int>(a))>>2;
        auto& target=channel_target_ ? *channel_target_ : *this;
        if(index<0)throw std::runtime_error("XMT channel out of bounds");
        auto& ch=target.script_channel(static_cast<std::size_t>(index));
        if(op==0x25) {
            if(p+4ull>samples_)throw std::runtime_error("Truncated XMT channel mode");
            const auto mode=word(p)&0xffff;
            if(mode!=1&&mode!=2&&mode!=0x10)throw std::runtime_error("Unrecovered channel mode");
            ch.mode=std::uint16_t(mode);pc_=p+4;continue;
        }
        if(op==8) {if(ch.step[b&3]!=0)return;pc_=p;continue;}
        if((op==0x2e || op==0x31 || op==0x40) && p+4ull>samples_)throw std::runtime_error("Truncated scalar instruction");
        if(op==0x2e) {ch.a[b&3]=number(p);ch.flags=0x201;pc_=p+4;continue;}
        if(op==0x40) {
            const unsigned component=a&3;const float value=number(p);
            if(!std::isfinite(value)||b<=0)throw std::runtime_error("Invalid turn transition");
            auto& from=ch.a[component];
            if(value<from){if((value+1.f)-from<from-value)from-=1.f;}
            else if((from+1.f)-value<value-from)from+=1.f;
            ch.b[component]=value;ch.step[component]=1.f/static_cast<float>(b);ch.c[component]=value;
            ch.next_step[component]=1;ch.component[component]&=~0x400;ch.flags=ch.mode|0x100;
            pc_=p+4;continue;
        }
        if(op==0x27 && channel_target_ && channel_target_!=this)throw std::runtime_error("Cross-script sampled tracks unsupported");
        unsigned payload=0;
        if (op==0x30 || op==0x33 || op==0x42) payload=12;
        else if (op==0x27 || op==0x31) payload=4;
        else throw std::runtime_error("Unrecovered XMT opcode "+std::to_string(op)+" at "+std::to_string(pc_));
        if (std::uint64_t(p)+payload>samples_) throw std::runtime_error("Truncated XMT instruction payload");
        if (op==0x30) {
            for (unsigned i=0;i<3;++i) ch.a[i]=number(p+i*4);
            ch.step={}; ch.flags=0x201;
        } else if (op==0x27) {
            const auto track=relative(p);
            if (std::uint64_t(track)+24>samples_) throw std::runtime_error("Truncated XMT track header");
            ch.track=track+24; ch.mode|=0x400; ch.flags=ch.mode+0x100;
            for (unsigned i=0;i<4;++i) if ((b>>i)&1) {
                ch.component[i]|=0x400;
                ch.a[i]=sample(std::uint64_t(word(track))+i); ch.b[i]=sample(std::uint64_t(word(track+8))+i);
                ch.c[i]=sample(std::uint64_t(word(track+16))+i); ch.t[i]=0;
                ch.step[i]=number(track+12); ch.next_step[i]=number(track+20);
            }
        } else {
            if (b<=0) throw std::runtime_error("Unsupported nonpositive XMT transition duration");
            const float rate=1.0f/static_cast<float>(b);
            const unsigned first=op==0x33 ? 0 : a&3, count=op==0x33 ? 4 : (op==0x31 ? 1 : 3);
            if (first+count>4) throw std::runtime_error("XMT component out of bounds");
            for (unsigned j=0;j<count;++j) {
                const auto i=first+j;
                const float target=j==3 ? 1.0f : number(p+j*4);
                if (op==0x42) {
                    if (target<ch.a[i]) { if ((target+1.0f)-ch.a[i]<ch.a[i]-target) ch.a[i]-=1.0f; }
                    else if ((ch.a[i]+1.0f)-target<target-ch.a[i]) ch.a[i]+=1.0f;
                }
                if (op==0x42 || ch.step[i]==0) {
                    ch.b[i]=target; ch.step[i]=rate; ch.c[i]=target; ch.next_step[i]=1;
                } else { ch.c[i]=target; ch.next_step[i]=rate; }
                ch.component[i]&=~0x400;
                ch.flags=ch.mode|0x100;
            }
        }
        pc_=p+payload;
    }
    throw std::runtime_error("XMT instruction budget exhausted");
}
void Animation::set_channel(std::size_t index,const Float4& value) {
    for(float v:value)if(!std::isfinite(v))throw std::runtime_error("Non-finite channel value");
    auto& ch=channel(index);ch.a=value;ch.step={};ch.flags=0x201;
}
void Animation::release_channel(std::size_t index) {
    channel(index).flags=0;
}
void Animation::set_channel_mode(std::size_t index,unsigned mode) {
    if(mode!=1&&mode!=2&&mode!=0x10)throw std::runtime_error("Unrecovered channel mode");
    channel(index).mode=std::uint16_t(mode);
}
void Animation::write_channel_output(std::size_t index,const Float4& value) {
    for(float v:value)if(!std::isfinite(v))throw std::runtime_error("Non-finite channel output");
    channel(index).output=value;
}
void Animation::set_component(std::size_t index,unsigned component,float value) {
    if(component>=4 || !std::isfinite(value))throw std::runtime_error("Invalid channel component");
    auto& ch=channel(index);ch.a[component]=value;ch.flags=0x201;
}
void Animation::turn_component(std::size_t index,unsigned component,float value,int duration) {
    Rounding rounding;
    if(component>=4 || !std::isfinite(value) || duration<=0)throw std::runtime_error("Invalid turn transition");
    auto& ch=channel(index);auto& from=ch.a[component];
    if(value<from){if((value+1.f)-from<from-value)from-=1.f;}
    else if((from+1.f)-value<value-from)from+=1.f;
    ch.b[component]=value;ch.step[component]=1.f/float(duration);ch.c[component]=value;ch.next_step[component]=1;
    ch.component[component]&=~0x400;ch.flags=ch.mode|0x100;
}
bool Animation::moving(std::size_t index,unsigned component) const {
    if(component>=4)throw std::runtime_error("Invalid channel component");
    return channel(index).step[component]!=0;
}
void Animation::move_channel(std::size_t index,const Float4& target,int duration) {
    Rounding rounding;
    if(duration<=0)throw std::runtime_error("Unsupported nonpositive channel movement duration");
    for(float value:target)if(!std::isfinite(value))throw std::runtime_error("Non-finite channel movement");
    auto& ch=channel(index);
    const float rate=1.f/static_cast<float>(duration);
    for(unsigned i=0;i<4;++i) {
        if(ch.step[i]==0) {ch.b[i]=target[i];ch.step[i]=rate;ch.c[i]=target[i];ch.next_step[i]=1;}
        else {ch.c[i]=target[i];ch.next_step[i]=rate;}
        ch.component[i]&=~0x400;
    }
    ch.flags=ch.mode|0x100;
}
void Animation::read_track(Channel& ch) {
    for (unsigned instructions=0;instructions<100000;++instructions) {
        if (ch.track<code_ || std::uint64_t(ch.track)+8>samples_) throw std::runtime_error("XMT track PC out of bounds");
        const auto w=word(ch.track), tag=wide_tracks_ ? w : w>>16;
        if ((wide_tracks_ && w<0x10000000) || (!wide_tracks_ && tag==0)) {
            const auto index=wide_tracks_ ? w : w&0xffff;
            const auto step=number(ch.track+4);
            for (unsigned i=0;i<4;++i) if (ch.component[i]&0x400) { ch.c[i]=sample(std::uint64_t(index)+i); ch.next_step[i]=step; }
            ch.track+=8; return;
        }
        if ((wide_tracks_ && (w&0x10000000)) || (!wide_tracks_ && tag==1)) { ch.track=relative(ch.track+4); continue; }
        ch.flags=0; return;
    }
    throw std::runtime_error("XMT track instruction budget exhausted");
}
void Animation::advance(Channel& ch) {
    if (!(ch.flags&0x100)) return;
    int moving=4, queued=4;
    for (unsigned i=0;i<4;++i) ch.t[i]+=ch.step[i];
    for (unsigned i=0;i<4;++i) {
        if (ch.t[i]>=1) {
            ch.t[i]-=1;
            if (!(ch.component[i]&0x8000)) {
                ch.a[i]=ch.b[i]; ch.b[i]=ch.c[i]; ch.step[i]=ch.next_step[i];
                if (ch.step[i]==0) ch.t[i]=0;
                ch.next_step[i]=0;
            } else if (ch.component[i]&0x4000) {
                ch.c[i]=ch.a[i]; ch.a[i]=ch.b[i]; ch.b[i]=ch.c[i]; ++moving;
            }
        }
        if (ch.step[i]==0) --moving;
        if (ch.next_step[i]==0) --queued;
    }
    if (queued==0 && (ch.flags&0x400)) read_track(ch);
    if (moving==0) ch.flags=1;
    ch.flags|=0x200;
}
void Animation::evaluate(Channel& ch) {
    if (!(ch.flags&0x200)) return;
    switch (ch.flags&0x7f) {
        case 0: break; // Ended track keeps its last evaluated output (0x1b3c78).
        case 1: ch.output=ch.a; break;
        case 2:
            for (unsigned i=0;i<4;++i) ch.output[i]=(ch.b[i]-ch.a[i])*ch.t[i]+ch.a[i];
            break;
        case 0x10:
            // 0x1b3c78: preserve the individual VU operations and their order.
            // Reassociating this cubic changes rounded camera positions.
            for(unsigned i=0;i<4;++i) {
                const float t2=ch.t[i]*ch.t[i],t3=t2*ch.t[i];
                const float d=ch.b[i]-ch.a[i],e=ch.c[i]-ch.b[i];
                float cubic=ch.a[i]*2.f-ch.b[i]*2.f;
                cubic=cubic+d;cubic=cubic+e;
                float quadratic=ch.a[i]*-3.f+ch.b[i]*3.f;
                quadratic=quadratic-d*2.f;quadratic=quadratic-e;
                ch.output[i]=((cubic*t3+quadratic*t2)+d*ch.t[i])+ch.a[i];
            }
            break;
        default: throw std::runtime_error("Unrecovered XMT interpolation mode");
    }
    ch.flags&=~0x200;
}
void Animation::tick_channels() {Rounding rounding;for(std::size_t i=0;i<channel_count_;++i)advance(channel(i));}
void Animation::tick_script() {Rounding rounding;if(flags_&0x100){if(wait_)--wait_;else run();}}
void Animation::tick() {tick_channels();tick_script();}
std::vector<Float4> Animation::values() {
    Rounding rounding;
    std::vector<Float4> result; result.reserve(channel_count_);
    for(std::size_t i=0;i<channel_count_;++i) {auto& ch=channel(i);evaluate(ch);result.push_back(ch.output);}
    return result;
}
Float4 Animation::value(std::size_t index) {
    Rounding rounding;auto& ch=channel(index);evaluate(ch);return ch.output;
}
std::vector<Float4> Animation::channel_outputs() const {
    std::vector<Float4> result;result.reserve(channel_count_);
    for(std::size_t i=0;i<channel_count_;++i)result.push_back(channel(i).output);
    return result;
}
}
