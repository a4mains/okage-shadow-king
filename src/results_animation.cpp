#include "okage/results_animation.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
bool ResultsAnimation::animate_record(RuwResource& resource,const ResultsAnimationRecord& record,bool quick) {
    const auto& w=record.words;
    const auto kind=std::bit_cast<std::int32_t>(w[1]);
    if(kind<0||kind>2)return false;
    const auto id=std::bit_cast<std::int32_t>(w[0]);
    constexpr auto keep=RuwResource::preserve_component;
    if(kind==1) {
        resource.add_extent(13,id,std::int32_t(unsigned(quick)*400u+28u),keep,
                            std::bit_cast<std::int32_t>(w[2]),keep);
    }
    return resource.add_rgba(13,id,{keep,keep,keep,std::int32_t((unsigned(quick)<<7)|6u)})!=0;
}
void ResultsAnimation::run(RuwResource& resource,std::span<const ResultsAnimationRecord> table,
                          bool quick,bool bypass_timer,bool complete,bool count_items,
                          std::uint32_t positive_count) {
    bool last=false;
    std::uint32_t item_count=0;
    for(std::size_t i=0;;++i) {
        if(!bypass_timer&&std::bit_cast<std::int32_t>(timer)<std::int32_t(i*12))return;
        if(i>=table.size())throw std::runtime_error("Results animation missing terminator");
        const auto& record=table[i];
        if(record.words[0]==0xffffffffu&&record.words[1]==0xffffffffu) {
            if(complete&&last){timer=0;advance=1;}
            return;
        }
        if(count_items&&record.words[1]==2) {
            ++item_count;
            //16a4d0..4f8: count increments even when timer already>=240.
            if(std::bit_cast<std::int32_t>(timer)<240&&
               std::bit_cast<std::int32_t>(item_count)>=std::bit_cast<std::int32_t>(positive_count))timer=240;
        }
        last=animate_record(resource,record,quick);
    }
}
void ResultsAnimation::heading(RuwResource& resource,const ResultsUiData& data) {
    run(resource,data.animation_table(0),false,false,true,false,0);
}
void ResultsAnimation::items(RuwResource& resource,const ResultsUiData& data,unsigned positive_count) {
    run(resource,data.animation_table(1),false,false,true,true,positive_count);
}
void ResultsAnimation::currency(RuwResource& resource,const ResultsUiData& data,bool quick) {
    run(resource,data.animation_table(2),quick,false,false,false,0);
}
void ResultsAnimation::party_heading(RuwResource& resource,const ResultsUiData& data,bool quick) {
    run(resource,data.animation_table(3),quick,quick,true,false,0);
}
}
