#include "okage/battle_hud.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <iomanip>
#include <sstream>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
constexpr int keep=RuwResource::preserve_component;
std::uint32_t word(Bytes b,unsigned at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("HUD data bounds");
    return unsigned(b[at])|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);
}
int half(Bytes b,unsigned at) {
    return __builtin_bit_cast(std::int16_t, (std::uint16_t(unsigned(b[at])|(unsigned(b[at+1])<<8))));
}
struct Rounding {
    int old=std::fegetround();
    explicit Rounding(int mode) {
        if(old<0||std::fesetround(mode))throw std::runtime_error("HUD rounding unavailable");
    }
    ~Rounding(){std::fesetround(old);}
};
std::string format_reached(std::string_view source,std::span<const std::string> args,
                   std::span<const std::string_view> conversions) {
    std::string out;unsigned argument=0;
    for(std::size_t i=0;i<source.size();) {
        if(source[i]!='%'){out+=source[i++];continue;}
        if(argument>=args.size()||argument>=conversions.size()||
           source.substr(i,conversions[argument].size())!=conversions[argument])
            throw std::runtime_error("Unrecovered HUD string conversion");
        out+=args[argument];i+=conversions[argument++].size();
    }
    if(argument!=args.size())throw std::runtime_error("HUD format argument count");
    return out;
}
template<std::size_t N> void copy(std::array<char,N>& target,std::string_view value) {
    if(value.size()>=N)throw std::runtime_error("HUD scratch text extent");
    std::copy(value.begin(),value.end(),target.begin());target[value.size()]=0;
}
template<std::size_t N> std::string_view text(const std::array<char,N>& value) {
    const auto end=std::find(value.begin(),value.end(),0);
    if(end==value.end())throw std::runtime_error("HUD scratch lacks terminator");
    return {value.data(),std::size_t(end-value.begin())};
}
void alpha(RuwResource& r,unsigned group,int id,int value) {r.set_rgba(group,id,{keep,keep,keep,value});}
void color(RuwResource& r,unsigned group,int id,std::array<int,3> rgb) {r.set_rgba(group,id,{rgb[0],rgb[1],rgb[2],keep});}
void icons(RuwResource& r,const BattleHudData& data,unsigned row,Bytes c,bool selected) {
    const int category=__builtin_bit_cast(std::int8_t, (c[0])),first=half(c,0x4a),
        second=__builtin_bit_cast(std::int8_t, (c[1])),third=half(c,0x32);
    std::array<int,3> rgb{195,191,215};int icon=38;
    if(!first) {if(category==6)rgb={0,0,0};}
    else {
        if(category<1||category>6)throw std::runtime_error("HUD status palette index outside source table");
        rgb=data.palette()[category-1];icon=first+71;
    }
    const auto triplet=[&](unsigned column,int value,std::array<int,3> tint) {
        if(selected&&column<2){r.set_runtime_word0(2,22+column,std::uint32_t(value));color(r,2,22+column,tint);}
        for(auto [group,base]:{std::pair{2u,25u},std::pair{5u,14u},std::pair{3u,47u}}) {
            const unsigned id=base+row*3+column;
            r.set_runtime_word0(group,id,std::uint32_t(value));color(r,group,id,tint);
        }
    };
    triplet(0,icon,rgb);
    triplet(1,second?second+61:38,second?std::array<int,3>{255,255,255}:std::array<int,3>{195,191,215});
    switch(third) {
    case 0:rgb={254,65,22};break;
    case 1:rgb={132,76,247};break;
    case 2:rgb={247,204,70};break;
    case 3:rgb={255,255,255};break;
    default:return;
    }
    if(selected)color(r,2,52,rgb);
    color(r,2,27+row*3,rgb);color(r,5,16+row*3,rgb);color(r,3,49+row*3,rgb);
}
void row_visibility(RuwResource& r,unsigned row,int value) {
    for(unsigned id:{16+row,19+row,25+row*3,26+row*3,27+row*3,42+row,45+row})alpha(r,2,id,value);
    for(unsigned id:{42+row,45+row})r.set_secondary_rgba(2,id,{keep,keep,keep,value});
    for(unsigned id:{14+row*3,15+row*3,16+row*3})alpha(r,5,id,value);
}
}
BattleHudData::BattleHudData(Bytes b) {
    constexpr std::array<std::uint8_t,6> magic={'O','K','B','H','U','D'};
    const bool v2=b.size()==480;
    if((b.size()!=352&&!v2)||!std::equal(magic.begin(),magic.end(),b.begin())||
       b[6]!=(v2?'2':'1')||b[7]||word(b,8)!=(v2?2u:1u)||word(b,12)!=(v2?400u:272u))
        throw std::runtime_error("Unsupported battle HUD import");
    constexpr std::string_view hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<64;++i)if(hash[i]!=digits[(b[16+i/2]>>(i%2?0:4))&15])
        throw std::runtime_error("Unsupported battle HUD source revision");
    for(unsigned row=0;row<6;++row)for(unsigned col=0;col<3;++col)
        palette_[row][col]=__builtin_bit_cast(std::int32_t, (word(b,80+row*12+col*4)));
    warning_fraction_=__builtin_bit_cast(double, (std::uint64_t(word(b,152))|(std::uint64_t(word(b,156))<<32)));
    if(!std::isfinite(warning_fraction_)||warning_fraction_<0||warning_fraction_>1)
        throw std::runtime_error("HUD warning threshold outside recovered scope");
    if(v2)short_formats_.emplace();
    for(unsigned i=0;i<(v2?5u:3u);++i) {
        const auto first=b.begin()+160+i*64,last=first+64,end=std::find(first,last,0);
        if(end==last||!std::all_of(end,last,[](auto c){return c==0;}))throw std::runtime_error("HUD format terminator/padding");
        auto& destination=i<3?formats_[i]:(*short_formats_)[i-3];
        destination.assign(reinterpret_cast<const char*>(&*first),end-first);
    }
}
std::string BattleHudData::number(std::int32_t value) const {
    const auto tint=std::to_string(0x1e6e46c3);
    if(value<=0) {
        const std::array args{tint};const std::array<std::string_view,1> spec{"%d"};
        return format_reached(formats_[0],args,spec);
    }
    auto digits=std::to_string(std::min(value,999));digits.insert(0,3-digits.size(),'0');
    const std::array number_arg{digits};const std::array<std::string_view,1> decimal_spec{"%03d"};
    const auto decimal=format_reached(formats_[1],number_arg,decimal_spec);
    const auto significant=decimal.find_first_of("123456789");
    if(significant==decimal.npos)throw std::runtime_error("HUD number lacks significant digit");
    const std::array args{tint,decimal.substr(0,significant),decimal.substr(significant)};
    const std::array<std::string_view,3> spec{"%d","%s","%s"};
    return format_reached(formats_[2],args,spec);
}
std::string BattleHudData::number2(std::int32_t value) const {
    if(!short_formats_)throw std::runtime_error("Two-digit HUD formatting requires v2 owned import");
    const auto tint=std::to_string(0x1e6e46c3);
    if(value<=0) {
        const std::array args{tint};const std::array<std::string_view,1> spec{"%d"};
        return format_reached((*short_formats_)[0],args,spec);
    }
    auto digits=std::to_string(std::min(value,99));digits.insert(0,2-digits.size(),'0');
    const std::array number_arg{digits};const std::array<std::string_view,1> decimal_spec{"%02d"};
    const auto decimal=format_reached((*short_formats_)[1],number_arg,decimal_spec);
    const auto significant=decimal.find_first_of("123456789");
    if(significant==decimal.npos)throw std::runtime_error("HUD number lacks significant digit");
    const std::array args{tint,decimal.substr(0,significant),decimal.substr(significant)};
    const std::array<std::string_view,3> spec{"%d","%s","%s"};
    return format_reached(formats_[2],args,spec);
}
void BattleHud::refresh(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
                        Bytes vars,const UiPartySelection& party,unsigned mode,int selected) {
    if(mode==4)selected=0;
    if(menu_state)return;
    if(vars.size()<0xd78)throw std::runtime_error("HUD shared variable extent");
    bank2_bound=true;if(++mode_counter>120)mode_counter=0;
    for(unsigned row=0;row<3;++row) {
        copy(padded_name,tables.system_message(0x8b));
        r.set_text(2,38+row,text(padded_name));r.set_text(3,60+row*2,text(padded_name));
        const int unit=party.at(2,row),slot=party.at(3,row);
        if(unit==-1) {
            r.set_text(2,38+row,tables.system_message(0x8b));
            copy(padded_name,tables.system_message(0x8b));row_visibility(r,row,0);
        } else {
            copied_unit=tables.ui_unit(unit);
            if(slot<0||slot>=8)continue;
            const auto c=vars.subspan(0x5c4+unsigned(slot)*0x50,0x50);
            const auto hp=__builtin_bit_cast(std::int32_t, (word(c,8)));const int max=half(c,0x18),charge=half(c,0x26);
            if(!max)throw std::runtime_error("HUD zero maximum HP division is unrecovered");
            float hp_fraction,charge_fraction;
            {
                Rounding rounding(FE_TOWARDZERO);
                hp_fraction=float(hp)/float(max);charge_fraction=float(charge)/100.f;
            }
            r.set_fill_fraction(2,42+row,hp_fraction);r.set_fill_fraction(2,45+row,charge_fraction);
            copy(current_number,data.number(hp));copy(maximum_number,data.number(max));
            const std::array args{std::string(text(current_number)),std::string(text(maximum_number))};
            const std::array<std::string_view,2> spec{"%s","%s"};
            copy(formatted,format_reached(tables.system_message(0x55),args,spec));
            r.set_text(2,38+row,text(formatted));r.set_text(3,60+row*2,text(formatted));
            bool low;
            {
                Rounding rounding(FE_TONEAREST); // Source software-double helpers.
                const double threshold=double(max)*data.warning_fraction();low=double(hp)<threshold;
            }
            std::array<int,3> rgb{255,255,255};
            if(low) {if(!selection_state)selection_state=1;if(mode_counter%60<30)rgb={190,40,45};}
            if(!hp)rgb={190,40,45};color(r,2,38+row,rgb);color(r,3,60+row*2,rgb);
            copy(padded_name,tables.system_message(0x8b));
            std::string_view name=copied_unit->name;
            if(slot==0) {
                const auto first=vars.begin()+0x844,end=std::find(first,vars.end(),0);
                if(end==vars.end())throw std::runtime_error("HUD player name lacks terminator");
                name={reinterpret_cast<const char*>(&*first),std::size_t(end-first)};
            }
            if(name.size()>padded_name.size())throw std::runtime_error("HUD name copy extent");
            // Original memcpy omits NUL: short names keep authored trailing spaces.
            std::copy(name.begin(),name.end(),padded_name.begin());(void)text(padded_name);
            icons(r,data,row,c,selected==int(row));row_visibility(r,row,128);
            if(!selection_state)selection_state=2;
            alpha(r,2,35+row,selected==int(row)?__builtin_bit_cast(std::int32_t, (selected_alpha)):128);
        }
        const std::array args{std::string(text(padded_name))};const std::array<std::string_view,1> spec{"%s"};
        copy(formatted,format_reached(tables.system_message(0x90),args,spec));
        r.set_text(2,35+row,text(formatted));r.set_text(3,59+row*2,text(formatted));
    }
}
void BattleHud::initialize_type2(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
                               Bytes vars,const UiPartySelection& party,unsigned mode,int selected) {
    type2_flag_b=0;type2_flag_a=0;
    reset_type2_widgets(r,data,tables,vars,party,mode,selected);
    type2_work.fill(0);type2_enabled=1;
}
void BattleHud::reset_type2_widgets(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
                                    Bytes vars,const UiPartySelection& party,unsigned mode,int selected) {
    r.reset_class(3,1);r.reset_class(3,2);r.reset_widget(4,1);r.reset_widget(2,9);
    r.reset_class(2,0);r.reset_class(2,1);r.reset_class(4,0);
    r.add_class_rgba(2,-1,{keep,keep,keep,128});
    for(int cls:{-1,0,1,2,3,4,10})r.set_class_rgba(3,cls,{keep,keep,keep,0});
    for(int cls:{-1,0})r.set_class_rgba(4,cls,{keep,keep,keep,0});
    row_flags.fill(0);r.add_class_rgba(2,3,{keep,keep,keep,128});flag_a8=0;
    r.set_class_rgba(2,4,{keep,keep,keep,0});flag_a0=0;
    refresh(r,data,tables,vars,party,mode,selected);
    r.add_rgba(4,1,{keep,keep,keep,-256});
}
void BattleHud::refresh_command_costs(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
    Bytes vars,const UiPartySelection& party,unsigned mode,int selected,std::int32_t cost,
    std::int32_t kind,std::int32_t pulse,std::int32_t hp_gate) {
    if(mode==4)selected=0;
    const int slot=party.at(3,selected);if(slot<0||slot>=8)return;
    if(vars.size()<0xd78)throw std::runtime_error("HUD shared variable extent");
    const auto c=vars.subspan(0x5c4+unsigned(slot)*0x50,0x50);
    const auto subtract=[](std::int32_t a,std::int32_t b){return __builtin_bit_cast(std::int32_t, (std::uint32_t(a)-std::uint32_t(b)));};
    const auto resources=std::min(__builtin_bit_cast(std::int32_t, (word(vars,0x590))),99);
    const auto maximum=std::min(__builtin_bit_cast(std::int32_t, (word(vars,0x594))),99);
    const auto format=[&](bool resource,std::int32_t current,std::int32_t max) {
        copy(current_number,resource?data.number2(current):data.number(current));
        copy(maximum_number,resource?data.number2(max):data.number(max));
        const std::array args{std::string(text(current_number)),std::string(text(maximum_number))};
        const std::array<std::string_view,2> spec{"%s","%s"};
        copy(formatted,format_reached(tables.system_message(resource?0x8f:0x55),args,spec));
    };
    const auto paint=[&](bool resource,std::array<int,3> rgb) {
        color(r,2,resource?41:49,rgb);color(r,4,resource?26:25,rgb);
    };
    format(true,resources,maximum);
    if(kind==1&&pulse%120<60&&cost>0) {
        const auto remaining=subtract(resources,cost);
        if(remaining>=0){format(true,remaining,maximum);paint(true,{230,160,40});}
        // Source bltz15ef78 preserves the previous resource colors.
    } else paint(true,{255,255,255});
    r.set_text(2,41,text(formatted));r.set_text(4,26,text(formatted));
    const auto hp=__builtin_bit_cast(std::int32_t, (word(c,8)));const auto max_hp=half(c,0x18);
    format(false,hp,max_hp);
    if(kind==0&&pulse%120<60&&cost>0&&subtract(hp,hp_gate)>0) {
        format(false,subtract(hp,cost),max_hp);paint(false,{230,160,40});
    } else paint(false,{255,255,255});
    r.set_text(2,49,text(formatted));r.set_text(4,25,text(formatted));
}
void BattleHud::refresh_command_party(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
    Bytes vars,const UiPartySelection& party,unsigned mode,int selected,std::int32_t display_mode,
    std::int32_t available,std::int32_t& countdown,std::int32_t command_alpha) {
    if(mode==4)selected=0;
    if(vars.size()<0xd78)throw std::runtime_error("HUD shared variable extent");
    bank2_bound=true;
    if(display_mode==0) {
        r.add_class_rgba(2,5,{keep,keep,keep,128});
        if(available<2) {
            r.set_class_rgba(2,5,{keep,keep,keep,0});alpha(r,2,53,0);alpha(r,2,54,0);
            r.set_runtime_word0(2,14,28);
        } else {
            if(!countdown)countdown=2;
            for(auto [id,x]:{std::pair{22,462},std::pair{23,478},std::pair{52,494},std::pair{34,504}})
                r.set_position(2,id,x,keep);
            alpha(r,2,22,0);alpha(r,2,23,0);alpha(r,2,52,0);
            alpha(r,2,54,command_alpha);alpha(r,2,53,command_alpha);alpha(r,2,54,command_alpha);
            r.set_runtime_word0(2,14,11);
        }
    }
    const int slot=party.at(3,selected);
    const auto unit=__builtin_bit_cast(std::int32_t, (std::uint32_t(slot)+170u));
    copied_unit=tables.ui_unit(unit);
    const std::array<std::string,0> no_args{};const std::array<std::string_view,0> no_spec{};
    copy(padded_name,format_reached(tables.system_message(0x7e),no_args,no_spec));
    std::string_view name=copied_unit->name;
    if(slot==0) {
        const auto first=vars.begin()+0x844,end=std::find(first,vars.end(),0);
        if(end==vars.end())throw std::runtime_error("HUD player name lacks terminator");
        name={reinterpret_cast<const char*>(&*first),std::size_t(end-first)};
    }
    if(name.size()>padded_name.size())throw std::runtime_error("HUD name copy extent");
    std::copy(name.begin(),name.end(),padded_name.begin());(void)text(padded_name);
    const std::array name_args{std::string(text(padded_name))};const std::array<std::string_view,1> name_spec{"%s"};
    copy(formatted,format_reached(tables.system_message(0x8e),name_args,name_spec));
    r.set_text(2,34,text(formatted));r.set_text(4,36,text(formatted));
    const auto format=[&](bool resource,std::int32_t current,std::int32_t max) {
        copy(current_number,resource?data.number2(current):data.number(current));
        copy(maximum_number,resource?data.number2(max):data.number(max));
        const std::array args{std::string(text(current_number)),std::string(text(maximum_number))};
        const std::array<std::string_view,2> spec{"%s","%s"};
        copy(formatted,format_reached(tables.system_message(resource?0x8f:0x55),args,spec));
    };
    format(true,std::min(__builtin_bit_cast(std::int32_t, (word(vars,0x590))),99),
                std::min(__builtin_bit_cast(std::int32_t, (word(vars,0x594))),99));
    color(r,2,41,{255,255,255});color(r,4,26,{255,255,255});
    r.set_text(2,41,text(formatted));r.set_text(4,26,text(formatted));
    if(slot<0||slot>=8)return;
    const auto c=vars.subspan(0x5c4+unsigned(slot)*0x50,0x50);
    format(false,__builtin_bit_cast(std::int32_t, (word(c,8))),half(c,0x18));
    r.set_text(2,49,text(formatted));r.set_text(4,25,text(formatted));
    color(r,2,49,{255,255,255});color(r,4,25,{255,255,255});
}
void BattleHud::reset_type3(RuwResource& r) {
    type3_reset=0;r.add_class_rgba(5,-1,{keep,keep,keep,128});
    r.set_class_rgba(5,0,{keep,keep,keep,0});type3_ready=1;
}
void BattleHud::initialize_type3(RuwResource& r,const GameDataTables& tables) {
    reset_type3(r);type3_ready=1;type3_animation=0;type3_slots.fill(-1);type3_cursor=0;
    for(unsigned i=0;i<5;++i) {
        r.set_text(5,29+i,tables.system_message(0xa6));r.set_text(5,39+i,tables.system_message(0xa6));
        r.set_runtime_word0(5,34+i,71);
    }
    for(unsigned i=0;i<4;++i)r.set_position(5,48+i,keep,359+i*17);
}
void BattleHud::refresh_type3(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
                             Bytes vars,const UiPartySelection& party) {
    if(vars.size()<0xd78)throw std::runtime_error("Type3 HUD shared variable extent");
    const std::array<std::string_view,1> one_string{"%s"};
    const std::array<std::string_view,2> two_strings{"%s","%s"};
    for(unsigned row=0;row<3;++row) {
        r.set_text(5,26+row,tables.system_message(0x8b));
        const int unit=party.at(2,row),slot=party.at(3,row);
        if(unit==-1) {
            for(unsigned id:{8+row*2,9+row*2})alpha(r,5,id,0);
            for(unsigned id:{8+row*2,9+row*2})r.set_secondary_rgba(5,id,{keep,keep,keep,0});
            copy(type3_padded_name,format_reached(tables.system_message(0x8b),{},{}));
        } else {
            // 125400 copies into the stack here, not163698's persistent copy.
            const auto& copied=tables.ui_unit(unit);
            if(slot<0||slot>=8)continue;
            const auto c=vars.subspan(0x5c4+unsigned(slot)*0x50,0x50);
            const auto hp=__builtin_bit_cast(std::int32_t, (word(c,8)));
            const int maximum=half(c,0x18),charge=half(c,0x26);
            copy(type3_current_number,data.number(hp));copy(type3_maximum_number,data.number(maximum));
            const std::array args{std::string(text(type3_current_number)),std::string(text(type3_maximum_number))};
            copy(type3_formatted,format_reached(tables.system_message(0xa3),args,two_strings));
            r.set_text(5,26+row,text(type3_formatted));
            if(!maximum)throw std::runtime_error("Type3 HUD zero maximum HP division is unrecovered");
            float hp_fraction,charge_fraction;
            {
                Rounding rounding(FE_TOWARDZERO);
                hp_fraction=float(hp)/float(maximum);charge_fraction=float(charge)/100.f;
            }
            r.set_fill_fraction(5,8+row*2,hp_fraction);r.set_fill_fraction(5,9+row*2,charge_fraction);
            for(unsigned id:{8+row*2,9+row*2})alpha(r,5,id,128);
            for(unsigned id:{8+row*2,9+row*2})r.set_secondary_rgba(5,id,{keep,keep,keep,128});
            copy(type3_padded_name,format_reached(tables.system_message(0x8b),{},{}));
            std::string_view name=copied.name;
            if(slot==0) {
                const auto first=vars.begin()+0x844,end=std::find(first,vars.end(),0);
                if(end==vars.end())throw std::runtime_error("Type3 HUD player name lacks terminator");
                name={reinterpret_cast<const char*>(&*first),std::size_t(end-first)};
            }
            if(name.size()>type3_padded_name.size())throw std::runtime_error("Type3 HUD name copy extent");
            std::copy(name.begin(),name.end(),type3_padded_name.begin());(void)text(type3_padded_name);
        }
        const std::array args{std::string(text(type3_padded_name))};
        copy(type3_formatted,format_reached(tables.system_message(0xa4),args,one_string));
        r.set_text(5,23+row,text(type3_formatted));
    }
    // Source performs the upper clamp before124200, which also clamps to99.
    const auto current=std::min(__builtin_bit_cast(std::int32_t, (word(vars,0x590))),99);
    const auto maximum=std::min(__builtin_bit_cast(std::int32_t, (word(vars,0x594))),99);
    copy(type3_current_number,data.number2(current));copy(type3_maximum_number,data.number2(maximum));
    const std::array args{std::string(text(type3_current_number)),std::string(text(type3_maximum_number))};
    copy(type3_formatted,format_reached(tables.system_message(0xa5),args,two_strings));
    r.set_text(5,44,text(type3_formatted));type3_ready=1;
}
void BattleHud::update_type3(RuwResource& r,const BattleHudData& data,const GameDataTables& tables,
                            Bytes vars,const UiPartySelection& party,unsigned mode,
                            const std::function<void(bool)>& set_layout_enabled) {
    if(!set_layout_enabled)throw std::runtime_error("Type3 HUD requires window layout setter");
    set_layout_enabled(false);
    refresh_type3(r,data,tables,vars,party);
    if(type3_ready==1)set_layout_enabled(true);
    type3_ready=0;
    if(!type3_reset) {
        r.add_class_rgba(5,-1,{keep,keep,keep,128});
        r.set_class_rgba(5,0,{keep,keep,keep,0});
        alpha(r,5,52,64);alpha(r,5,53,64);
        for(unsigned id=48;id<52;++id)alpha(r,5,id,64);
    } else {
        r.set_class_rgba(5,-1,{keep,keep,keep,0});
        r.add_class_rgba(5,0,{keep,keep,keep,128});
    }
    if(mode==4)throw std::runtime_error("Type3 HUD demo callback is unrecovered");
    if(type3_reset)return;
    type3_blink=std::uint32_t(__builtin_bit_cast(std::int32_t, (type3_blink+1u))%60);
    for(unsigned row=0;row<3;++row) {
        for(unsigned id:{14+row*3,15+row*3,16+row*3,8+row*2,9+row*2})
            r.add_rgba(5,id,{keep,keep,keep,-128});
        for(unsigned id:{8+row*2,9+row*2})r.add_secondary_rgba(5,id,{keep,keep,keep,-128});
        alpha(r,5,54+row,0);
    }
    if(party.counts()[3]>3)throw std::runtime_error("Type3 HUD party exceeds authored row scope");
    for(unsigned row=0;row<party.counts()[3];++row) {
        const int slot=party.at(3,row);if(slot<0||slot>=8)continue;
        const auto c=vars.subspan(0x5c4+unsigned(slot)*0x50,0x50);
        for(unsigned id:{14+row*3,15+row*3,16+row*3,8+row*2,9+row*2})
            r.add_rgba(5,id,{keep,keep,keep,128});
        for(unsigned id:{8+row*2,9+row*2})r.add_secondary_rgba(5,id,{keep,keep,keep,128});
        alpha(r,5,54+row,128);
        icons(r,data,row,c,false);
        const auto hp=__builtin_bit_cast(std::int32_t, (word(c,8)));const auto maximum=half(c,0x18);
        bool low;
        {
            Rounding rounding(FE_TONEAREST);
            low=double(hp)<double(maximum)*data.warning_fraction();
        }
        std::array<int,3> rgb{255,255,255};
        if(low&&__builtin_bit_cast(std::int32_t, (type3_blink))%60<30)rgb={190,40,45};
        color(r,5,26+row,rgb);
        if(hp==0)color(r,5,26+row,{190,40,45});
    }
}

