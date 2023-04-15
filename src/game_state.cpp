#include "okage/game_state.hpp"
#include "okage/event.hpp"
#include <algorithm>
#include <bit>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<4)throw std::runtime_error("Truncated game data word");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|(std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
template<std::size_t N>
std::vector<std::array<std::uint32_t,N>> records(Bytes b,std::string_view section,
                                               std::initializer_list<unsigned> string_columns,
                                               int owned_string_column=-1,
                                               std::vector<std::string>* owned_strings=nullptr,
                                               bool optional_section=false,
                                               std::size_t* section_start=nullptr) {
    if(word(b,0)!=0x424458||word(b,16)!=0xffffffff)throw std::runtime_error("Invalid game XDB header");
    if(owned_string_column>=0 &&
       (owned_string_column>=int(N)||!owned_strings||
        std::find(string_columns.begin(),string_columns.end(),unsigned(owned_string_column))==string_columns.end()))
        throw std::runtime_error("Invalid owned game XDB string column");
    const std::uint64_t count=word(b,12),base=32+count*16,strings=base+word(b,4);
    if(count>65536||strings>b.size()||strings+word(b,8)!=b.size())throw std::runtime_error("Game XDB section bounds");
    auto string=[&](std::uint32_t offset) {
        if(offset>=word(b,8))throw std::runtime_error("Game XDB string bounds");
        const auto first=b.begin()+strings+offset,last=std::find(first,b.end(),0);
        if(last==b.end())throw std::runtime_error("Unterminated game XDB string");
        return std::string_view(reinterpret_cast<const char*>(&*first),last-first);
    };
    for(std::size_t i=0;i<count;++i) {
        const auto at=32+i*16;
        if(string(word(b,at))!=section)continue;
        const std::uint64_t offset=word(b,at+4),words=word(b,at+8),start=base+offset;
        if((offset&3)||!words||words%N||start>strings||words*4>strings-start)
            throw std::runtime_error("Game XDB record bounds");
        if(section_start)*section_start=std::size_t(start);
        std::vector<std::array<std::uint32_t,N>> result(words/N);
        for(std::size_t row=0;row<result.size();++row) {
            for(unsigned j=0;j<N;++j)result[row][j]=word(b,start+(row*N+j)*4);
            for(const auto j:string_columns) {
                const auto value=string(result[row][j]);
                if(int(j)==owned_string_column)owned_strings->emplace_back(value);
            }
        }
        return result;
    }
    if(optional_section)return {};
    throw std::runtime_error("Missing game XDB table");
}
std::optional<GameUnitUiRecord> ui_record(Bytes b,std::size_t at) {
    if(at>b.size()||b.size()-at<108)return std::nullopt;
    const std::size_t strings=32+std::size_t(word(b,12))*16+word(b,4);
    GameUnitUiRecord result;
    for(unsigned j=0;j<27;++j)result.words[j]=word(b,at+j*4);
    for(unsigned j=0;j<2;++j) {
        const auto offset=result.words[j];
        if(offset>=word(b,8))return std::nullopt;
        const auto first=b.begin()+strings+offset,last=std::find(first,b.end(),0);
        if(last==b.end())return std::nullopt;
        auto& owned=j==0?result.name:result.model_name;
        owned.assign(reinterpret_cast<const char*>(&*first),last-first);
    }
    return result;
}
std::uint16_t half(Bytes b,std::size_t at) {return unsigned(b[at])|(unsigned(b[at+1])<<8);}
int signed_half(Bytes b,std::size_t at) {return std::bit_cast<std::int16_t>(half(b,at));}
void half(std::span<std::uint8_t> b,std::size_t at,std::uint32_t value) {b[at]=value;b[at+1]=value>>8;}
void word(std::span<std::uint8_t> b,std::size_t at,std::uint32_t value) {for(unsigned i=0;i<4;++i)b[at+i]=value>>(i*8);}
std::vector<std::uint8_t> copy_variables(Bytes variables) {
    if(variables.size()<0xd78)throw std::runtime_error("Game state variable bounds");
    return {variables.begin(),variables.end()};
}
void refresh(std::span<std::uint8_t> b,const GameDataTables& tables,unsigned slot) {
    if(slot>=8)throw std::runtime_error("Game character slot bounds");
    const auto at=0x5c4+slot*0x50;
    if(!b[at])return;
    const auto& unit=tables.unit(signed_half(b,at+2));
    const int level=signed_half(b,at+4);
    for(unsigned j=0;j<4;++j) {
        std::uint32_t value=unit[2+j*2];
        if(level>=2) {
            const auto product=unit[3+j*2]*std::uint32_t(level-1);
            value+=std::uint32_t(std::bit_cast<std::int32_t>(product)/98);
        }
        half(b,at+0x18+j*2,value+half(b,at+0xc+j*2));
    }
    for(unsigned j=0;j<17;++j)half(b,at+0x28+j*2,unit[10+j]);
    for(unsigned j=0;j<2;++j)half(b,at+0x28+j*2,half(b,at+0x28+j*2)+half(b,at+0x14+j*2));
    constexpr unsigned destinations[]={0x1a,0x1c,0x28,0x2a,0x2c,0x2e,0x34};
    for(unsigned j=0;j<3;++j) {
        const int id=signed_half(b,at+0x20+j*2);
        if(!id)continue;
        const auto& item=tables.item(id);
        if(item[2]&0x10)for(unsigned k=0;k<7;++k)
            half(b,at+destinations[k],half(b,at+destinations[k])+item[5+k]);
    }
    for(auto off:{0x18,0x1a,0x1c,0x1e,0x28,0x2a,0x2c}) {
        const int limit=off<0x1e?999:99;
        if(signed_half(b,at+off)>limit)half(b,at+off,limit);
    }
}
}
GameDataTables::GameDataTables(Bytes battle_data,Bytes item_data) {
    std::size_t unit_start=0;
    units_=records<27>(battle_data,"unit",{0,1},1,&unit_model_names_,false,&unit_start);
    items_=records<15>(item_data,"item",{0,14});
    (void)records<1>(battle_data,"sysmes",{0},0,&system_messages_,true);
    std::vector<std::string> names,descriptions;
    const auto actions=records<13>(battle_data,"waza",{0,12},0,&names,true);
    (void)records<13>(battle_data,"waza",{0,12},12,&descriptions,true);
    for(std::size_t i=0;i<actions.size();++i)
        actions_.push_back({actions[i],std::move(names[i]),std::move(descriptions[i])});
    ui_units_.reserve(units_.size());
    for(std::size_t i=0;i<units_.size();++i) {
        auto record=ui_record(battle_data,unit_start+i*108);
        if(!record)throw std::runtime_error("Invalid game UI unit record");
        ui_units_.push_back(std::move(*record));
    }
    if(unit_start>=108)preceding_ui_unit_=ui_record(battle_data,unit_start-108);
}
const std::array<std::uint32_t,27>& GameDataTables::unit(int id) const {
    if(id<1||std::size_t(id)>units_.size())throw std::runtime_error("Game unit ID bounds");
    return units_[id-1];
}
const std::string& GameDataTables::unit_model_name(int id) const {
    if(id<1||std::size_t(id)>unit_model_names_.size())throw std::runtime_error("Game unit ID bounds");
    return unit_model_names_[id-1];
}
const GameUnitUiRecord& GameDataTables::ui_unit(int id) const {
    if(id<0||std::size_t(id)>ui_units_.size())throw std::runtime_error("Game UI unit ID bounds");
    if(id==0) {
        if(!preceding_ui_unit_)throw std::runtime_error("Game UI preceding unit record unavailable");
        return *preceding_ui_unit_;
    }
    return ui_units_[id-1];
}
const std::string& GameDataTables::system_message(unsigned index) const {
    if(std::size_t(index)>=system_messages_.size())throw std::runtime_error("Game system message index bounds");
    return system_messages_[index];
}
const std::array<std::uint32_t,15>& GameDataTables::item(int id) const {
    if(id<1||std::size_t(id)>items_.size())throw std::runtime_error("Game item ID bounds");
    return items_[id-1];
}
const GameActionRecord& GameDataTables::action(int id) const {
    if(id<0||std::size_t(id)>=actions_.size())throw std::runtime_error("Game waza action bounds/owner");
    return actions_[unsigned(id)];
}
void refresh_game_character(const GameDataTables& tables,std::span<std::uint8_t> variables,unsigned slot) {
    if(slot>=8)throw std::runtime_error("Game character slot bounds");
    auto storage=copy_variables(variables);
    refresh(storage,tables,slot);
    std::copy(storage.begin(),storage.end(),variables.begin());
}
void refresh_game_characters(std::span<std::uint8_t> variables,const GameDataTables& tables,unsigned mask) {
    auto storage=copy_variables(variables);std::span<std::uint8_t> b=storage;
    std::uint32_t capacity=0,current=0,maximum=0;
    for(unsigned i=0;i<8;++i) {
        const auto at=0x5c4+i*0x50;
        if(!b[at])continue;
        refresh(b,tables,i);
        if(word(b,0x570+i*4)==3)capacity+=std::uint32_t(signed_half(b,at+0x1e));
        maximum+=std::uint32_t(signed_half(b,at+0x18));
        if(mask&2)word(b,at+8,std::uint32_t(signed_half(b,at+0x18)));
        if(mask&4)b[at]=1;
        if(mask&8)b[at+1]=0;
        current+=word(b,at+8);
    }
    if(std::bit_cast<std::int32_t>(capacity)>99)capacity=99;
    word(b,0x598,current);word(b,0x59c,maximum);word(b,0x594,capacity);
    if(mask&1)word(b,0x590,capacity);
    if(std::bit_cast<std::int32_t>(word(b,0x590))>std::bit_cast<std::int32_t>(capacity))word(b,0x590,capacity);
    std::copy(storage.begin(),storage.end(),variables.begin());
}
void grant_and_equip_game_character(std::span<std::uint8_t> variables,const GameDataTables& tables,
                                   unsigned slot,const std::array<int,3>& items) {
    auto storage=copy_variables(variables);std::span<std::uint8_t> b=storage;
    if(slot>=8)throw std::runtime_error("Game character slot bounds");
    for(const auto id:items) {
        if(id<1||id>255)throw std::runtime_error("Game inventory ID bounds");
        (void)tables.item(id);
        const auto at=0x410+id;
        b[at]=std::clamp(int(std::bit_cast<std::int8_t>(b[at]))+1,0,99);
    }
    const auto at=0x5c4+slot*0x50;
    if(b[at]) {
        for(unsigned j=0;j<3;++j)half(b,at+0x20+j*2,items[j]);
        refresh(b,tables,slot);
    }
    std::copy(storage.begin(),storage.end(),variables.begin());
}
void initialize_new_game_variables(std::span<std::uint8_t> variables,const GameDataTables& tables) {
    auto storage=copy_variables(variables);
    initialize_game_variables(storage);
    refresh_game_characters(storage,tables,15);
    grant_and_equip_game_character(storage,tables,0,{131,174,240});
    std::copy(storage.begin(),storage.end(),variables.begin());
}
}
