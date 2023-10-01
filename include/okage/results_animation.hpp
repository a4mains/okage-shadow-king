#pragma once
#include "okage/results_data.hpp"
#include "okage/ruw.hpp"

namespace okage {
class ResultsAnimation {
public:
    std::uint32_t timer=0,advance=0;
    void heading(RuwResource&,const ResultsUiData&);                  //16a218
    void items(RuwResource&,const ResultsUiData&,unsigned positive_count); //16a398
    void currency(RuwResource&,const ResultsUiData&,bool quick);     //16a5a8
    void party_heading(RuwResource&,const ResultsUiData&,bool quick);//16a928
private:
    static bool animate_record(RuwResource&,const ResultsAnimationRecord&,bool quick); //16a128
    void run(RuwResource&,std::span<const ResultsAnimationRecord>,bool quick,
             bool bypass_timer,bool complete,bool count_items,std::uint32_t positive_count);
};
}
