#include "okage/game_state.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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

std::vector<std::uint8_t> make_xdb(
    const std::string& section,const std::vector<std::vector<std::uint32_t>>& rows,
    const std::vector<unsigned>& string_columns) {
    require(!rows.empty());
    const auto stride=rows.front().size();
    require(stride!=0 && std::all_of(rows.begin(),rows.end(),[&](const auto& row){return row.size()==stride;}));
    std::vector<std::uint8_t> strings;
    auto append_string=[&](const std::string& value) {
        const auto offset=std::uint32_t(strings.size());
        strings.insert(strings.end(),value.begin(),value.end());
        strings.push_back(0);
        return offset;
    };
    const auto section_name=append_string(section);
    require(section_name==0);
    auto data=rows;
    for(std::size_t i=0;i<data.size();++i) for(auto col:string_columns) {
        require(col<stride);
        data[i][col]=append_string(section+"-row-"+std::to_string(i+1)+"-word-"+std::to_string(col));
    }
    const std::size_t record_bytes=data.size()*stride*4;
    const std::size_t data_base=0x30;
    std::vector<std::uint8_t> out(data_base+record_bytes+strings.size());
    put32(out,0,0x00424458); // "XDB\0"
    put32(out,4,std::uint32_t(record_bytes));
    put32(out,8,std::uint32_t(strings.size()));
    put32(out,12,1);
    put32(out,16,0xffffffff);
    put32(out,0x20,0); // section key offset in strings
    put32(out,0x24,0); // section data offset
    put32(out,0x28,std::uint32_t(data.size()*stride)); // section size in words
    put32(out,0x2c,0);
    for(std::size_t i=0;i<data.size();++i)
        for(std::size_t j=0;j<stride;++j)
            put32(out,data_base+(i*stride+j)*4,data[i][j]);
    std::copy(strings.begin(),strings.end(),out.begin()+data_base+record_bytes);
    return out;
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
okage::GameDataTables tables(std::vector<std::vector<std::uint32_t>> units,
                             std::vector<std::vector<std::uint32_t>> items) {
    auto b=make_xdb("unit",units,{0,1});
    auto i=make_xdb("item",items,{0,14});
    return okage::GameDataTables(b,i);
}
}

int main() {
    try {
        {
            auto units=make_units(2); units[0][2]=321;
            auto items=make_items(3); items[1][2]=0x10;
            auto b=make_xdb("unit",units,{0,1});
            auto i=make_xdb("item",items,{0,14});
            okage::GameDataTables parsed(b,i);
            require(parsed.unit(1)[2]==321 && parsed.item(2)[2]==0x10);
            rejects([&]{(void)parsed.unit(0);});
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
            malformed=b; malformed.resize(19);
            rejects([&]{okage::GameDataTables bad(malformed,i);});
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
        std::cout<<"Game XDB parsing, signed growth, halfword math, aggregates, equipment and transactional failures passed\n";
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
    return 0;
}
