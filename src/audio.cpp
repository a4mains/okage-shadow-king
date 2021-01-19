#include "okage/audio.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace okage {
std::array<std::int16_t,28> decode_spu_block(Bytes block,AdpcmHistory& history) {
    if(block.size()!=16)throw std::runtime_error("SPU block extent invalid");
    constexpr std::array<std::array<int,2>,5> coefficients{{{0,0},{60,0},{115,-52},{98,-55},{122,-60}}};
    const unsigned predictor=block[0]>>4,shift=block[0]&15;
    if(predictor>=coefficients.size() || (block[1]&~7))throw std::runtime_error("Unrecovered SPU block header");
    std::array<std::int16_t,28> result{};
    for(unsigned i=0;i<28;++i) {
        int nibble=(block[2+i/2]>>((i&1)*4))&15;if(nibble>=8)nibble-=16;
        const auto value=std::clamp(((nibble*4096)>>shift)+((history.recent*coefficients[predictor][0]+history.older*coefficients[predictor][1]+32)>>6),-32768,32767);
        history.older=history.recent;history.recent=value;result[i]=std::int16_t(value);
    }
    return result;
}
DecodedSample decode_spu_sample(Bytes blocks) {
    if(blocks.empty() || blocks.size()%16 || blocks.size()>2*1024*1024)
        throw std::runtime_error("SPU sample block extent invalid");
    AdpcmHistory history;
    DecodedSample result;
    for(std::size_t at=0;at<blocks.size();at+=16) {
        const auto flags=blocks[at+1];
        if(flags&4)result.loop_sample=result.pcm.size();
        const auto decoded=decode_spu_block(blocks.subspan(at,16),history);
        result.pcm.insert(result.pcm.end(),decoded.begin(),decoded.end());
        result.consumed=at+16;
        if(flags&1) {result.end_flags=flags;return result;}
    }
    throw std::runtime_error("SPU sample lacks an end block");
}
}
