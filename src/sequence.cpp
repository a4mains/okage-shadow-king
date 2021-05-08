#include "okage/sequence.hpp"
#include <bit>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("Truncated XSEQ word");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|(std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
}
std::array<std::uint32_t,10> MusicChannel::words() const {
    const std::array<std::uint8_t,14> bytes={dirty,flags,volume,pan,bank,program,last_key,portamento,expression,modulation,pitch,bend_range,fine,coarse};
    std::array<std::uint32_t,10> out{};
    for(unsigned i=0;i<bytes.size();++i)out[i/4]|=std::uint32_t(bytes[i])<<((i%4)*8);
    out[3]|=std::uint32_t(data)<<16;out[4]=std::uint32_t(rpn)|(std::uint32_t(nrpn)<<16);
    out[5]=std::uint32_t(left)|(std::uint32_t(right)<<16);return out;
}
void MusicChannel::gain(unsigned left_gain,unsigned right_gain) {
    if(left_gain>128||right_gain>128)throw std::runtime_error("Unreviewed music channel gain");
    left=left_gain;right=right_gain;dirty|=2; // RX0x4578 consumes command0x73/mode3.
}
MusicSequence::MusicSequence(Bytes bytes,unsigned hz):bytes_(bytes.begin(),bytes.end()),driver_hz_(hz) {
    if(bytes.size()>4*1024*1024 || bytes.size()<52 || word(bytes,0)!=0x51455358 || word(bytes,4)!=0x101 || word(bytes,8)!=bytes.size())
        throw std::runtime_error("Unsupported XSEQ header");
    if(!hz || hz>1000)throw std::runtime_error("Invalid music driver frequency");
    std::uint32_t head=0,track=0,begin=0;
    for(std::size_t at=12;at<bytes.size();) {
        const auto id=word(bytes,at),size=word(bytes,at+4);
        if(size>bytes.size()-at-8)throw std::runtime_error("XSEQ chunk outside file");
        if(id==0x44414548) {if(head || size!=8)throw std::runtime_error("Unsupported XSEQ HEAD");head=at+8;}
        else if(id==0x4b435254) {if(track || size!=8)throw std::runtime_error("Unsupported XSEQ TRCK");track=at+8;}
        else if(id==0x55514553) {if(begin || !size)throw std::runtime_error("Unsupported XSEQ SEQU");begin=at+8;end_=begin+size;}
        else throw std::runtime_error("Unrecovered XSEQ chunk");
        at+=8+size;
    }
    if(!head || !track || !begin || bytes[head]!=1 || bytes[head+1]!=0 || word(bytes,head+4)!=track || word(bytes,track+4)!=begin)
        throw std::runtime_error("XSEQ requires one unmapped track");
    ppqn_=word(bytes,head)>>16;
    if(!ppqn_ || ppqn_>2147 || bytes[begin]!=0)throw std::runtime_error("Unsupported XSEQ division/initial delta");
    cursor_=begin+1;timing(); // Original0x4200 skips the initial zero byte.
}
std::uint8_t MusicSequence::byte() {
    if(cursor_>=end_)throw std::runtime_error("Truncated XSEQ event");
    return bytes_[cursor_++];
}
std::uint32_t MusicSequence::vlq() {
    std::uint32_t value=0;
    for(unsigned i=0;i<5;++i) {
        const auto b=byte();if(value>(std::numeric_limits<std::uint32_t>::max()>>7))throw std::runtime_error("XSEQ delta overflow");
        value=(value<<7)|(b&127);if(!(b&128))return value;
    }
    throw std::runtime_error("Unterminated XSEQ variable length value");
}
void MusicSequence::timing() {
    // Original0x5bb0 uses integer fixed-point timing, not floating BPM.
    const auto denominator=(ppqn_*1000000u)>>12;
    if(!tempo_ || tempo_>0xffffff)throw std::runtime_error("Invalid XSEQ tempo");
    delta_scale_=(tempo_*60u)/denominator;clock_step_=(driver_hz_*0x1000u)/60u;
    if(!delta_scale_)throw std::runtime_error("Unsupported zero XSEQ tick scale");
}
unsigned MusicSequence::next_status() const {
    if(cursor_>=end_)throw std::runtime_error("XSEQ cursor outside track");
    return bytes_[cursor_]&128 ? bytes_[cursor_] : status_;
}
void MusicSequence::parameter(MusicChannel& c,bool data_entry) {
    if(c.rpn!=0x3fff) {
        if(c.rpn==0)c.bend_range=c.data>>7;
        else if(c.rpn==1)c.fine=c.data>>7;
        else if(c.rpn==2)c.coarse=c.data>>7;
        else throw std::runtime_error("Unrecovered XSEQ RPN");
    } else if(c.nrpn!=0x3fff) {
        if(c.nrpn==0 || c.nrpn==0xa00) {
            if(data_entry) {if(!loops_.empty())loops_.back().limit=c.data&255;}
            else {if(loops_.size()>=256)throw std::runtime_error("XSEQ loop stack full");loops_.push_back({cursor_});}
        } else if(c.nrpn==1 || c.nrpn==0xf00) {
            if(!data_entry && !loops_.empty()) {
                auto& loop=loops_.back();
                if(!loop.limit || loop.count!=loop.limit || loop.count>126) {cursor_=loop.cursor;++loop.count;}
                else loops_.pop_back();
            }
        } else throw std::runtime_error("Unrecovered XSEQ NRPN");
    }
}
void MusicSequence::control(MusicChannel& c,unsigned op,unsigned value) {
    switch(op) {
        case 0:c.bank=std::uint8_t(value<<7);break;
        case 1:c.modulation=value;c.dirty|=16;break;
        case 5:c.portamento=value;break;
        case 6:c.data=value<<7;parameter(c,true);break;
        case 7:c.volume=value;c.dirty|=2;break;
        case 10:c.pan=value;c.dirty|=2;break;
        case 11:c.expression=value;c.dirty|=2;break;
        case 32:c.bank=(c.bank&127)|value;break;
        case 33:case 37:case 39:case 42:case 43:case 67:case 68:break;
        case 38:c.data=(c.data&127)|value;parameter(c,true);break;
        case 65:if(value<64)c.flags&=~8;else if(!(c.flags&8)){c.last_key=255;c.flags|=8;}break;
        case 84:c.last_key=value;break;
        case 96:++c.data;parameter(c,true);break;
        case 97:--c.data;parameter(c,true);break;
        case 98:c.rpn=0x3fff;c.data=0;c.nrpn=(c.nrpn&127)|value;parameter(c,false);break;
        case 99:c.nrpn=value<<7;c.rpn=0x3fff;c.data=0;parameter(c,false);break;
        case 100:c.nrpn=0x3fff;c.data=0;c.rpn=(c.rpn&127)|value;parameter(c,false);break;
        case 101:c.rpn=value<<7;c.nrpn=0x3fff;c.data=0;parameter(c,false);break;
        case 126:c.flags|=1;break;
        case 127:c.flags&=~1;break;
        default:throw std::runtime_error("Unrecovered XSEQ controller "+std::to_string(op));
    }
}
void MusicSequence::event(std::vector<MusicEvent>& events) {
    if(bytes_.at(cursor_)&128)status_=byte();
    if(status_<128)throw std::runtime_error("XSEQ running status missing");
    const auto channel=status_&15;auto& c=channels[channel];
    if(status_==255) {
        const auto kind=byte();const auto count=vlq();
        if(count>end_-cursor_)throw std::runtime_error("Truncated XSEQ metadata");
        if(kind==0x2f){if(count)throw std::runtime_error("Invalid XSEQ end event");ended_=true;}
        else if(kind==0x51){if(count!=3)throw std::runtime_error("Invalid XSEQ tempo event");tempo_=(bytes_[cursor_]<<16)|(bytes_[cursor_+1]<<8)|bytes_[cursor_+2];timing();}
        // Original0x647c skips other meta-event payloads, including text and time signatures.
        cursor_+=count;return;
    }
    if(status_==240 || status_==247) {
        const auto count=vlq();if(count>end_-cursor_)throw std::runtime_error("Truncated XSEQ system payload");cursor_+=count;return;
    }
    const auto first=byte();if(first&128)throw std::runtime_error("Invalid XSEQ data byte");
    const auto op=status_>>4;
    if(op==12){c.program=first;return;}
    if(op==13)return; // Original0x5e84 consumes channel pressure without a state change.
    const auto second=byte();if(second&128)throw std::runtime_error("Invalid XSEQ data byte");
    switch(op) {
        case 8:events.push_back({MusicEvent::Kind::note_off,channel,first,second,c});break;
        case 9:
            if(second){c.last_key=first;events.push_back({MusicEvent::Kind::note_on,channel,first,second,c});}
            else events.push_back({MusicEvent::Kind::note_off,channel,first,0,c});
            break;
        case 10:break; // Original0x5e54 consumes polyphonic pressure without applying it.
        case 11:control(c,first,second);events.push_back({MusicEvent::Kind::control,channel,first,second,c});break;
        case 14:c.pitch=(first>>7)|second;c.dirty|=8;events.push_back({MusicEvent::Kind::control,channel,128,c.pitch,c});break;
        default:throw std::runtime_error("Unrecovered XSEQ status");
    }
}
std::vector<MusicEvent> MusicSequence::tick(const Observer& observer) {
    std::vector<MusicEvent> events;if(ended_)return events;
    for(auto& c:channels)c.dirty=0; // Original0x48f4 clears these after applying voice changes.
    if(observer)observer(0,*this);
    unsigned count=0;
    while(remaining_<=0) {
        if(++count>100000)throw std::runtime_error("XSEQ zero-time event budget exhausted");
        if(observer)observer(1,*this);
        event(events);if(ended_)break;
        const auto delta=vlq();
        remaining_=__builtin_bit_cast(std::int32_t, (std::uint32_t(remaining_)+delta*delta_scale_));
    }
    remaining_=__builtin_bit_cast(std::int32_t, (std::uint32_t(remaining_)-clock_step_));
    if(observer)observer(2,*this);
    return events;
}
}
