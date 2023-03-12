#include "okage/battle_data.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
constexpr std::size_t header_size=56, directory_size=256*8, stage_size=23*64;
constexpr std::array<std::uint8_t,32> source_hash{
    0x2a,0x90,0xb6,0xaf,0x1b,0x5e,0x0a,0xb5,0x77,0xe8,0x99,0x11,0x3d,0x7d,0x35,0xd9,
    0x76,0x69,0xa5,0xc1,0x3a,0xee,0x2a,0xe8,0x48,0x31,0x2b,0xd0,0x46,0x69,0xaa,0xa5};
[[noreturn]] void invalid() { throw std::runtime_error("Invalid battle data artifact"); }
std::uint32_t u32(Bytes b,std::size_t p) {
    return std::uint32_t(b[p]) | std::uint32_t(b[p+1])<<8 |
           std::uint32_t(b[p+2])<<16 | std::uint32_t(b[p+3])<<24;
}
}
BattleData::BattleData(Bytes b) {
    constexpr std::size_t entry_base=header_size+directory_size+stage_size;
    constexpr std::string_view magic="OKBTDAT1";
    if(b.size()<entry_base || !std::equal(magic.begin(),magic.end(),b.begin()) ||
       u32(b,8)!=1 || u32(b,12)!=256 || u32(b,16)!=23 ||
       !std::equal(source_hash.begin(),source_hash.end(),b.begin()+24)) invalid();
    const auto count=u32(b,20);
    if(count>256*10 || b.size()!=entry_base+std::size_t(count)*12) invalid();
    std::uint32_t next=0;
    for(std::size_t i=0;i<formations_.size();++i) {
        const auto at=header_size+i*8;
        auto& r=formations_[i]; r={u32(b,at),u32(b,at+4)};
        if(r.count==0xffffffffu) { if(r.first!=0) invalid(); continue; }
        if(r.count<1 || r.count>10 || r.first!=next || r.count>count-next) invalid();
        next+=r.count;
    }
    if(next!=count) invalid();
    constexpr std::string_view prefix="btldata/beta2/field/";
    for(std::size_t i=0;i<stages_.size();++i) {
        const auto path=b.subspan(header_size+directory_size+i*64,64);
        const auto end=std::find(path.begin(),path.end(),0);
        if(end==path.end() || !std::all_of(end,path.end(),[](auto c){return c==0;})) invalid();
        std::string value(path.begin(),end);
        if(!value.starts_with(prefix) || !value.ends_with(".xpf") ||
           value.size()<=prefix.size()+4 || value.find("..")!=std::string::npos) invalid();
        for(std::size_t j=prefix.size();j<value.size();++j) {
            const char c=value[j];
            if(!((c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='_' || c=='.')) invalid();
        }
        stages_[i]=std::move(value);
    }
    entries_.reserve(count);
    for(std::size_t i=0;i<count;++i) {
        const auto p=entry_base+i*12;
        const auto id=std::uint16_t(std::uint16_t(b[p]) | std::uint16_t(b[p+1])<<8);
        entries_.push_back({__builtin_bit_cast(std::int16_t, (id)),__builtin_bit_cast(std::int8_t, (b[p+2])),
            __builtin_bit_cast(std::int8_t, (b[p+3])),__builtin_bit_cast(float, (u32(b,p+4))),__builtin_bit_cast(float, (u32(b,p+8)))});
    }
}
bool BattleData::has_formation(int index) const {
    if(index<0 || index>=int(formations_.size())) throw std::runtime_error("Battle formation index out of range");
    return formations_[std::size_t(index)].count!=0xffffffffu;
}
std::span<const BattleFormationEntry> BattleData::formation(int index) const {
    if(!has_formation(index)) throw std::runtime_error("Absent battle formation");
    const auto r=formations_[std::size_t(index)];
    return std::span<const BattleFormationEntry>(entries_).subspan(r.first,r.count);
}
const std::string& BattleData::stage_path(int index) const {
    if(index<1 || index>int(stages_.size())) throw std::runtime_error("Battle stage index out of range");
    return stages_[std::size_t(index-1)];
}
BattleCameraData::BattleCameraData(Bytes b) {
    constexpr std::string_view magic="OKBTCAM1";
    constexpr std::size_t header=48,slot_size=4+0x90;
    if(b.size()<header||!std::equal(magic.begin(),magic.end(),b.begin())||
       u32(b,12)!=23||!std::equal(source_hash.begin(),source_hash.end(),b.begin()+16))
        throw std::runtime_error("Invalid battle camera data artifact");
    const auto version=u32(b,8);
    rows_per_stage_=version==1?2:version==2?5:0;
    if(rows_per_stage_==0||b.size()!=header+23*rows_per_stage_*3*slot_size)
        throw std::runtime_error("Invalid battle camera data artifact");
    std::size_t at=header;
    for(auto& stage:stages_)for(unsigned row_index=0;row_index<rows_per_stage_;++row_index)
      for(auto& slot:stage[row_index]) {
        const auto present=u32(b,at);at+=4;
        if(present>1)throw std::runtime_error("Invalid battle camera presence flag");
        if(present)std::copy_n(b.begin()+at,0x90,slot.emplace().begin());
        else if(!std::all_of(b.begin()+at,b.begin()+at+0x90,[](auto v){return v==0;}))
            throw std::runtime_error("Nonempty absent battle camera preset");
        at+=0x90;
    }
}
BattleCameraSequence BattleCameraData::sequence(int stage,bool after_intro) const {
    return sequence_at_offset(stage,after_intro?1u:0u);
}
BattleCameraSequence BattleCameraData::sequence_at_offset(int stage,unsigned row_offset) const {
    if(stage<1||stage>23)throw std::runtime_error("Battle camera stage index out of range");
    if(row_offset>=rows_per_stage_)
        throw std::runtime_error("Battle camera row offset is outside imported authored coverage");
    BattleCameraSequence result;
    const auto& row=stages_[stage-1][row_offset];
    for(unsigned i=0;i<3;++i)if(row[i])result[i]=read_battle_camera_preset(*row[i]);
    return result;
}
}
