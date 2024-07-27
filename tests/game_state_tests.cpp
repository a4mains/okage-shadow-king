#include "okage/game_state.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void require(bool value) {
    if(!value) throw std::runtime_error("Game state test assertion failed");
}
template<class F> void rejects(F&& f) {
    bool failed=false;
    try { f(); } catch(const std::runtime_error&) { failed=true; }
    require(failed);
}
void put32(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) b.at(at+i)=std::uint8_t(value>>(i*8));
}
void put16(std::vector<std::uint8_t>& b,std::size_t at,std::uint16_t value) {
    b.at(at)=std::uint8_t(value); b.at(at+1)=std::uint8_t(value>>8);
}
std::uint16_t get16(const std::vector<std::uint8_t>& b,std::size_t at) {
    return std::uint16_t(b.at(at)) | (std::uint16_t(b.at(at+1))<<8);
}
std::uint32_t get32(const std::vector<std::uint8_t>& b,std::size_t at) {
    return std::uint32_t(b.at(at)) | (std::uint32_t(b.at(at+1))<<8) |
           (std::uint32_t(b.at(at+2))<<16) | (std::uint32_t(b.at(at+3))<<24);
}

struct SectionFixture {
    std::string name;
    std::vector<std::vector<std::uint32_t>> rows;
    std::vector<unsigned> string_columns;
};
std::vector<std::uint8_t> make_xdb_multi(std::vector<SectionFixture> sections) {
    require(!sections.empty());
    std::vector<std::uint8_t> strings;
    auto append_string=[&](const std::string& value) {
        const auto offset=std::uint32_t(strings.size());
        strings.insert(strings.end(),value.begin(),value.end());
        strings.push_back(0);
        return offset;
    };
    std::vector<std::uint32_t> section_name_offsets;
    for(const auto& section:sections) section_name_offsets.push_back(append_string(section.name));
    struct BuiltSection { std::vector<std::vector<std::uint32_t>> rows; std::uint32_t offset; std::uint32_t words; };
    std::vector<BuiltSection> built;
    std::uint32_t record_bytes=0;
    for(auto& section:sections) {
        require(!section.rows.empty());
        const auto stride=section.rows.front().size();
        require(stride!=0 && std::all_of(section.rows.begin(),section.rows.end(),[&](const auto& row){return row.size()==stride;}));
        auto rows=section.rows;
        for(std::size_t i=0;i<rows.size();++i) for(auto col:section.string_columns) {
            require(col<stride);
            rows[i][col]=append_string(section.name+"-row-"+std::to_string(i+1)+"-word-"+std::to_string(col));
        }
        built.push_back({std::move(rows),record_bytes,std::uint32_t(section.rows.size()*stride)});
        record_bytes+=std::uint32_t(section.rows.size()*stride*4);
    }
    const std::size_t data_base=32+sections.size()*16;
    std::vector<std::uint8_t> out(data_base+record_bytes+strings.size());
    put32(out,0,0x00424458); // "XDB\0"
    put32(out,4,record_bytes);
    put32(out,8,std::uint32_t(strings.size()));
    put32(out,12,std::uint32_t(sections.size()));
    put32(out,16,0xffffffff);
    std::size_t row_at=0;
    for(std::size_t i=0;i<built.size();++i) {
        const auto dir=32+i*16;
        put32(out,dir,section_name_offsets[i]);
        put32(out,dir+4,built[i].offset);
        put32(out,dir+8,built[i].words);
        put32(out,dir+12,0);
        for(const auto& row:built[i].rows) for(const auto value:row) {
            put32(out,data_base+row_at,value);
            row_at+=4;
        }
    }
    std::copy(strings.begin(),strings.end(),out.begin()+data_base+record_bytes);
    return out;
}
std::vector<std::uint8_t> make_xdb(
    const std::string& section,const std::vector<std::vector<std::uint32_t>>& rows,
    const std::vector<unsigned>& string_columns) {
    return make_xdb_multi({SectionFixture{section,rows,string_columns}});
}
std::vector<std::vector<std::uint32_t>> make_units(std::size_t count) {
    std::vector<std::vector<std::uint32_t>> rows(count,std::vector<std::uint32_t>(27));
    for(auto& r:rows) {
        r[2]=100; r[4]=50; r[6]=40; r[8]=30;
        for(unsigned i=10;i<27;++i) r[i]=i;
    }
    return rows;
}
std::vector<std::vector<std::uint32_t>> make_items(std::size_t count) {
    std::vector<std::vector<std::uint32_t>> rows(count,std::vector<std::uint32_t>(15));
    return rows;
}
std::vector<std::vector<std::uint32_t>> make_messages(std::size_t count) {
    return std::vector<std::vector<std::uint32_t>>(count,std::vector<std::uint32_t>(1));
}
okage::GameDataTables tables(std::vector<std::vector<std::uint32_t>> units,
                             std::vector<std::vector<std::uint32_t>> items) {
    auto b=make_xdb("unit",units,{0,1});
    auto i=make_xdb("item",items,{0,14});
    return okage::GameDataTables(b,i);
}
void refresh_one_record() {
    auto units=make_units(1);units[0][3]=196;
    auto items=make_items(1);items[0][2]=0x10;items[0][5]=7;
    const auto data=tables(units,items);
    std::vector<std::uint8_t> vars(0xda0,0xa5);
    const unsigned slot=3,at=0x5c4+slot*0x50;
    std::fill_n(vars.begin()+at,0x50,0);
    vars[at]=1;vars[at+1]=4;put16(vars,at+2,1);put16(vars,at+4,2);
    put32(vars,at+8,0x10000);
    put16(vars,at+0xc,5);put16(vars,at+0x20,1);
    const auto before=vars;
    okage::refresh_game_character(data,vars,slot);
    require(get16(vars,at+0x18)==107); // 100 + 196/98 + bonus5.
    require(get16(vars,at+0x1a)==57); // equipment adds7 to attack50.
    require(get32(vars,at+8)==0x10000&&vars[at+1]==4);
    require(std::equal(vars.begin(),vars.begin()+at,before.begin()));
    require(std::equal(vars.begin()+at+0x50,vars.end(),before.begin()+at+0x50));
    require(get32(vars,0x590)==0xa5a5a5a5&&get32(vars,0x598)==0xa5a5a5a5);
    auto invalid=before;put16(invalid,at+0x24,2);const auto failed=invalid;
    rejects([&]{okage::refresh_game_character(data,invalid,slot);});require(invalid==failed);
    invalid=before;invalid[at]=0;const auto inactive=invalid;
    okage::refresh_game_character(data,invalid,slot);require(invalid==inactive);
    for(auto selected:{8u,0xffffffffu}) {
        rejects([&]{okage::refresh_game_character(data,invalid,selected);});require(invalid==inactive);
    }
    auto short_backing=std::vector<std::uint8_t>(0xd77,0xa5);const auto short_before=short_backing;
    rejects([&]{okage::refresh_game_character(data,short_backing,0);});require(short_backing==short_before);
    // Last supported record exercises full owned bounds and preserves HP0.
    vars.assign(0xda0,0);const unsigned last=0x5c4+7*0x50;
    vars[last]=1;put16(vars,last+2,1);put16(vars,last+4,1);
    okage::refresh_game_character(data,vars,7);
    require(get16(vars,last+0x18)==100&&get32(vars,last+8)==0);
}
void owned_item_names() {
    auto units=make_xdb("unit",make_units(1),{0,1});
    auto items=make_xdb("item",make_items(2),{0,14});
    okage::GameDataTables parsed(units,items);
    const auto first_record=parsed.item(1);
    require(parsed.item_name(1)=="item-row-1-word-0");
    require(parsed.item_name(2)=="item-row-2-word-0");
    for(int id:{0,-1,3,2147483647})rejects([&]{(void)parsed.item_name(id);});

    auto malformed=items;
    put32(malformed,48,0xffffffffu); // First item's name offset is invalid.
    rejects([&]{okage::GameDataTables bad(units,malformed);});
    malformed=items;
    put32(malformed,48,get32(malformed,8)-1);
    malformed.back()='X';
    rejects([&]{okage::GameDataTables bad(units,malformed);});

    std::fill(units.begin(),units.end(),0);
    std::fill(items.begin(),items.end(),0);
    require(parsed.item(1)==first_record);
    require(parsed.item_name(1)=="item-row-1-word-0");
    require(parsed.item_name(2)=="item-row-2-word-0");
    const auto after_buffers_die=[] {
        const auto owned_units=make_xdb("unit",make_units(1),{0,1});
        const auto owned_items=make_xdb("item",make_items(2),{0,14});
        return okage::GameDataTables(owned_units,owned_items);
    }();
    require(after_buffers_die.item_name(1)=="item-row-1-word-0");
    require(after_buffers_die.item_name(2)=="item-row-2-word-0");
}
void learned_skill_table() {
    auto items=make_xdb("item",make_items(1),{0,14});
    auto battle=make_xdb_multi({
        {"unit",make_units(1),{0,1}},
        {"learn",{{11,0,0x80000001u,14,15,16},{21,22,23,24,25,26}}, {}}
    });
    const okage::GameDataTables parsed(battle,items);
    require(parsed.learned_skill(0xaa,0)==11);
    require(parsed.learned_skill(0xab,0)==-1);
    require(parsed.learned_skill(0xac,0)==-1); // signed negative table word
    require(parsed.learned_skill(0xaf,0)==16);
    require(parsed.learned_skill(0xad,1)==24);
    require(parsed.learned_skill(0xa9,0)==-1 && parsed.learned_skill(0xb0,0)==-1);
    rejects([&]{(void)parsed.learned_skill(0xaa,-1);});
    rejects([&]{(void)parsed.learned_skill(0xaa,2);});
    rejects([&]{(void)parsed.learned_skill(0xa9,2);}); // row read precedes unit-ID rejection

    const auto absent=okage::GameDataTables(make_xdb("unit",make_units(1),{0,1}),items);
    rejects([&]{(void)absent.learned_skill(0xaa,0);});
    auto malformed=battle;
    put32(malformed,32+16+8,11);
    rejects([&]{okage::GameDataTables bad(malformed,items);});
}
void snapshot_at_temporary_level() {
    auto units=make_units(1);
    units[0][2]=100; units[0][3]=196;
    units[0][4]=50; units[0][5]=98;
    auto items=make_items(1);
    items[0][2]=0x10; items[0][5]=7;
    const auto data=tables(units,items);
    std::vector<std::uint8_t> vars(0xd78,0xa5);
    constexpr unsigned slot=2;
    const std::size_t at=0x5c4+slot*0x50;
    std::fill_n(vars.begin()+at,0x50,0);
    vars[at]=1; vars[at+1]=4;
    put16(vars,at+2,1); put16(vars,at+4,1);
    put32(vars,at+8,0x12345678); // Snapshot/refresh must not heal HP.
    put16(vars,at+0xc,5);
    put16(vars,at+0x14,2);
    put16(vars,at+0x20,1); // Authored equipment adds seven to attack.
    vars[at+0x50]=0x3c; vars[at+0x51]=0xd2; // Next record's first two bytes.
    const auto before=vars;

    const auto snapshot=okage::snapshot_game_character_at_level(vars,data,slot,3);
    require(get16(std::vector<std::uint8_t>(snapshot.begin(),snapshot.end()),4)==3);
    require(get16(std::vector<std::uint8_t>(snapshot.begin(),snapshot.end()),0x18)==109);
    require(get16(std::vector<std::uint8_t>(snapshot.begin(),snapshot.end()),0x1a)==59);
    require(snapshot[0x50]==0x3c&&snapshot[0x51]==0xd2);

    // The owner's level is restored and its last derived refresh is at level 1.
    require(get16(vars,at+4)==1);
    require(get16(vars,at+0x18)==105);
    require(get16(vars,at+0x1a)==57);
    require(get32(vars,at+8)==0x12345678);
    require(get16(vars,at+0x20)==1);
    require(std::equal(vars.begin(),vars.begin()+at,before.begin()));
    require(std::equal(vars.begin()+at+0x50,vars.end(),before.begin()+at+0x50));

    for(unsigned bad_slot:{8u,0xffffffffu}) {
        const auto unchanged=vars;
        rejects([&]{(void)okage::snapshot_game_character_at_level(vars,data,bad_slot,3);});
        require(vars==unchanged);
    }
    const auto unchanged=vars;
    auto too_short=std::span<std::uint8_t>(vars).first(at+0x51);
    rejects([&]{(void)okage::snapshot_game_character_at_level(too_short,data,slot,3);});
    require(vars==unchanged);
    auto invalid=vars;
    put16(invalid,at+2,2);
    const auto invalid_before=invalid;
    rejects([&]{(void)okage::snapshot_game_character_at_level(invalid,data,slot,3);});
    require(invalid==invalid_before);

    constexpr unsigned last_slot=7;
    constexpr std::size_t last_at=0x5c4+last_slot*0x50;
    std::fill_n(vars.begin()+last_at,0x50,0);
    vars[last_at]=1; put16(vars,last_at+2,1); put16(vars,last_at+4,1);
    put32(vars,last_at+8,0x87654321);
    vars[last_at+0x50]=0x6e; vars[last_at+0x51]=0xa9;
    const auto last_snapshot=okage::snapshot_game_character_at_level(vars,data,last_slot,2);
    require(last_snapshot.size()==0x52);
    require(last_snapshot[0x50]==0x6e&&last_snapshot[0x51]==0xa9);
    require(get32(vars,last_at+8)==0x87654321&&get16(vars,last_at+4)==1);
    auto last_short=std::span<std::uint8_t>(vars).first(last_at+0x51);
    rejects([&]{(void)okage::snapshot_game_character_at_level(last_short,data,last_slot,2);});
}
void ui_records() {
    auto item=make_xdb("item",make_items(1),{0,14});
    auto prefix=make_units(1);prefix[0][2]=0xaabbccddu;prefix[0][26]=0xffffffffu;
    auto battle=make_xdb_multi({{"prefix",prefix,{0,1}},{"unit",make_units(2),{0,1}}});
    auto expected=std::array<std::uint32_t,27>{};
    for(unsigned j=0;j<27;++j)expected[j]=get32(battle,64+4*j);
    okage::GameDataTables parsed(battle,item);
    require(parsed.ui_unit(0).words==expected);
    require(parsed.ui_unit(0).name=="prefix-row-1-word-0"&&parsed.ui_unit(0).model_name=="prefix-row-1-word-1");
    for(int id:{1,2}) {
        require(parsed.ui_unit(id).words==parsed.unit(id));
        require(parsed.ui_unit(id).name=="unit-row-"+std::to_string(id)+"-word-0");
        require(parsed.ui_unit(id).model_name==parsed.unit_model_name(id));
    }
    for(int id:{-1,-2147483647-1,3,2147483647})rejects([&]{(void)parsed.ui_unit(id);});
    rejects([&]{(void)parsed.unit(0);});rejects([&]{(void)parsed.unit_model_name(0);});
    auto malformed=battle;put32(malformed,64,0xffffffffu);
    okage::GameDataTables invalid_name(malformed,item);
    rejects([&]{(void)invalid_name.ui_unit(0);});require(invalid_name.ui_unit(2).words==parsed.unit(2));
    malformed=battle;put32(malformed,68,get32(malformed,8)); // First invalid string offset.
    okage::GameDataTables invalid_model(malformed,item);
    rejects([&]{(void)invalid_model.ui_unit(0);});require(invalid_model.item(1)==parsed.item(1));
    malformed=battle;put32(malformed,68,get32(malformed,8));
    put32(malformed,8,get32(malformed,8)+1);malformed.push_back('X');
    okage::GameDataTables unterminated(malformed,item);
    rejects([&]{(void)unterminated.ui_unit(0);});require(unterminated.unit_model_name(1)==parsed.unit_model_name(1));
    const auto minimal=tables(make_units(1),make_items(1));
    rejects([&]{(void)minimal.ui_unit(0);});require(minimal.ui_unit(1).name=="unit-row-1-word-0");
    // A real predecessor can consist of directory entries rather than records.
    auto directory=make_xdb_multi({{"unit",make_units(1),{0,1}},
        {"a",make_messages(1),{}},{"b",make_messages(1),{}},{"c",make_messages(1),{}},
        {"d",make_messages(1),{}},{"e",make_messages(1),{}},{"f",make_messages(1),{}}});
    okage::GameDataTables overlapping(directory,item);
    for(unsigned j=0;j<27;++j)require(overlapping.ui_unit(0).words[j]==get32(directory,36+j*4));
    require(overlapping.ui_unit(0).name=="unit");
    std::fill(battle.begin(),battle.end(),0);std::fill(item.begin(),item.end(),0);
    require(parsed.ui_unit(0).words==expected&&parsed.ui_unit(0).name=="prefix-row-1-word-0");
    require(parsed.ui_unit(2).name=="unit-row-2-word-0");
    const auto after_inputs_die=[] {
        const auto b=make_xdb_multi({{"prefix",make_units(1),{0,1}},{"unit",make_units(1),{0,1}}});
        const auto i=make_xdb("item",make_items(1),{0,14});return okage::GameDataTables(b,i);
    }();
    require(after_inputs_die.ui_unit(0).model_name=="prefix-row-1-word-1");
    require(after_inputs_die.ui_unit(1).name=="unit-row-1-word-0");
}
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Missing owned game table");
    return {(std::istreambuf_iterator<char>(in)),{}};
}
void diagnostic(const char* battle_path,const char* item_path) {
    auto b=read(battle_path),i=read(item_path);okage::GameDataTables tables(b,i);
    const std::array<std::uint32_t,27> preceding={0,6885,0,3502,27540,3328,0,12738,40852,
        600,0,12744,43252,336,0,12750,44596,24,0,13940,44692,263,0,19494,45744,222,0};
    require(tables.ui_unit(0).words==preceding&&tables.ui_unit(0).name=="unit");
    require(tables.ui_unit(0).model_name==" Group  Poisons Target.");
    require(tables.ui_unit(170).name=="Ari"&&tables.ui_unit(170).model_name=="lucan");
    require(tables.ui_unit(170).words==tables.unit(170)&&tables.ui_unit(170).words[2]==32);
    require(tables.ui_unit(170).words[0]==2918&&tables.ui_unit(170).words[1]==2922);
    for(unsigned j=0;j<27;++j)require(get32(b,36+4*j)==tables.ui_unit(0).words[j]);
    rejects([&]{(void)tables.unit(0);});rejects([&]{(void)tables.unit_model_name(0);});
    rejects([&]{(void)tables.ui_unit(-1);});rejects([&]{(void)tables.ui_unit(256);});
    std::fill(b.begin(),b.end(),0);std::fill(i.begin(),i.end(),0);
    require(tables.ui_unit(0).words==preceding&&tables.ui_unit(170).name=="Ari");
    std::cout<<"Owned BKDATA UI predecessor/display-name diagnostic passed\n";
}
}

