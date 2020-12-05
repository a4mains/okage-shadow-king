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
    const auto value=__builtin_bit_cast(float, (word(at)));
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
    const auto target=std::int64_t(at)+__builtin_bit_cast(std::int32_t, (word(at)));
    if (target<code_ || target>=samples_ || (target&3)) throw std::runtime_error("XMT relative target out of code bounds");
    return static_cast<std::uint32_t>(target);
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
    channels_.resize(count);
}
void Animation::start(std::size_t entry) {
    const auto at=std::uint64_t(code_)+entry*8;
    if (entry>(samples_-code_)/8 || at+8>samples_) throw std::runtime_error("XMT entry out of bounds");
    pc_=at; wait_=0; flags_|=0x100;
    Rounding rounding; run();
}
void Animation::run() {
    const auto resume_pc=pc_;
    for (unsigned instructions=0;instructions<100000;++instructions) {
        if (pc_<code_ || pc_+4>samples_ || (pc_&3)) throw std::runtime_error("XMT PC out of bounds");
        const auto w=word(pc_), op=w&255, a=(w>>8)&0xfff;
        const auto b=__builtin_bit_cast(std::int32_t, (w))>>20;
        const auto p=pc_+4;
        if (op==0) { flags_&=~0x100; pc_=resume_pc; return; } // Retail stores PC only on yield.
        if (op==1) { pc_=relative(p); continue; }
        if (op==2) { wait_=static_cast<std::uint16_t>(b); pc_=p; return; }
        const auto index=(op==0x30 ? b : static_cast<int>(a))>>2;
        if (index<0 || std::size_t(index)>=channels_.size()) throw std::runtime_error("XMT channel out of bounds");
        auto& ch=channels_[index];
        unsigned payload=0;
        if (op==0x30 || op==0x33 || op==0x42) payload=12;
        else if (op==0x27) payload=4;
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
            const unsigned first=op==0x33 ? 0 : a&3, count=op==0x33 ? 4 : 3;
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
        default: throw std::runtime_error("Unrecovered XMT interpolation mode");
    }
    ch.flags&=~0x200;
}
void Animation::tick() {
    Rounding rounding;
    for (auto& ch:channels_) advance(ch);
    if (flags_&0x100) { if (wait_) --wait_; else run(); }
}
std::vector<Float4> Animation::values() {
    Rounding rounding;
    std::vector<Float4> result; result.reserve(channels_.size());
    for (auto& ch:channels_) { evaluate(ch); result.push_back(ch.output); }
    return result;
}
}
