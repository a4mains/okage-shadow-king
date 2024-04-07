#include "okage/name_entry.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
std::string format(std::string_view source,std::string_view value) {
    const auto at=source.find("%s");
    if(at==source.npos||source.find('%',at+2)!=source.npos||source.substr(0,at).find('%')!=source.npos)
        throw std::runtime_error("Unrecovered name entry text format");
    return std::string(source.substr(0,at))+std::string(value)+std::string(source.substr(at+2));
}
int coordinate(int base,int column,double step) {
    const int prior=std::fegetround();if(prior<0||std::fesetround(FE_TONEAREST))throw std::runtime_error("Name coordinate rounding");
    volatile double c=column,s=step,p=c*s,b=base,sum=b-p;
    const double value=sum;std::fesetround(prior);
    if(!std::isfinite(value)||value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())throw std::runtime_error("Name coordinate extent");
    return int(value);
}
}
void NameEntryInterface::second_update(std::span<std::uint8_t> variables,const GameDataTables& tables) {
    if(!(words_[0x90/4]&0x40)||!words_[0x9c/4])return;
    if(variables.size()<0x854)throw std::runtime_error("Name layout variable extent");
    auto& r=*resource_;constexpr int keep=RuwResource::preserve_component;
    const auto get=[&](unsigned index){if(index>7)throw std::runtime_error("Name layout word index");return std::bit_cast<std::int32_t>(words_[0xcc/4+index]);};
    const auto message=[&](int n)->const std::string&{if(n<0)throw std::runtime_error("Name UI message index");return tables.system_message(unsigned(n));};
    const auto rendered=[&](int style,std::string_view text){return format(message(style),text);};
    r.set_position(17,20,keep,get(3)*28+122);
    const int page=get(1);window().set_transform_parameter(128);
    const float scale=std::bit_cast<float>(0x3f99999au);
    for(int i=0;i<52;++i){const int id=i+43,column=i%13;r.set_position(9,id,coordinate(column*44+38,column,data_.spacing[0]),i/13*35+151);r.set_runtime_word0(9,id,50);r.set_runtime_float_pair(9,id,scale,1);}
    for(int i=52;i<118;++i){const int id=i+43;r.set_rgba(9,id,{keep,keep,keep,0});r.set_runtime_float_pair(9,id,scale,1);}
    for(int i=0;i<12;++i){const int id=128+i;r.set_position(9,id,coordinate(80+i*44-(i<10?42:0),i%13,data_.spacing[i<10?2:1]),322);r.set_rgba(9,id,{keep,keep,keep,128});r.set_runtime_word0(9,id,50);r.set_runtime_float_pair(9,id,scale,1);}
    for(int i=0;i<15;++i){const auto text=rendered(0xde,message(i+page*15));r.set_position(9,13+i,56,160+i*36);r.set_text(9,13+i,i<4?text:data_.empty);}
    r.set_text(9,28,rendered(0xde,message(30)));r.set_text(9,29,data_.empty);
    r.set_text(9,40,rendered(0xdf,message(33-page)));r.set_text(9,41,rendered(0xdf,message(34)));r.set_text(9,42,rendered(0xdf,message(35)));
    r.set_text(9,38,message(0xe0));
    const auto start=variables.begin()+0x844,end=std::find(start,variables.begin()+0x854,0);
    if(end==variables.begin()+0x854)throw std::runtime_error("Name field missing terminator");
    const std::string_view name(reinterpret_cast<const char*>(&*start),end-start);
    r.set_text(17,14,rendered(0xf7,name));r.set_text(17,15,message(0x69));r.set_text(17,16,message(0x6a));r.set_text(9,39,rendered(0xe1,name));
    const int selected=get(0),length=std::bit_cast<std::int32_t>(words_[0xa8/4]);
    const auto cursors=[&]{r.set_position(9,11,length*20+162,keep);r.set_position(9,144,length*20+162,keep);};
    if(selected<0) {
        if(selected<-3)throw std::runtime_error("Name special selection extent");
        for(int i=0;i<3;++i){if(-selected-1==i){r.set_rgba(9,31+i*2,{255,255,255,127});r.set_rgba(9,39-selected,{0,0,64,127});}else{r.reset_widget(9,31+i*2);r.reset_widget(9,40+i);}}
        r.set_rgba(9,10,{keep,keep,keep,0});r.set_rgba(9,12,{keep,keep,keep,0});cursors();return;
    }
    if(selected>=97)throw std::runtime_error("Name grid selection extent");
    const int text_row=selected<52?selected/13+page*5+36:46;
    const auto& letters=message(text_row);const unsigned offset=(selected<52?selected:selected-52)%13;
    if(offset>=letters.size())throw std::runtime_error("Name grid character extent");
    name_.selected_character={letters[offset],0};cursors();
    const auto selected_text=rendered(0xe2,std::string_view(name_.selected_character.data(),name_.selected_character[0]?1:0));
    for(int id:{31,33,35,40,41,42})r.reset_widget(9,id);
    const int row=selected/13,column=selected%13,extra=row==4&&column>=10?42:0;
    r.set_position(9,10,coordinate(column*44+extra+56,column,data_.spacing[3]),row*36+(row==4?191:159));
    const int x=coordinate(column*44+extra+41,column,data_.spacing[3]),y=row*35+(row==4?184:152);
    r.set_position(9,12,x,y);r.set_position(9,12,x,y); //source repeats the same helper call
    r.set_extent(9,12,28,26);r.set_text(9,10,selected_text);
    r.set_rgba(9,10,{0,0,64,127});r.set_rgba(9,12,{keep,keep,keep,127});
}
}