std::string BattleHud::diagnostic(const RuwResource& r,Bytes vars,std::string_view execution) const {
    if(execution!="isolated"&&execution!="native-session"&&execution!="ui-update")throw std::runtime_error("HUD diagnostic execution tag");
    const auto& h=*this;std::ostringstream out;
    const auto hex=[](Bytes b){std::ostringstream s;for(auto v:b)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return s.str();};
    out<<"{\"schema\":\"okage-battle-hud-diagnostic-v1\",\"scope\":\""
       <<(execution=="ui-update"?"RUW and HUD state at an isolated dispatcher boundary":
       "163fb8 and 168228 RUW/stat side effects only; zero incoming party cache; no manager or callbacks")
       <<"\",\"execution\":\""<<execution<<"\",\"game_mode\":3,\"selected\":0,\"vars_hex\":\""<<hex(vars)<<"\",\"globals\":{";
    out<<"\"type2_flag_a\":"<<h.type2_flag_a<<",\"type2_flag_b\":"<<h.type2_flag_b<<",\"type2_enabled\":"<<h.type2_enabled
       <<",\"type3_ready\":"<<h.type3_ready<<",\"type3_reset\":"<<h.type3_reset<<",\"type3_animation\":"<<h.type3_animation
       <<",\"type3_selection\":"<<h.type3_cursor<<",\"ui_menu_state\":"<<h.menu_state<<",\"ui_mode_counter\":"<<h.mode_counter
       <<",\"ui_selection_pointer\":"<<h.selection_state<<"},\"groups\":[";
    bool group_comma=false;
    for(const auto& group:r.widgets()) {
        if(group_comma)out<<',';group_comma=true;out<<'[';bool comma=false;
        for(const auto& w:group) {
            if(comma)out<<',';comma=true;out<<"{\"words\":[";
            for(unsigned i=0;i<17;++i){if(i)out<<',';out<<w.words[i];}out<<"],\"runtime\":";
            if(w.runtime) {out<<'[';for(unsigned i=0;i<10;++i){if(i)out<<',';out<<w.runtime->words[i];}out<<']';}
            else out<<"null";
            out<<",\"text_hex\":";
            if(w.runtime&&w.runtime->text)out<<'"'<<hex(okage::Bytes(reinterpret_cast<const std::uint8_t*>(w.runtime->text->data()),128))<<'"';
            else out<<"null";
            out<<'}';
        }
        out<<']';
    }
    out<<"]}\n";return out.str();
}
}
