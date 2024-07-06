#include "okage/navigation_mask.hpp"
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
std::size_t record_offset(std::size_t extent,std::int32_t index) {
    if(index<0||index>=10)throw std::runtime_error("Navigation mask record index");
    const auto offset=std::size_t(0x878)+std::size_t(index)*0x80;
    if(offset>extent||extent-offset<0x80)
        throw std::runtime_error("Navigation mask variable extent");
    return offset;
}
}
NavigationMask::NavigationMask(std::uint32_t texture_slot,unsigned width,unsigned height)
    :texture_slot_(texture_slot) {
    if(texture_slot>std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("Navigation mask texture identity");
    if(width!=32||height!=32)throw std::runtime_error("Unrecovered navigation mask texture extent");
    // 0x1145c0 allocates width*height; 0x1bd6b8 fills that extent with zero.
}
void NavigationMask::reveal_cell(std::int32_t x,std::int32_t y,std::uint8_t value) {
    if(x<=0)x=1;
    else if(x>=31)x=30;
    if(y<=0)y=1;
    else if(y>=31)x=30; // 0x1148f8 MOVZ a3,v0,v1; a3 is X, not Y.
    if(y>30)throw std::runtime_error("Navigation mask source upper-Y write outside allocation");
    const auto center=std::size_t(y)*32+std::size_t(x);
    // Exact stores0x114910..0x114970, including the unusual diagonal order.
    constexpr std::array<int,9> offsets{0,1,-1,32,-32,33,-33,31,-31};
    for(auto delta:offsets)bytes_[std::size_t(std::int64_t(center)+delta)]=value;
}
void NavigationMask::restore_record(Bytes variables,std::int32_t index) {
    const auto offset=record_offset(variables.size(),index);
    for(std::size_t i=0;i<bytes_.size();++i)
        bytes_[i]=(variables[offset+i/8]&(0x80u>>(i%8)))?0xff:0;
}
void NavigationMask::pack_record(std::span<std::uint8_t> variables,std::int32_t index) const {
    const auto offset=record_offset(variables.size(),index);
    for(std::size_t i=0;i<0x80;++i) {
        std::uint8_t packed=0;
        for(unsigned bit=0;bit<8;++bit)
            if(bytes_[i*8+bit]==0xff)packed|=std::uint8_t(0x80u>>bit);
        variables[offset+i]=packed;
    }
}
}
