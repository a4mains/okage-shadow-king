#include "okage/audio.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace okage {
DecodedSample decode_spu_sample(Bytes blocks) {
    if(blocks.empty() || blocks.size()%16 || blocks.size()>2*1024*1024)
        throw std::runtime_error("SPU sample block extent invalid");
    constexpr std::array<std::array<int,2>,5> coefficients{{{0,0},{60,0},{115,-52},{98,-55},{122,-60}}};
    std::int32_t recent=0,older=0;
    DecodedSample result;
    for(std::size_t at=0;at<blocks.size();at+=16) {
        const unsigned predictor=blocks[at]>>4,shift=blocks[at]&15,flags=blocks[at+1];
        if(predictor>=coefficients.size() || (flags&~7))throw std::runtime_error("Unrecovered SPU block header");
        if(flags&4)result.loop_sample=result.pcm.size();
        for(unsigned i=0;i<28;++i) {
            int nibble=(blocks[at+2+i/2]>>((i&1)*4))&15;
            if(nibble>=8)nibble-=16;
            const auto residual=(nibble*4096)>>shift;
            const auto prediction=(recent*coefficients[predictor][0]+older*coefficients[predictor][1]+32)>>6;
            const auto value=std::clamp(residual+prediction,-32768,32767);
            older=recent;recent=value;
            result.pcm.push_back(static_cast<std::int16_t>(value));
        }
        result.consumed=at+16;
        if(flags&1) {result.end_flags=flags;return result;}
    }
    throw std::runtime_error("SPU sample lacks an end block");
}
}
