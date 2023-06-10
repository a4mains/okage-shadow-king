#include "okage/battle_hud.hpp"
#include "okage/battle_data.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace okage {
namespace {
constexpr std::int32_t keep=RuwResource::preserve_component;

std::string format_one(std::string_view format,std::string_view value) {
    std::string result;
    bool substituted=false;
    for(std::size_t i=0;i<format.size();) {
        if(format[i]!='%') {result.push_back(format[i++]);continue;}
        if(substituted||i+1>=format.size()||format[i+1]!='s')
            throw std::runtime_error("Unrecovered battle history string format");
        result.append(value);i+=2;substituted=true;
    }
    if(!substituted)throw std::runtime_error("Battle history format lacks its source string argument");
    return result;
}

std::string format_name_occurrence(std::string_view format,std::string_view name,char occurrence) {
    std::string result;unsigned argument=0;
    for(std::size_t i=0;i<format.size();) {
        if(format[i]!='%') {result.push_back(format[i++]);continue;}
        if(i+1>=format.size())throw std::runtime_error("Truncated battle history format token");
        if(format[i+1]=='s'&&argument==0) {result.append(name);++argument;i+=2;continue;}
        if(format[i+1]=='c'&&argument==1) {result.push_back(occurrence);++argument;i+=2;continue;}
        throw std::runtime_error("Unrecovered battle history name format");
    }
    if(argument!=2)throw std::runtime_error("Battle history name format argument count");
    return result;
}

template<std::size_t N> void bounded(std::string_view value,const char* what) {
    if(value.size()>=N)throw std::runtime_error(what);
}

struct RenderRow {
    std::string message;
    std::string value;
    std::int32_t style{};
    std::int32_t signed_value{};
    bool empty=false;
    bool group_message=false;
};

RenderRow render_row(const std::array<std::int32_t,15>& slots,unsigned row,
                     const GameDataTables& tables,
                     const BattleData& battle_data,unsigned formation_index,
                     std::optional<std::string_view> special_name) {
    const auto base=row*3;
    const auto code=std::bit_cast<std::uint32_t>(slots[base]);
    const auto style=slots[base+1];
    const auto signed_value=slots[base+2];
    if(code==0xffffffffu)return {{},{},style,signed_value,true};
    if(code>=0xfffffffcu)throw std::runtime_error("Battle history special-row kind is outside event 0/5 scope");

    std::string name;
    bool group_message=false;
    char occurrence=' ';
    if(code<0xaa) {
        if(!battle_data.has_formation(int(formation_index)))
            throw std::runtime_error("Battle history group lookup requires an authored formation");
        const auto entries=battle_data.formation(int(formation_index));
        if(entries.empty())throw std::runtime_error("Battle history formation has no lookup entries");
        const auto group=std::int32_t(code>>4);
        const auto entry_index=std::uint32_t(code&0xf);
        std::optional<std::size_t> selected;
        for(std::size_t i=0;i<entries.size();++i) {
            if(std::int32_t(entries[i].field_2)==group) {selected=i;break;}
        }
        if(!selected)throw std::runtime_error("Battle history encoded group is absent from selected formation");
        const auto unit_id=entries[*selected].unit_id;
        name=tables.ui_unit(unit_id).name;
        const auto duplicate_count=std::count_if(entries.begin(),entries.end(),[&](const auto& entry) {
            return entry.unit_id==unit_id;
        });
        if(duplicate_count!=1) {
            std::size_t rank=0;
            for(std::size_t i=0;i<std::min<std::size_t>(entry_index,entries.size());++i)
                if(entries[i].unit_id==unit_id)++rank;
            if(rank>('z'-'A'))throw std::runtime_error("Battle history duplicate label exceeds source character range");
            occurrence=char('A'+rank);
        }
        group_message=true;
    } else if(code==0xaa) {
        // The original resolves the unit record first, then substitutes the
        // type-2 live string for its display template at exactly ID 0xaa.
        (void)tables.ui_unit(int(code));
        if(!special_name)throw std::runtime_error("Battle history code 0xaa requires its live type-2 name owner");
        name.assign(*special_name);
    } else {
        if(code>std::uint32_t(std::numeric_limits<int>::max()))
            throw std::runtime_error("Battle history unit ID is outside source range");
        name=tables.ui_unit(int(code)).name;
    }

    std::string message;
    if(group_message) {
        message=format_name_occurrence(tables.system_message(0xa9),name,occurrence);
    } else {
        // The original 0x2480f0 template is exactly "%s" before sysmes[0xa7].
        const auto named=format_one("%s",name);
        message=format_one(tables.system_message(0xa7),named);
    }

    bounded<128>(message,"Battle history message exceeds RUW text owner");
    return {std::move(message),{},style,signed_value,false,group_message};
}

std::string format_display_value(std::int32_t value,const BattleHudData& hud_data,
                          const GameDataTables& tables,BattleHud& hud) {
    if(value==0)return tables.system_message(0xaa);
    if(value==std::numeric_limits<std::int32_t>::min())
        throw std::runtime_error("Battle history absolute value overflows source signed arithmetic");
    const auto magnitude=value<0?-value:value;
    const auto number=hud_data.number(magnitude);
    if(number.size()>=hud.type3_current_number.size())
        throw std::runtime_error("Battle history numeric scratch extent");
    std::copy(number.begin(),number.end(),hud.type3_current_number.begin());
    hud.type3_current_number[number.size()]=0;
    auto result=format_one(tables.system_message(0xab),number);
    bounded<128>(result,"Battle history value exceeds RUW text owner");
    return result;
}

void require_text_widget(const RuwResource& r,int id) {
    const auto& widget=r.widget(5,id);
    if(!widget.runtime||!widget.runtime->text)
        throw std::runtime_error("Battle history RUW text/runtime owner is absent");
}
void require_runtime_widget(const RuwResource& r,int id) {
    if(!r.widget(5,id).runtime)
        throw std::runtime_error("Battle history RUW runtime owner is absent");
}
}

