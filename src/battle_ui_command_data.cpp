#include "okage/battle_ui_command_data.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<4) throw std::runtime_error("Battle UI command data bounds");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
template<std::size_t N>
std::array<std::int32_t,N> signed_words(Bytes b,std::size_t at) {
    std::array<std::int32_t,N> result{};
    for(std::size_t i=0;i<N;++i) result[i]=__builtin_bit_cast(std::int32_t, (word(b,at+4*i)));
    return result;
}
}
BattleUiCommandData::BattleUiCommandData(Bytes b) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','B','C','M','D','1',0};
    if(b.size()!=144||!std::equal(magic.begin(),magic.end(),b.begin())||
       word(b,8)!=1||word(b,12)!=64)
        throw std::runtime_error("Unsupported battle UI command import");
    constexpr std::string_view hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<64;++i) {
        const auto nibble=(b[16+i/2]>>(i%2?0:4))&15;
        if(hash[i]!=digits[nibble]) throw std::runtime_error("Unsupported battle UI command source revision");
    }
    category_zero_=signed_words<2>(b,80);
    category_nonzero_=signed_words<2>(b,88);
    selection_=signed_words<4>(b,96);
    metrics_=signed_words<4>(b,112);
    clip_=signed_words<4>(b,128);
    if(category_zero_!=std::array<std::int32_t,2>{5,19}||
       category_nonzero_!=std::array<std::int32_t,2>{20,6}||
       selection_!=std::array<std::int32_t,4>{2,0,4,6}||
       metrics_!=std::array<std::int32_t,4>{12,24,6,18}||
       clip_!=std::array<std::int32_t,4>{-1000,0,512,2048})
        throw std::runtime_error("Battle UI command values outside recovered source scope");
}
}