int main(int argc,char** argv) {
    try {
        if(argc==4&&std::string_view(argv[1])=="--diagnostic") {
            diagnostic(argv[2],argv[3]);return 0;
        }
        require(argc==1);ui_records();refresh_one_record();owned_item_names();learned_skill_table();snapshot_at_temporary_level();
        {
            auto b=make_xdb_multi({
                SectionFixture{"unit",make_units(1),{0,1}},
                SectionFixture{"sysmes",make_messages(167),{0}}
            });
            auto i=make_xdb("item",make_items(1),{0,14});
            okage::GameDataTables parsed(b,i);
            require(parsed.system_message(0)=="sysmes-row-1-word-0");
            require(parsed.system_message(0xa6)=="sysmes-row-167-word-0");
            rejects([&]{(void)parsed.system_message(167);});

            // System messages own their string bytes, independent of BKDATA.
            std::fill(b.begin(),b.end(),0);
            require(parsed.system_message(0)=="sysmes-row-1-word-0");
            require(parsed.system_message(0xa6)=="sysmes-row-167-word-0");
            const auto after_buffers_die=[&] {
                const auto owned_battle=make_xdb_multi({
                    SectionFixture{"unit",make_units(1),{0,1}},
                    SectionFixture{"sysmes",make_messages(167),{0}}
                });
                const auto owned_items=make_xdb("item",make_items(1),{0,14});
                return okage::GameDataTables(owned_battle,owned_items);
            }();
            require(after_buffers_die.system_message(0xa6)=="sysmes-row-167-word-0");

            auto missing=tables(make_units(1),make_items(1));
            rejects([&]{(void)missing.system_message(0);});

            auto malformed=make_xdb_multi({
                SectionFixture{"unit",make_units(1),{0,1}},
                SectionFixture{"sysmes",make_messages(2),{0}}
            });
            // BKDATA data starts after a two-entry directory; unit is 27 words.
            put32(malformed,32+2*16+27*4,0xffffffffu);
            rejects([&]{okage::GameDataTables bad(malformed,i);});

            malformed=make_xdb_multi({
                SectionFixture{"unit",make_units(1),{0,1}},
                SectionFixture{"sysmes",make_messages(2),{0}}
            });
            malformed.back()='X';
            rejects([&]{okage::GameDataTables bad(malformed,i);});
        }
        {
            auto units=make_units(2); units[0][2]=321;
            auto items=make_items(3); items[1][2]=0x10;
            auto b=make_xdb("unit",units,{0,1});
            auto i=make_xdb("item",items,{0,14});
            okage::GameDataTables parsed(b,i);
            require(parsed.unit(1)[2]==321 && parsed.item(2)[2]==0x10);
            require(parsed.unit_model_name(1)=="unit-row-1-word-1");
            require(parsed.unit_model_name(2)=="unit-row-2-word-1");
            rejects([&]{(void)parsed.unit(0);});
            rejects([&]{(void)parsed.unit_model_name(0);});
            rejects([&]{(void)parsed.unit_model_name(3);});
            rejects([&]{(void)parsed.item(4);});

            auto malformed=b;
            put32(malformed,0x24,0xfffffffcu); // section data offset outside payload
            rejects([&]{okage::GameDataTables bad(malformed,i);});
            malformed=b;
            put32(malformed,0x30,0xffffffffu); // first resolved unit string offset
            rejects([&]{okage::GameDataTables bad(malformed,i);});
            malformed=i;
            malformed.back()='X';
            rejects([&]{okage::GameDataTables bad(b,malformed);});
            malformed=b;
            put32(malformed,0x34,0xffffffffu); // first unit model-name string offset
            rejects([&]{okage::GameDataTables bad(malformed,i);});
            malformed=b;
            malformed.back()='X';
            rejects([&]{okage::GameDataTables bad(malformed,i);});
            malformed=b; malformed.resize(19);
            rejects([&]{okage::GameDataTables bad(malformed,i);});

            std::fill(b.begin(),b.end(),0);
            std::fill(i.begin(),i.end(),0);
            require(parsed.unit_model_name(1)=="unit-row-1-word-1");
            require(parsed.unit_model_name(2)=="unit-row-2-word-1");

            const auto after_inputs_die=[&] {
                const auto source_units=make_xdb("unit",units,{0,1});
                const auto source_items=make_xdb("item",items,{0,14});
                return okage::GameDataTables(source_units,source_items);
            }();
            require(after_inputs_die.unit_model_name(1)=="unit-row-1-word-1");
            require(after_inputs_die.unit_model_name(2)=="unit-row-2-word-1");
        }
        {
            auto units=make_units(2);
            units[0][2]=100; units[0][3]=0x60000000u;
            units[0][10]=0xffff; units[0][11]=11; units[0][12]=12; units[0][13]=13; units[0][16]=16;
            units[1][2]=995; units[1][8]=150;
            auto items=make_items(3);
            items[0][2]=0x10;
            items[0][5]=1; items[0][6]=2; items[0][7]=0xffff;
            items[0][8]=4; items[0][9]=5; items[0][10]=6; items[0][11]=7;
            const auto data=tables(units,items);
            std::vector<std::uint8_t> vars(0xd78);
            const std::size_t c0=0x5c4,c1=c0+0x50;
            vars[c0]=1; put16(vars,c0+2,1); put16(vars,c0+4,5);
            put16(vars,c0+8,25); put16(vars,c0+0xc,24074);
            put16(vars,c0+0x14,2); put16(vars,c0+0x16,3); put16(vars,c0+0x20,1);
            put16(vars,c0+0x22,2); put16(vars,c0+0x24,3);
            put32(vars,0x570,3); put32(vars,0x590,80);
            okage::refresh_game_characters(vars,data,0);
            // 0x60000000 * (5-1) wraps to signed 0x80000000; signed /98
            // yields -21913098, then the 16-bit record write keeps 100.
            require(get16(vars,c0+0x18)==100);
            require(get16(vars,c0+0x1a)==51 && get16(vars,c0+0x1c)==42);
            require(get16(vars,c0+0x28)==0); // 0xffff + 2 + 0xffff wraps to 0
            require(get16(vars,c0+0x2a)==18 && get16(vars,c0+0x2c)==17);
            require(get16(vars,c0+0x2e)==19 && get16(vars,c0+0x34)==23);
            require(get32(vars,0x598)==25 && get32(vars,0x59c)==100);
            require(get32(vars,0x594)==30 && get32(vars,0x590)==30);

            vars[c1]=1; put16(vars,c1+2,2); put16(vars,c1+4,1);
            put16(vars,c1+8,10); put16(vars,c1+0xc,10);
            put32(vars,0x574,3);
            okage::refresh_game_characters(vars,data,6);
            require(get16(vars,c1+0x18)==999); // 995 + 10 is capped at 999
            require(get16(vars,c1+8)==999 && get16(vars,c0+8)==100);
            require(get32(vars,0x598)==1099 && get32(vars,0x59c)==1099);
            require(get32(vars,0x594)==99); // per-record 99 cap and aggregate 99 cap
            vars[c0+1]=0x7f; vars[c1+1]=0x55;
            okage::refresh_game_characters(vars,data,8);
            require(vars[c0+1]==0 && vars[c1+1]==0);
            vars[c0]=2;vars[c0+1]=0x7f;vars[c1+1]=0x55;
            put32(vars,c0+8,1);put32(vars,c1+8,0);put32(vars,0x590,0);
            const auto empty=std::vector<std::uint8_t>(vars.begin()+c1+0x50,vars.begin()+c1+0xa0);
            okage::refresh_game_characters(vars,data,15);
            require(get32(vars,c0+8)==100&&get32(vars,c1+8)==999);
            require(vars[c0]==1&&vars[c1]==1&&vars[c0+1]==0&&vars[c1+1]==0);
            require(get32(vars,0x590)==99&&get32(vars,0x598)==1099&&get32(vars,0x59c)==1099);
            require(std::equal(empty.begin(),empty.end(),vars.begin()+c1+0x50));
            const auto healed=vars;okage::refresh_game_characters(vars,data,15);require(vars==healed);
        }
        {
            const auto data=tables(make_units(2),make_items(3));
            std::vector<std::uint8_t> vars(0xd78);
            const std::size_t c=0x5c4;
            vars[c]=1; put16(vars,c+2,1); put16(vars,c+4,1);
            vars[0x411]=0xff; vars[0x412]=99; vars[0x413]=98;
            okage::grant_and_equip_game_character(vars,data,0,{1,2,3});
            require(vars[0x411]==0 && vars[0x412]==99 && vars[0x413]==99);
            require(get16(vars,c+0x20)==1 && get16(vars,c+0x22)==2 && get16(vars,c+0x24)==3);
            const auto before=vars;
            rejects([&]{okage::grant_and_equip_game_character(vars,data,0,{1,2,0});});
            require(vars==before);
            rejects([&]{okage::refresh_game_characters(std::span<std::uint8_t>(vars).first(0xd77),data,0);});
            require(vars==before);
        }
        {
            auto units=make_units(2); auto items=make_items(3);
            const auto data=tables(units,items);
            std::vector<std::uint8_t> vars(0xd78);
            const std::size_t c0=0x5c4,c1=c0+0x50;
            vars[c0]=1; put16(vars,c0+2,1); put16(vars,c0+4,1); put16(vars,c0+8,17);
            vars[c1]=1; put16(vars,c1+2,99); put16(vars,c1+4,1); put16(vars,c1+8,23);
            const auto before=vars;
            rejects([&]{okage::refresh_game_characters(vars,data,2);});
            require(vars==before);
        }
        {
            const auto data=tables(make_units(176),make_items(240));
            std::vector<std::uint8_t> vars(0xd78,0xa5);
            okage::initialize_new_game_variables(vars,data);
            constexpr unsigned levels[]={1,5,10,25,30,40,1};
            for(unsigned i=0;i<7;++i) {
                const auto c=0x5c4+i*0x50;
                require(vars[c]==1 && get16(vars,c+2)==170+i && get16(vars,c+4)==levels[i]);
                require(get16(vars,c+8)==100 && get16(vars,c+0x18)==100);
            }
            require(get16(vars,0x5c4+0x20)==131 && get16(vars,0x5c4+0x22)==174 &&
                    get16(vars,0x5c4+0x24)==240);
            require(vars[0x410+131]==1 && vars[0x410+174]==1 && vars[0x410+240]==1);
            require(get32(vars,0x598)==700 && get32(vars,0x59c)==700);
        }
        {
            okage::GamePlayTimeCounter cadence;
            std::vector<std::uint8_t> vars(0xd78,0xa5);
            put32(vars,0x520,41);
            for(unsigned i=0;i<59;++i)require(!cadence.main_loop_pass(vars));
            require(get32(vars,0x520)==41);
            require(cadence.main_loop_pass(vars));
            require(get32(vars,0x520)==42);
            for(unsigned i=0;i<59;++i)require(!cadence.main_loop_pass(vars));
            require(cadence.main_loop_pass(vars));
            require(get32(vars,0x520)==43);
            require(vars[0x51f]==0xa5&&vars[0x524]==0xa5);
        }
        {
            okage::GamePlayTimeCounter cap;
            std::vector<std::uint8_t> vars(0xd78);
            put32(vars,0x520,359998);
            for(unsigned i=0;i<60;++i)cap.main_loop_pass(vars);
            require(get32(vars,0x520)==359999);
            for(unsigned i=0;i<120;++i)cap.main_loop_pass(vars);
            require(get32(vars,0x520)==359999);
            put32(vars,0x520,360000);
            for(unsigned i=0;i<60;++i)cap.main_loop_pass(vars);
            require(get32(vars,0x520)==360000);
        }
        {
            okage::GamePlayTimeCounter signed_math;
            std::vector<std::uint8_t> vars(0xd78);
            put32(vars,0x520,0xffffffffu);
            for(unsigned i=0;i<60;++i)signed_math.main_loop_pass(vars);
            require(get32(vars,0x520)==0); // negative signed source count increments
            put32(vars,0x520,0x7fffffffu);
            for(unsigned i=0;i<60;++i)signed_math.main_loop_pass(vars);
            require(get32(vars,0x520)==0x80000000u); // ADDIU wrap then signed SLT
        }
        {
            okage::GamePlayTimeCounter survives_restore;
            std::vector<std::uint8_t> live(0xd78),restored(0xd78);
            put32(live,0x520,9);
            for(unsigned i=0;i<59;++i)require(!survives_restore.main_loop_pass(live));
            put32(restored,0x520,900);
            require(survives_restore.main_loop_pass(restored));
            require(get32(restored,0x520)==901);
            const auto before=restored;
            rejects([&]{survives_restore.main_loop_pass(std::span<std::uint8_t>(restored).first(0xd77));});
            require(restored==before);
        }
        std::cout<<"Game XDB parsing, signed growth, halfword math, aggregates, equipment, playtime cadence and transactional failures passed\n";
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
    return 0;
}