void BattleHud::append_history_event(RuwResource& r,const BattleHudData& hud_data,
        const GameDataTables& tables,const BattleData& battle_data,
        unsigned formation_index,unsigned kind,std::uint32_t code,
        std::int32_t display_value,bool display_value_known,
        std::optional<std::string_view> special_name) {
    if(kind!=0&&kind!=5)throw std::runtime_error("Battle history event kind is outside reached 0/5 scope");
    if(!display_value_known)
        throw std::runtime_error("Battle history display word is unknown; refusing to normalize source stack residue");
    if(type3_history_cursor>4)throw std::runtime_error("Battle history cursor outside five-row source extent");
    if(code>=0xfffffffcu)throw std::runtime_error("Battle history event code is a source-reserved sentinel");

    const auto cursor=type3_history_cursor;
    type3_ready=1;
    type3_slots[cursor*3]=std::bit_cast<std::int32_t>(code);
    type3_slots[cursor*3+1]=kind==0?0x16:0x56; // DAT_00248090 + kind*4.
    type3_slots[cursor*3+2]=display_value;

    for(unsigned row=0;row<5;++row) {
        const auto id=row+0x27;
        const auto number_id=row+0x22;
        const auto message_id=row+0x1d;
        const auto current=render_row(type3_slots,row,tables,battle_data,formation_index,special_name);
        if(current.empty) {
            require_runtime_widget(r,int(number_id));
            require_runtime_widget(r,int(message_id));
            r.set_rgba(5,int(number_id),{keep,keep,keep,0});
            r.set_rgba(5,int(message_id),{keep,keep,keep,0});
            continue;
        }
        require_runtime_widget(r,int(id));
        require_runtime_widget(r,int(number_id));
        require_text_widget(r,int(message_id));
        require_text_widget(r,int(id));
        const auto signed_value=current.signed_value;
        if(signed_value<0)r.set_rgba(5,int(id),{0x80,0x20,0x20,keep});
        else r.set_rgba(5,int(id),{0x20,0x80,0x20,keep});
        if(current.style>0x52&&current.style<0x55)
            r.set_rgba(5,int(id),{0xff,0xff,0xff,keep});
        const auto value_text=format_display_value(signed_value,hud_data,tables,*this);
        r.set_position(5,int(message_id),0x120,int(row*0x11+0x15b));
        r.set_position(5,int(number_id),0x1d8,int(row*0x11+0x158));
        r.set_position(5,int(id),keep,int(row*0x11+0x15c));
        r.set_text(5,int(message_id),current.message);
        r.set_text(5,int(id),value_text);
        r.set_rgba(5,int(message_id),current.group_message?
            std::array<std::int32_t,4>{0xdc,0x3c,0x96,0x7f}:
            std::array<std::int32_t,4>{0xf0,0xf0,0xf5,0x7f});
        r.set_rgba(5,int(number_id),{0xff,0xff,0xff,0x7f});
        if(current.style==0x56)r.set_rgba(5,int(number_id),{10,10,10,0x7f});
        r.set_runtime_word0(5,int(number_id),std::uint32_t(current.style));
    }
    if(cursor<4)type3_history_cursor=cursor+1;
    else {
        for(unsigned row=0;row<4;++row) {
            type3_slots[row*3]=type3_slots[(row+1)*3];
            type3_slots[row*3+1]=type3_slots[(row+1)*3+1];
            type3_slots[row*3+2]=type3_slots[(row+1)*3+2];
        }
        type3_history_cursor=4;
    }
    type3_ready=1;
}
}
