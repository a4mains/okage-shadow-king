#include "okage/stream_bank.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at){
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("RXWS word bounds");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
}
Bytes StreamBank::record(int index) const{
    if(index<0 || std::size_t(index)>=records.size())return {};
    const auto at=records[index];return Bytes(bytes).subspan(at,bytes.at(at+1));
}
StreamBank read_stream_bank(Bytes b){
    if(b.size()<16 || b.size()>128*1024*1024 || word(b,0)!=0x53575852 || word(b,4)!=b.size()-16 || word(b,8)!=0x200 || word(b,12))
        throw std::runtime_error("Unsupported RXWS header/extent");
    StreamBank result;std::uint32_t form_end=0;
    constexpr std::array<std::uint32_t,5> tags={0x4d524f46,0x54585446,0x4b52414d,0x48435242,0x59444f42};
    for(std::size_t at=16;at<b.size();){
        if(b.size()-at<16)throw std::runtime_error("RXWS chunk header extent");
        const auto tag=word(b,at),size=word(b,at+4);
        if(size>b.size()-at-16 || word(b,at+8)!=0x100 || word(b,at+12))throw std::runtime_error("RXWS chunk version/extent");
        const auto found=std::find(tags.begin(),tags.end(),tag);
        if(found==tags.end())throw std::runtime_error("Unrecovered RXWS chunk");
        const auto index=std::size_t(found-tags.begin());
        if(result.chunks[index])throw std::runtime_error("Duplicate RXWS chunk");
        result.chunks[index]=at+16;
        if(index==0){if(size<4)throw std::runtime_error("RXWS FORM header");form_end=at+16+size;}
        if(index==4)result.body_size=size;
        at+=16+size;
    }
    if(!result.chunks[0] || !result.chunks[4])throw std::runtime_error("RXWS requires FORM and BODY");
    auto at=result.chunks[0];const auto count=word(b,at);at+=4;
    if(count>(form_end-at)/2)throw std::runtime_error("RXWS record count");
    for(unsigned i=0;i<count;++i){
        if(at>form_end || form_end-at<2 || b[at+1]<2 || b[at+1]>form_end-at)throw std::runtime_error("RXWS record extent");
        result.records.push_back(at);at+=b[at+1];
    }
    // 0x174f28 walks only the declared records. ALL.XWS pads FORM to a
    // 16-byte boundary; admit that observed zero padding, not arbitrary tails.
    if(at!=form_end && (form_end%16 || form_end-at>=16 ||
       std::any_of(b.begin()+at,b.begin()+form_end,[](auto v){return v!=0;})))
        throw std::runtime_error("RXWS FORM trailing bytes");
    result.bytes.assign(b.begin(),b.end());return result;
}
namespace {
StreamTone tone(const StreamBank& bank,int index,float speed,int volume,bool volume_in_range){
    const auto r=bank.record(index);
    // Original 0x1769b0 ignores byte 3 when flags byte 2 has bit 1 set,
    // passing -1 for voice selection. Footsteps use byte 3=0x80 here.
    if(r.size()!=28 || r[0]!=0 || (r[2]!=2&&r[2]!=3) || r[6] || r[7] || r[8] || r[9]!=1 || word(r,12) || bank.chunks[2] || bank.chunks[3])
        throw std::runtime_error("Unrecovered RXWS codec/channels/loop/markers");
    if(!std::isfinite(speed) || speed<0 || speed>=8 || !volume_in_range)throw std::runtime_error("RXWS cue speed/volume range");
    StreamTone t;t.offset=word(r,16);t.size=word(r,20);t.sample_rate=unsigned(r[10])|(unsigned(r[11])<<8);
    if(!t.sample_rate || t.offset%16 || !t.size || t.size%16 || t.offset>bank.body_size || t.size>bank.body_size-t.offset)
        throw std::runtime_error("RXWS sample payload bounds");
    if(r[2]&1) {
        t.loop_offset=word(r,24);
        if(*t.loop_offset>=t.size||*t.loop_offset%16)
            throw std::runtime_error("RXWS loop offset outside aligned sample");
    } else if(word(r,24)!=0xffffffff)throw std::runtime_error("RXWS finite loop metadata");
    t.offset+=bank.chunks[4];
    // EE0x172bf8 rate conversion, IOP0x9398 speed multiplication, and
    // EE0x176738's staged gains with recovered default global gains128.
    t.pitch=((t.sample_rate*4096/48000)*unsigned(speed*4096))>>12;
    t.left=(volume*std::bit_cast<std::int8_t>(r[4]))>>7;
    t.right=(volume*std::bit_cast<std::int8_t>(r[5]))>>7;
    return t;
}
}
StreamTone stream_tone(const StreamBank& bank,int index,float speed,unsigned volume){
    return tone(bank,index,speed,volume<=128?int(volume):0,volume<=128);
}
StreamTone stream_tone_signed(const StreamBank& bank,int index,float speed,int volume){
    return tone(bank,index,speed,volume,volume>=-128&&volume<=128);
}
std::vector<std::uint8_t> prepare_stream_sample(Bytes blocks,std::optional<std::uint32_t> loop_offset){
    if(blocks.empty()||blocks.size()%16||blocks.size()>2*1024*1024-128)
        throw std::runtime_error("RXWS finite sample extent");
    if(loop_offset) {
        if(*loop_offset>=blocks.size()||*loop_offset%16)
            throw std::runtime_error("RXWS loop offset outside aligned sample");
        std::vector<std::uint8_t> result(blocks.begin(),blocks.end());
        for(std::size_t i=1;i<result.size();i+=16)result[i]=0;
        result[*loop_offset+1]=4;result[result.size()-15]|=3;
        (void)decode_spu_sample(result);
        return result;
    }
    const auto aligned=(blocks.size()+63)&~std::size_t(63);
    std::vector<std::uint8_t> result(aligned+64);
    std::copy(blocks.begin(),blocks.end(),result.begin());
    for(std::size_t i=1;i<blocks.size();i+=16)result[i]=0;
    result[aligned+1]=1;
    (void)decode_spu_sample(result);
    return result;
}
void StreamPlayer::play(int index,float speed,unsigned volume){
    start(stream_tone(bank_,index,speed,volume));
}
void StreamPlayer::play_signed(int index,float speed,int volume){
    start(stream_tone_signed(bank_,index,speed,volume));
}
void StreamPlayer::start(const StreamTone& tone){
    auto sample=prepare_stream_sample(Bytes(bank_.bytes).subspan(tone.offset,tone.size),tone.loop_offset);
    std::erase_if(voices_,[](const auto& v){return v->voice.ended();});
    if(voices_.size()>=48)throw std::runtime_error("RXWS shared voice allocation not recovered");
    voices_.push_back(std::make_unique<Playing>(std::move(sample),tone));
}
void StreamPlayer::mix(std::span<std::int32_t> stereo){
    if(stereo.size()%2)throw std::runtime_error("RXWS stereo output extent");
    for(std::size_t i=0;i<stereo.size();i+=2)for(auto& voice:voices_){const auto v=voice->voice.tick();stereo[i]+=v[0];stereo[i+1]+=v[1];}
    std::erase_if(voices_,[](const auto& v){return v->voice.ended();});
}
}
