#include "okage/results_progress.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
#include <string_view>

namespace okage {
namespace {
constexpr std::array<std::size_t,6> bank_offsets{0x000,0x2d0,0x0f0,0x1e0,0x3c0,0x4b0};
std::uint16_t half(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<2)throw std::runtime_error("Results halfword bounds");
    return static_cast<std::uint16_t>(std::uint16_t(b[at])|
        static_cast<std::uint16_t>(std::uint16_t(b[at+1])<<8));
}
std::int16_t shalf(Bytes b,std::size_t at) {return std::bit_cast<std::int16_t>(half(b,at));}
void put16(std::span<std::uint8_t> b,std::size_t at,std::uint16_t v) {
    if(at>b.size()||b.size()-at<2)throw std::runtime_error("Results halfword write bounds");
    b[at]=std::uint8_t(v);b[at+1]=std::uint8_t(v>>8);
}
void put32(std::span<std::uint8_t> b,std::size_t at,std::uint32_t v) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Results word write bounds");
    for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(v>>(i*8));
}
std::string format_reached(std::string_view format,std::span<const std::string> args) {
    std::string out;std::size_t arg=0;
    for(std::size_t i=0;i<format.size();) {
        if(format[i]!='%'){out+=format[i++];continue;}
        if(arg>=args.size())throw std::runtime_error("Results message has extra conversion");
        if(format.substr(i,2)!="%s")throw std::runtime_error("Unrecovered results message conversion");
        i+=2;
        out+=args[arg++];
    }
    if(arg!=args.size())throw std::runtime_error("Results message conversion count");
    return out;
}
std::string value_text(const BattleHudData& data,std::int32_t v) {return data.number(v);}
std::string value_text2(const BattleHudData& data,std::int32_t v) {return data.number2(v);}
void set_alpha(RuwResource& r,int id,bool visible) {
    if(visible)r.set_rgba(13,id,{RuwResource::preserve_component,RuwResource::preserve_component,
                                 RuwResource::preserve_component,128});
    else r.add_rgba(13,id,{RuwResource::preserve_component,RuwResource::preserve_component,
                           RuwResource::preserve_component,-128});
}
unsigned selected_count(const UiPartySelection& party) {
    const auto n=party.counts()[3];
    if(n>3)throw std::runtime_error("Results source snapshot storage supports three selected rows");
    return n;
}
unsigned selected_slot(const UiPartySelection& party,unsigned row) {
    const auto slot=party.at(3,row);
    if(slot<0||slot>=7)throw std::runtime_error("Results selected character slot outside constructor scope");
    return unsigned(slot);
}
unsigned selected_unit(const UiPartySelection& party,unsigned row) {
    const auto unit=party.at(2,row);
    if(unit<=0)throw std::runtime_error("Results selected unit ID outside source table");
    return unsigned(unit);
}
std::size_t char_at(unsigned slot) {return 0x5c4u+std::size_t(slot)*0x50u;}
}

void ResultsProgress::initialize(std::span<std::uint8_t> variables,const GameDataTables& tables,
                                 const UiPartySelection& party,std::span<std::uint8_t> reward,
                                 RuwResource& r,const BattleHudData& hud,unsigned mode,
                                 const std::function<void(unsigned)>& play_jingle) {
    if(mode!=2)throw std::runtime_error("Results progression currently covers original game mode 2 only");
    if(variables.size()<0xd78)throw std::runtime_error("Results variable owner is truncated");
    if(reward.size()<0x32)throw std::runtime_error("Results reward owner is truncated");
    const unsigned count=selected_count(party);
    // Constructor-owned shared fields are reset before iterating rows.
    notice=0;notice_row=0;notice_wait=0;notice_counters.fill(-1);
    for(unsigned row=0;row<count;++row) {
        const unsigned slot=selected_slot(party,row),unit=selected_unit(party,row);
        refresh_game_character(tables,variables,slot);
        const auto at=char_at(slot);
        if(at>variables.size()||variables.size()-at<0x50)
            throw std::runtime_error("Results selected character record is truncated");
        const auto name=tables.ui_unit(int(unit)).name;
        (void)name;

        const std::size_t grant_at=0x26+row*4;
        auto grant=std::int32_t(shalf(reward,grant_at));
        if(grant>9999)grant=9999;
        const auto level=shalf(variables,at+4);
        auto xp=shalf(variables,at+6);
        if(level==99)grant=0;
        if(grant==0&&((level==99&&xp<999)||level<99))grant=1;
        if(variables[at]==6)grant=0;
        grant=std::bit_cast<std::int16_t>(std::uint16_t(grant));
        put16(reward,grant_at,std::uint16_t(grant));

        // 0x125c58's six calls are visibly ordered and each copy is 0x52
        // bytes at a 0x50 stride. The two-byte overlaps must be retained.
        for(const auto bank:bank_offsets) {
            const auto image=snapshot_game_character_at_level(variables,tables,slot,level);
            const auto dst=bank+std::size_t(row)*0x50;
            std::copy(image.begin(),image.end(),scratch.begin()+dst);
        }

        const std::int32_t total=std::int32_t(xp)+grant;
        const std::int32_t new_level=std::int32_t(std::uint16_t(level))+total/1000;
        const auto remainder=total%1000;
        const auto narrowed_level=std::bit_cast<std::int16_t>(std::uint16_t(new_level));
        put16(variables,at+4,std::uint16_t(narrowed_level>99?99:narrowed_level));
        put16(variables,at+6,std::uint16_t(remainder));

        // 0x169900's derived scratch row and earned-level total.
        put16(scratch,bank_offsets[3]+row*0x50+4,std::uint16_t(narrowed_level>99?99:narrowed_level));
        put16(scratch,bank_offsets[3]+row*0x50+6,std::uint16_t(remainder));
        const auto total16=std::bit_cast<std::int16_t>(std::uint16_t(total));
        put16(scratch,bank_offsets[2]+row*0x50+6,std::uint16_t(total16));
        const std::size_t earned_at=0x5a0+row*4;
        put32(scratch,earned_at,std::uint32_t(std::int32_t(total16)));

        // The original invokes these before advancing to the next row; keep
        // that order so later 0x52-byte snapshot overlaps match source writes.
        (void)update_character(row,true,variables,tables,party,r,hud,play_jingle);
        r.set_runtime_word0(13,int(row+0x28),std::uint32_t(slot+0x38));
        hide_changed(r,row);
    }
}

