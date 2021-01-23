#include "okage/event.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace okage {
namespace {
std::uint32_t word(Bytes data,std::size_t at) {
    if(at>data.size() || data.size()-at<4)throw std::runtime_error("Truncated event word");
    return std::uint32_t(data[at]) | (std::uint32_t(data[at+1])<<8) |
           (std::uint32_t(data[at+2])<<16) | (std::uint32_t(data[at+3])<<24);
}
bool condition(unsigned mode,std::int32_t value,std::int32_t expected) {
    switch(mode) {
        case 1:return value==expected;
        case 2:return value!=expected;
        case 3:return value>expected;
        case 4:return value<expected;
        case 5:return value>=expected;
        case 6:return value<=expected;
        default:return true; // Original 0x130378 default, not an unknown-op stub.
    }
}
}
std::int32_t event_variable(Bytes variables,std::uint32_t selector) {
    const auto index=selector&0xffff,bank=selector>>16;
    std::size_t at;
    if(bank<=1) {
        at=0x10+bank*0x100+(index>>3);
        if(index>=0x800 || at>=variables.size())throw std::runtime_error("Event bit bank bounds");
        return (variables[at]&(0x80>>(index&7)))!=0;
    }
    if(bank==2 || bank==3) {
        at=0x210+(bank-2)*0x200+index;
        if(index>=(bank==2 ? 0x200u : 0x100u) || at>=variables.size())
            throw std::runtime_error("Event byte bank bounds");
        return __builtin_bit_cast(std::int8_t, (variables[at]));
    }
    if(bank==4)return __builtin_bit_cast(std::int32_t, (word(variables,0x510+std::size_t(index)*4)));
    throw std::runtime_error("Unrecovered event variable bank "+std::to_string(bank));
}
void set_event_variable(std::span<std::uint8_t> variables,unsigned index,unsigned bank,std::int32_t value) {
    if(index>0xffff || bank>4)throw std::runtime_error("Unrecovered event variable write bank/index");
    // Validate the same backing extent as the reader before mutating it.
    event_variable(variables,(bank<<16)|index);
    if(bank<=1){
        auto& byte=variables[0x10+bank*0x100+(index>>3)];const auto mask=0x80>>(index&7);
        byte=value ? byte|mask : byte&~mask;
    } else if(bank<=3)variables[0x210+(bank-2)*0x200+index]=std::uint8_t(value);
    else for(unsigned i=0;i<4;++i)variables[0x510+index*4+i]=std::uint32_t(value)>>(i*8);
}
std::int32_t poll_dialogue_wait(DialogueWaitState& state,std::int32_t request,
                               const DialogueObservation& dialogue) {
    if(state.countdown) {
        --state.countdown;
        return state.countdown ? -1 : 0;
    }
    if(request==-1)return state.mode==5 && state.active!=0 ? 0 : -1;
    if(request==0) {
        if(!dialogue.present || dialogue.status<0)state.countdown=2;
        return -1;
    }
    if(!dialogue.present)throw std::runtime_error("Dialogue progress wait requires a message");
    const bool ready=std::all_of(dialogue.layer_flags.begin(),dialogue.layer_flags.end(),
                                  [](auto flags){return (flags&0x40)!=0;});
    const auto progress=ready ? __builtin_bit_cast(std::uint32_t, (dialogue.text_progress)) : 0;
    if(state.observed!=progress) {++state.observed;++state.accumulated;}
    if(__builtin_bit_cast(std::int32_t, (state.accumulated))>=request) {
        state.countdown=2;
        state.accumulated-=__builtin_bit_cast(std::uint32_t, (request));
    }
    return 1;
}
std::vector<std::uint32_t> run_event_setup(Bytes script,Bytes variables,
                                         std::span<std::uint8_t> field_state) {
    if(word(script,0)!=0x544d58 || word(script,16)!=0xffffffff)
        throw std::runtime_error("Event setup requires extended XMT header");
    const std::uint64_t end=32ull+word(script,4);
    if(end<36 || (end&3) || end+word(script,8)+word(script,12)!=script.size())
        throw std::runtime_error("Event setup section bounds");
    std::vector<std::uint8_t> state(field_state.begin(),field_state.end());
    std::vector<std::uint32_t> pcs;
    std::int64_t pc=32;
    auto code_word=[&](std::int64_t at){
        if(at<32 || std::uint64_t(at)+4>end || (at&3))throw std::runtime_error("Event setup PC bounds");
        return word(script,std::size_t(at));
    };
    auto write_word=[&](std::size_t at,std::uint32_t value){
        if(at>state.size() || state.size()-at<4)throw std::runtime_error("Event field word bounds");
        for(unsigned i=0;i<4;++i)state[at+i]=value>>(8*i);
    };
    for(unsigned budget=0;budget<100000;++budget) {
        const auto instruction=code_word(pc),op=instruction&255,a=(instruction>>8)&0xfff;
        const auto b=__builtin_bit_cast(std::int32_t, (instruction))>>20;
        pcs.push_back(std::uint32_t(pc));
        const auto payload=pc+4;
        switch(op) {
            case 0:
                std::copy(state.begin(),state.end(),field_state.begin());return pcs;
            case 1:pc=payload+__builtin_bit_cast(std::int32_t, (code_word(payload)));break;
            case 11: {
                const auto value=event_variable(variables,code_word(payload));
                const auto expected=__builtin_bit_cast(std::int32_t, (code_word(payload+4)));
                pc=condition(a,value,expected) ? payload+8 : payload+(b>>2)*4-4;
                break;
            }
            case 0x50: {
                const auto index=b<0 ? -8-b : b;
                const auto at=std::int64_t(0x1a8)+index;
                if(at<0 || std::uint64_t(at)>=state.size())throw std::runtime_error("Event field byte bounds");
                state[std::size_t(at)]=a;pc=payload;break;
            }
            case 0x59:
                if(b&0x200) {write_word(0x400+(b&255)*4,code_word(payload));pc=payload+4;}
                else if(!(b&0x100) && b>=0) {write_word(0x400+b*4,a);pc=payload;}
                else throw std::runtime_error("Event string service not yet recovered");
                break;
            default:throw std::runtime_error("Unrecovered event setup opcode "+std::to_string(op)+" at "+std::to_string(pc));
        }
    }
    throw std::runtime_error("Event setup instruction budget exhausted");
}
}
