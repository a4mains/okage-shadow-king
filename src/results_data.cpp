#include "okage/results_data.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t word(Bytes data,unsigned at) {
    if(at>data.size()||data.size()-at<4)throw std::runtime_error("Results UI data bounds");
    return unsigned(data[at])|(unsigned(data[at+1])<<8)|(unsigned(data[at+2])<<16)|(unsigned(data[at+3])<<24);
}
bool digest(Bytes data,unsigned at,std::string_view hex) {
    if(at>data.size()||data.size()-at<hex.size()/2)return false;
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<hex.size();++i)
        if(hex[i]!=digits[(data[at+i/2]>>(i%2?0:4))&15])return false;
    return true;
}
constexpr std::array<std::uint32_t,16> expected_widgets={
    0x40,0x41,0x42,0x43,0x72,0x73,0x74,0x75,
    0x6e,0x6f,0x70,0x71,0x76,0x77,0x78,0x79
};
constexpr std::array<unsigned,4> table_offsets={0,14,38,44};
constexpr std::array<unsigned,4> table_counts={14,24,6,5};
constexpr std::array<std::uint32_t,4> animation_end={0xffffffffu,0xffffffffu,0,0};
}

ResultsUiData::ResultsUiData(Bytes data) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','S','U','I','1',0};
    constexpr unsigned header=88;
    constexpr unsigned payload=112+64+49*16+32;
    if(data.size()!=header+payload||!std::equal(magic.begin(),magic.end(),data.begin())||
       word(data,8)!=1||word(data,12)!=28||word(data,16)!=16||word(data,20)!=49)
        throw std::runtime_error("Unsupported results UI data format");
    if(!digest(data,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")||
       !digest(data,56,"9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183"))
        throw std::runtime_error("Unsupported results UI source revision");

    for(unsigned i=0;i<definition_.words.size();++i)
        definition_.words[i]=word(data,header+i*4);
    const auto& row=definition_.words;
    if(row[0]!=1||row[21]!=1||row[22]!=1||row[23]!=1||row[24]!=11||
       row[1]!=44||row[2]!=40||row[3]!=1||row[25]!=33||row[26]!=17||row[27]!=0)
        throw std::runtime_error("Unsupported results UI host binding or row layout");

    constexpr unsigned widgets_at=header+112;
    for(unsigned i=0;i<widget_ids_.size();++i)widget_ids_[i]=word(data,widgets_at+i*4);
    if(widget_ids_!=expected_widgets)throw std::runtime_error("Unsupported results widget IDs");

    constexpr unsigned records_at=widgets_at+64;
    for(unsigned i=0;i<animation_records_.size();++i)
        for(unsigned j=0;j<4;++j)
            animation_records_[i].words[j]=word(data,records_at+i*16+j*4);
    for(unsigned table=0;table<4;++table) {
        const unsigned first=table_offsets[table],count=table_counts[table];
        if(table<3&&animation_records_[first+count-1].words!=animation_end)
            throw std::runtime_error("Results animation table terminator differs");
        for(unsigned i=0;i+1<count;++i) {
            const auto& record=animation_records_[first+i].words;
            if(record==animation_end||record[1]>2)
                throw std::runtime_error("Unsupported results animation record");
        }
    }
    if(animation_records_[48].words!=animation_end)
        throw std::runtime_error("Results animation table terminator differs");

    constexpr unsigned streams_at=records_at+49*16;
    for(unsigned block=0;block<2;++block)
        for(unsigned i=0;i<4;++i)
            stream_constants_[block][i]=word(data,streams_at+block*16+i*4);
    constexpr std::array<std::array<std::uint32_t,4>,2> expected_streams={{{12,24,6,18},
        {0xfffffc18u,10000,0,0x3f800000u}}};
    if(stream_constants_!=expected_streams)
        throw std::runtime_error("Unsupported results stream constants");
}

std::span<const ResultsAnimationRecord> ResultsUiData::animation_table(unsigned index) const {
    if(index>=table_offsets.size())throw std::runtime_error("Results animation table bounds");
    return {animation_records_.data()+table_offsets[index],table_counts[index]};
}
}
