#include "okage/results_text.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t word(Bytes data,unsigned at) {
    if(at>data.size()||data.size()-at<4)throw std::runtime_error("Results text data bounds");
    return unsigned(data[at])|(unsigned(data[at+1])<<8)|(unsigned(data[at+2])<<16)|(unsigned(data[at+3])<<24);
}
bool digest(Bytes data,unsigned at,std::string_view hex) {
    if(at>data.size()||data.size()-at<hex.size()/2)return false;
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<hex.size();++i)
        if(hex[i]!=digits[(data[at+i/2]>>(i%2?0:4))&15])return false;
    return true;
}
std::string format_reached(std::string_view source,std::span<const std::string> args,
                           std::span<const std::string_view> conversions) {
    std::string output;unsigned argument=0;
    for(std::size_t at=0;at<source.size();) {
        if(source[at]!='%'){output+=source[at++];continue;}
        if(argument>=args.size()||argument>=conversions.size()||
           source.substr(at,conversions[argument].size())!=conversions[argument])
            throw std::runtime_error("Unrecovered results text conversion");
        output+=args[argument];at+=conversions[argument++].size();
    }
    if(argument!=args.size())throw std::runtime_error("Results text argument count");
    return output;
}
std::string padded_decimal(std::int32_t value) {
    auto digits=std::to_string(value);
    if(digits.size()<3)digits.insert(0,3-digits.size(),'0');
    return digits;
}
}

ResultsTextData::ResultsTextData(Bytes data) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','T','X','T','1',0};
    constexpr unsigned header=80,slot_bytes=64,payload_bytes=5*slot_bytes;
    if(data.size()!=header+payload_bytes||!std::equal(magic.begin(),magic.end(),data.begin())||
       word(data,8)!=1||word(data,12)!=payload_bytes)
        throw std::runtime_error("Unsupported results text import");
    if(!digest(data,16,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5"))
        throw std::runtime_error("Unsupported results text source revision");
    for(unsigned i=0;i<formats_.size();++i) {
        const auto first=data.begin()+header+i*slot_bytes;
        const auto last=first+slot_bytes;
        const auto end=std::find(first,last,0);
        if(end==last||end==first||!std::all_of(end,last,[](auto value){return value==0;}))
            throw std::runtime_error("Results text format terminator or padding");
        if(!std::all_of(first,end,[](auto value){return value>0&&value<128;}))
            throw std::runtime_error("Results text format is not ASCII");
        formats_[i].assign(reinterpret_cast<const char*>(&*first),std::size_t(end-first));
    }
}

std::string ResultsTextData::render(std::int32_t value,unsigned final_format) const {
    std::string prefix,suffix;
    if(value<1) {
        prefix=formats_[0];suffix=formats_[1];
    } else {
        const auto first=(value/1000000)%1000;
        const auto second=(value/1000)%1000;
        const auto third=value%1000;
        const std::array group_args{padded_decimal(first),padded_decimal(second),padded_decimal(third)};
        constexpr std::array<std::string_view,3> group_spec={"%03d","%03d","%03d"};
        const auto grouped=format_reached(formats_[2],group_args,group_spec);
        const auto significant=grouped.find_first_of("123456789");
        if(significant==grouped.npos)throw std::runtime_error("Results money value lacks significant digit");
        prefix=grouped.substr(0,significant);
        suffix=grouped.substr(significant);
    }
    const std::array args{std::to_string(0x1effff7fu),prefix,suffix};
    constexpr std::array<std::string_view,3> spec={"%d","%s","%s"};
    return format_reached(formats_[final_format],args,spec);
}

std::string ResultsTextData::money(std::int32_t value) const {return render(value,4);}
std::string ResultsTextData::money_summary(std::int32_t value) const {return render(value,3);}
}
