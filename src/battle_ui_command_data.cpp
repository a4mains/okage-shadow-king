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
    for(std::size_t i=0;i<N;++i) result[i]=std::bit_cast<std::int32_t>(word(b,at+4*i));
    return result;
}
}
BattleUiCommandData::BattleUiCommandData(Bytes b) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','B','C','M','D','1',0};
    const bool v2=b.size()==384 && word(b,8)==2 && word(b,12)==304;
    auto expected_magic=magic;if(v2)expected_magic[6]='2';
    if((!v2&&(b.size()!=144||word(b,8)!=1||word(b,12)!=64))||
       !std::equal(expected_magic.begin(),expected_magic.end(),b.begin()))
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
    if(v2) {
        targets_.emplace();
        for(unsigned i=0;i<4;++i)targets_->palette[i]=signed_words<3>(b,144+i*12);
        for(unsigned i=0;i<3;++i) {
            auto begin=b.begin()+192+i*64,last=begin+64,end=std::find(begin,last,0);
            if(end==last||!std::all_of(end,last,[](auto x){return x==0;}))
                throw std::runtime_error("Battle target format terminator/padding");
            targets_->formats[i].assign(begin,end);
        }
        if(targets_->formats[0].find('%')!=std::string::npos)
            throw std::runtime_error("Battle target blank format conversion");
        for(unsigned i=1;i<3;++i) {
            const auto& f=targets_->formats[i];const auto pos=f.find('%');
            if(pos==std::string::npos||f.substr(pos,2)!="%s"||f.find('%',pos+1)!=std::string::npos)
                throw std::runtime_error("Battle target string conversion");
        }
    }
}
const std::array<std::array<std::int32_t,3>,4>& BattleUiCommandData::target_palette() const {
    if(!targets_)throw std::runtime_error("Target list requires v2 owned command import");
    return targets_->palette;
}
const std::string& BattleUiCommandData::target_blank() const {
    (void)target_palette();return targets_->formats[0];
}
std::string BattleUiCommandData::target_count_text(std::string_view text) const {
    (void)target_palette();auto out=targets_->formats[1];out.replace(out.find("%s"),2,text);return out;
}
std::string BattleUiCommandData::target_name_text(std::string_view text) const {
    (void)target_palette();auto out=targets_->formats[2];out.replace(out.find("%s"),2,text);return out;
}

}
