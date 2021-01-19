#include "okage/voice.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace okage {
int VoiceEnvelope::tick(){
    if(phase_==Phase::stopped)return 0;
    unsigned shift=0;int step=0,target=0;bool exponential=false,decreasing=false;
    switch(phase_){
        case Phase::attack:shift=(a_>>10)&31;step=7-((a_>>8)&3);target=32767;exponential=a_&0x8000;break;
        case Phase::decay:shift=(a_>>4)&15;step=-8;target=((a_&15)+1)*2048;exponential=true;decreasing=true;break;
        case Phase::sustain:shift=(b_>>8)&31;step=7-((b_>>6)&3);exponential=b_&0x8000;decreasing=b_&0x4000;if(decreasing)step=-1-step;break;
        case Phase::release:shift=b_&31;step=-8;exponential=b_&32;decreasing=true;break;
        case Phase::stopped:break;
    }
    unsigned rate=32768u>>std::max(0,int(shift)-11);
    int delta=step*(1<<std::max(0,11-int(shift)));
    if(exponential && decreasing)delta=(delta*level_)>>15;
    if(exponential && !decreasing && level_>24576)rate/=4;
    counter_+=std::max(1u,rate);
    if(counter_>=32768){counter_=0;level_=std::clamp(level_+delta,0,32767);}
    if(phase_==Phase::sustain){if(!level_)phase_=Phase::stopped;}
    else if(decreasing ? level_<=target : level_>=target){
        if(phase_==Phase::release)phase_=Phase::stopped;
        else phase_=Phase(int(phase_)+1);
    }
    return level_;
}
const GaussianTable& gaussian_table(){
    // Windowed-sinc construction credited to Near, nocash and Ryphecha by PCSX2.
    static const auto table=[] {
        std::array<double,512> curve{};double sum=0;
        for(unsigned i=0;i<512;++i){
            const double x=511.5-i,angle=2*std::numbers::pi*x/1023;
            curve[i]=std::sin(std::numbers::pi*x/500)*(.42+.5*std::cos(angle)+.08*std::cos(2*angle))/x;
            sum+=curve[i];
        }
        for(auto& x:curve)x*=32640.*128/sum;
        GaussianTable result{};
        for(unsigned i=0;i<256;++i){
            const std::array<unsigned,4> indices={255-i,511-i,256+i,i};double correction=-32640;
            for(auto j:indices)correction+=curve[j];correction/=4;
            for(unsigned j=0;j<4;++j)result[i][j]=std::int16_t(std::round(curve[indices[j]]-correction));
        }
        return result;
    }();return table;
}
std::int32_t interpolate_voice(std::span<const std::int16_t,4> samples,unsigned fraction){
    if(fraction>=4096)throw std::runtime_error("Voice interpolation fraction");
    const auto& weights=gaussian_table()[fraction>>4];int output=0;
    for(unsigned i=0;i<4;++i)output+=(weights[i]*samples[i])>>15;
    return output;
}
SampleVoice::SampleVoice(Bytes blocks,std::uint16_t a,std::uint16_t b):envelope(a,b),blocks_(blocks){
    if(blocks.empty() || blocks.size()%16 || blocks.size()>2*1024*1024)throw std::runtime_error("Voice sample extent");
}
void SampleVoice::parameters(unsigned pitch,int left,int right){
    pitch_=std::min(pitch,0x3fffu);
    if(left < -32768 || left>32767 || right < -32768 || right>32767)throw std::runtime_error("Voice gain range");
    left_=std::clamp(left*128,-16384,16383)*2;right_=std::clamp(right*128,-16384,16383)*2;
}
void SampleVoice::fill(){
    if(write_-read_>12)return;
    if(block_>blocks_.size() || blocks_.size()-block_<16)throw std::runtime_error("Voice sample lacks loop/end block");
    const auto flags=blocks_[block_+1];
    if(flags&4)repeat_=block_;
    if(!group_)decoded_=decode_spu_block(blocks_.subspan(block_,16),history_);
    for(unsigned i=0;i<4;++i)fifo_[(write_+i)%32]=decoded_[group_*4+i];
    write_+=4;
    if(++group_==7){
        group_=0;block_+=16;
        if(flags&1){
            if(flags&2){block_=repeat_;++loop_count;}
            else envelope.stop();
        }
    }
}
std::array<std::int32_t,2> SampleVoice::tick(){
    if(ended())return {};
    fill();if(ended())return {};
    std::array<std::int16_t,4> input{};for(unsigned i=0;i<4;++i)input[i]=fifo_[(read_+i)%32];
    const auto sample=(interpolate_voice(input,fraction_)*envelope.tick())>>15;
    fraction_+=pitch_;read_+=fraction_>>12;fraction_&=4095;
    return {(sample*left_)>>15,(sample*right_)>>15};
}
}
