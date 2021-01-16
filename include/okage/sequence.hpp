#pragma once
#include "okage/assets.hpp"
#include <array>
#include <functional>

namespace okage {
struct MusicChannel {
    std::uint8_t dirty=0,flags=0,volume=127,pan=64,bank=0,program=0,last_key=255;
    std::uint8_t portamento=127,expression=127,modulation=0,pitch=64,bend_range=0,fine=64,coarse=64;
    std::uint16_t data=0,rpn=0x3fff,nrpn=0x3fff,left=128,right=128;
    // Inspection encoding of the recovered channel fields, not native memory layout.
    std::array<std::uint32_t,10> words() const;
};
struct MusicEvent {
    enum class Kind {note_on,note_off,control};
    Kind kind;
    unsigned channel,key,value;
    MusicChannel parameters;
};
// Native XSEQ data interpreter. This executes music data, never IOP instructions.
// Current format scope: version0x101, one track with initial zero delta.
class MusicSequence {
public:
    explicit MusicSequence(Bytes bytes,unsigned driver_hz=60);
    using Observer=std::function<void(unsigned,const MusicSequence&)>; // 0 before,1 event,2 after
    std::vector<MusicEvent> tick(const Observer& observer={});
    std::array<MusicChannel,16> channels;
    std::uint32_t cursor() const{return cursor_;}
    std::int32_t remaining() const{return remaining_;}
    unsigned tempo() const{return tempo_;}
    unsigned delta_scale() const{return delta_scale_;}
    unsigned clock_step() const{return clock_step_;}
    unsigned status() const{return status_;}
    unsigned next_status() const;
    bool ended() const{return ended_;}
    std::size_t loop_depth() const{return loops_.size();}
private:
    struct Loop {std::uint32_t cursor;std::uint8_t count=0,limit=0;};
    std::vector<std::uint8_t> bytes_;
    std::vector<Loop> loops_;
    std::uint32_t cursor_=0,end_=0;
    std::int32_t remaining_=0;
    unsigned ppqn_=0,driver_hz_=0,tempo_=500000,delta_scale_=0,clock_step_=0,status_=0;
    bool ended_=false;
    std::uint8_t byte();
    std::uint32_t vlq();
    void timing();
    void parameter(MusicChannel&,bool data_entry);
    void control(MusicChannel&,unsigned,unsigned);
    void event(std::vector<MusicEvent>&);
};
}