bool ResultsProgress::update_character(unsigned row,bool initial,
        std::span<std::uint8_t> variables,const GameDataTables& tables,
        const UiPartySelection& party,RuwResource& r,const BattleHudData& hud,
        const std::function<void(unsigned)>& play_jingle) {
    if(row>=selected_count(party))throw std::runtime_error("Results row outside selected party");
    const unsigned slot=selected_slot(party,row),unit=selected_unit(party,row);
    const auto at=char_at(slot);const std::size_t base=std::size_t(row)*0x50;
    if(variables.size()<0xd78||at>variables.size()||variables.size()-at<0x50)
        throw std::runtime_error("Results update variable extent");
    const auto& unit_data=tables.ui_unit(int(unit));
    const auto bank=[&](unsigned b,std::size_t off){return shalf(scratch,bank_offsets[b]+base+off);};
    const int idbase=int(row*13+0x46);
    const auto show=[&](unsigned i,std::string_view text,int second=-1,bool changed=false) {
        r.set_text(13,idbase+int(i),text);
        set_alpha(r,idbase+int(i),changed);
        if(second>=0)set_alpha(r,second,changed);
    };
    const auto fmt=[&](unsigned msg,std::initializer_list<std::string> args) {
        return format_reached(tables.system_message(msg),std::span<const std::string>(args.begin(),args.size()));
    };
    const auto player_name_begin=variables.begin()+0x844;
    const auto player_name_capacity=std::min<std::size_t>(32,variables.size()-0x844);
    const auto player_name_end=std::find(player_name_begin,
        player_name_begin+static_cast<std::ptrdiff_t>(player_name_capacity),0);
    if(player_name_end==player_name_begin+static_cast<std::ptrdiff_t>(player_name_capacity))
        throw std::runtime_error("Results player name is unterminated");
    const auto player_name_size=static_cast<std::size_t>(player_name_end-player_name_begin);
    const std::string player_name(reinterpret_cast<const char*>(&*player_name_begin),player_name_size);
    const std::string first_name=slot==0?player_name:unit_data.name;
    bool capped=false;
    // The original returns before touching any widget when a non-initial
    // update is already at level 99.
    if(!initial&&bank(1,4)==99)return true;
    (void)snapshot_game_character_at_level(variables,tables,slot,bank(1,4));
    const auto set_counter=[&](std::size_t offset,std::int16_t value) {
        put16(scratch,offset,std::uint16_t(value));
    };
    const auto advance=[&](std::size_t offset,std::int16_t limit) {
        const auto old=std::uint16_t(shalf(scratch,offset));
        auto value=std::bit_cast<std::int16_t>(std::uint16_t(old+1));
        if(value>limit)value=limit;
        set_counter(offset,value);return value;
    };
    // 0x16aac0 performs at most twelve full rows per call. The level/XP case
    // can return early after the current stat row, exactly as its source flag
    // at stack+0x290 requests.
    for(unsigned pass=0;pass<12;++pass) {
        show(0,fmt(0x96,{first_name}),-1,true);
        show(1,fmt(0x97,{value_text(hud,bank(0,0x18))}),-1,true);

        const auto current_level=bank(1,0x18);
        r.set_text(13,idbase+2,fmt(0x98,{value_text(hud,current_level)}));
        const auto next_level=advance(bank_offsets[1]+base+0x18,bank(5,0x18));
        set_alpha(r,idbase+2,next_level!=bank(0,0x18));
        set_alpha(r,int(row*6+0x16),next_level!=bank(0,0x18));

        show(3,fmt(0x99,{value_text(hud,bank(0,6)%1000)}),-1,true);

        auto xp_step=bank(1,6);
        const bool level_boundary=(int(xp_step)%1000)+(initial?0:1)>999;
        if(level_boundary) {
            const auto level_at=bank_offsets[4]+base+4;
            const auto old_level=bank(4,4);
            const auto attempted_level=std::bit_cast<std::int16_t>(std::uint16_t(old_level+1));
            (void)advance(level_at,bank(3,4));
            if(attempted_level>bank(3,4))capped=true;
            else if(!initial) {
                const int action=tables.learned_skill(int(unit),int(attempted_level));
                std::string skill_text;
                if(action<1)skill_text=tables.system_message(0x88);
                else skill_text=format_reached(tables.system_message(0x94),
                                                std::array{tables.action(action).name});
                r.set_text(1,5,skill_text);
                notice=1;notice_row=row;
                auto& counter=notice_counters.at(row);
                if(counter>=0)counter=std::bit_cast<std::int32_t>(std::uint32_t(counter)+1u);
                if(counter==-1) {
                    counter=0;notice_wait=0;
                    if(!play_jingle)throw std::runtime_error("Results skill notice requires source jingle service");
                    play_jingle(5);
                }
                const auto actor=slot==0?player_name:unit_data.name;
                const auto notice_text=format_reached(tables.system_message(0x95),
                    std::array{actor,value_text2(hud,int(attempted_level))});
                r.set_text(1,4,notice_text);
            }
            const auto slot=selected_slot(party,row);
            const auto image=snapshot_game_character_at_level(variables,tables,slot,attempted_level);
            std::copy(image.begin(),image.end(),scratch.begin()+bank_offsets[5]+base);
        }
        if(!initial) {
            xp_step=std::bit_cast<std::int16_t>(std::uint16_t(xp_step+1));
            set_counter(bank_offsets[1]+base+6,xp_step);
        }
        if(bank(2,6)<xp_step) {
            set_counter(bank_offsets[1]+base+6,bank(2,6));
            return true;
        }
        r.set_text(13,idbase+4,fmt(0x9a,{value_text(hud,int(xp_step)%1000)}));
        const bool xp_changed=bank(2,6)!=bank(0,6);
        set_alpha(r,idbase+4,xp_changed);
        set_alpha(r,int(row*6+0x17),xp_changed);
        if(level_boundary)return capped;

        const auto fixed=[&](unsigned index,unsigned msg,std::size_t source_offset,bool wide=false) {
            const auto value=wide?value_text2(hud,bank(0,source_offset)):
                                  value_text(hud,bank(0,source_offset));
            show(index,fmt(msg,{value}),-1,true);
        };
        fixed(5,0x9b,4,true);
        const auto x=bank(5,4);show(6,fmt(0x9c,{value_text2(hud,x)}),int(row*6+0x18),x!=bank(0,4));
        fixed(7,0x9d,0x1a);
        const auto y=bank(5,0x1a);r.set_text(13,idbase+8,fmt(0x9e,{value_text(hud,y)}));
        advance(bank_offsets[1]+base+0x1a,bank(5,0x1a));
        set_alpha(r,idbase+8,y!=bank(0,0x1a));set_alpha(r,int(row*6+0x19),y!=bank(0,0x1a));
        fixed(9,0x9f,0x1c);
        const auto z=bank(5,0x1c);r.set_text(13,idbase+10,fmt(0xa0,{value_text(hud,z)}));
        advance(bank_offsets[1]+base+0x1c,bank(5,0x1c));
        set_alpha(r,idbase+10,z!=bank(0,0x1c));set_alpha(r,int(row*6+0x1a),z!=bank(0,0x1c));
        fixed(11,0xa1,0x1e,true);
        const auto q=bank(5,0x1e);r.set_text(13,idbase+12,fmt(0xa2,{value_text2(hud,q)}));
        advance(bank_offsets[1]+base+0x1e,bank(5,0x1e));
        set_alpha(r,idbase+12,q!=bank(0,0x1e));set_alpha(r,int(row*6+0x1b),q!=bank(0,0x1e));
    }
    return capped;
}

void ResultsProgress::hide_changed(RuwResource& r,unsigned row) const {
    if(row>=3)throw std::runtime_error("Results hide row exceeds source storage");
    for(unsigned i:{2u,4u,6u,8u,10u,12u}) {
        set_alpha(r,int(row*13+i+0x46),false);
        set_alpha(r,int(row*6+i/2+0x15),false);
    }
}

} // namespace okage
