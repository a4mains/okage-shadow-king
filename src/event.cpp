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
void initialize_game_variables(std::span<std::uint8_t> variables) {
    if(variables.size()<0xd78)throw std::runtime_error("Game variable reset bounds");
    std::fill_n(variables.begin(),0xd78,0);
    variables[0xe]=1;
    // 0x124f10 copies these four-byte initial names from the pinned retail
    // constants at 0x2237d0/0x2237d8. Later naming is a separate operation.
    constexpr std::uint8_t hero_name[]={'A','r','i',0},shadow_name[]={'?','?','?',0};
    std::copy_n(hero_name,4,variables.begin()+0x844);
    std::copy_n(shadow_name,4,variables.begin()+0x864);
    set_event_variable(variables,3,4,1000);
    set_event_variable(variables,24,4,3);
    set_event_variable(variables,30,4,3);
    constexpr unsigned initial_levels[]={1,5,10,25,30,40,1};
    for(unsigned i=0;i<7;++i) {
        const auto at=0x5c4+i*0x50;
        // 0x125b08 writes its incoming byte/short arguments to the zeroed
        // temporary before copying all 0x50 bytes. Its decompilation omits
        // these stack stores; see instructions 0x125b58..0x125b78.
        variables[at]=1;
        variables[at+2]=0xaa+i;
        variables[at+4]=initial_levels[i];
        variables[at+0xe]=5;
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
    if(bank>=8 && bank<=15) {
        // 0x125758 supplies 0x50-byte records. Keep supported indices within
        // one record; the original unchecked helper can otherwise alias it.
        at=0x5c4+(bank&7)*0x50+std::size_t(index)*2;
        if(index>=0x28 || at>variables.size() || variables.size()-at<2)
            throw std::runtime_error("Event short bank bounds");
        const auto value=std::uint16_t(unsigned(variables[at])|(unsigned(variables[at+1])<<8));
        return __builtin_bit_cast(std::int16_t, (value));
    }
    throw std::runtime_error("Unrecovered event variable bank "+std::to_string(bank));
}
void set_event_variable(std::span<std::uint8_t> variables,unsigned index,unsigned bank,std::int32_t value) {
    if(index>0xffff || (bank>4 && (bank<8 || bank>15)))throw std::runtime_error("Unrecovered event variable write bank/index");
    // Validate the same backing extent as the reader before mutating it.
    event_variable(variables,(bank<<16)|index);
    if(bank<=1){
        auto& byte=variables[0x10+bank*0x100+(index>>3)];const auto mask=0x80>>(index&7);
        byte=value ? byte|mask : byte&~mask;
    } else if(bank<=3)variables[0x210+(bank-2)*0x200+index]=std::uint8_t(value);
    else if(bank==4)for(unsigned i=0;i<4;++i)variables[0x510+index*4+i]=std::uint32_t(value)>>(i*8);
    else {
        const auto at=0x5c4+(bank&7)*0x50+index*2;
        variables[at]=std::uint32_t(value)&255;
        variables[at+1]=(std::uint32_t(value)>>8)&255;
    }
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
    const auto strings=end+word(script,8);
    auto string=[&](std::uint32_t offset) {
        if(offset>=word(script,12))throw std::runtime_error("Event setup string offset bounds");
        const auto first=script.begin()+strings+offset,last=std::find(first,script.end(),0);
        if(last==script.end())throw std::runtime_error("Unterminated event setup string");
        return std::string(first,last);
    };
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
                else if(b>=0 && ((b&255)==0 || (b&255)==1)) {
                    // 0x131048 resolves against the current XMT string section,
                    // then copies an inline map filename or resource directory.
                    const auto value=string(code_word(payload));
                    const auto path=(b&255)==0?value+".xpf":"flddata/"+value+"/";
                    const std::size_t at=(b&255)==0?0x2c0:0x300;
                    if(path.size()>=64 || at+64>state.size())
                        throw std::runtime_error("Event setup resource path exceeds inline buffer");
                    std::copy(path.begin(),path.end(),state.begin()+at);state[at+path.size()]=0;
                    pc=payload+4;
                }
                else throw std::runtime_error("Event string service not yet recovered at PC="+std::to_string(pc)+
                    " a="+std::to_string(a)+" b="+std::to_string(b)+" payload="+std::to_string(code_word(payload)));
                break;
            default:throw std::runtime_error("Unrecovered event setup opcode "+std::to_string(op)+" at "+std::to_string(pc));
        }
    }
    throw std::runtime_error("Event setup instruction budget exhausted");
}
}
