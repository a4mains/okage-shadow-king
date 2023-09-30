#include "okage/results_data.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace okage;
namespace {
constexpr unsigned header=88;
constexpr unsigned data_size=1080;
void put32(std::vector<std::uint8_t>& data,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)data[at+i]=std::uint8_t(value>>(i*8));
}
void put_hex(std::vector<std::uint8_t>& data,unsigned at,std::string_view hex) {
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<hex.size()/2;++i) {
        const auto nibble=[&](char c){auto p=digits.find(c);assert(p!=digits.npos);return unsigned(p);};
        data[at+i]=std::uint8_t((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));
    }
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> data(data_size);
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','S','U','I','1',0};
    std::copy(magic.begin(),magic.end(),data.begin());
    put32(data,8,1); put32(data,12,28); put32(data,16,16); put32(data,20,49);
    put_hex(data,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    put_hex(data,56,"9307f0adb6f8183153095634ccf9ac284d2d9bc1ed320d13ea050099af7aa183");
    constexpr std::array<std::uint32_t,28> row={
        1,44,40,1,3282010112u,3265134592u,1132629209u,1065353216u,0,3217031168u,
        0,0,0,1125122048u,0,0,1408243325u,1408243325u,1400374088u,1400374088u,
        1,1,1,1,11,33,17,0
    };
    for(unsigned i=0;i<row.size();++i)put32(data,header+i*4,row[i]);
    constexpr unsigned widgets=header+112;
    constexpr std::array<std::uint32_t,16> ids={0x40,0x41,0x42,0x43,0x72,0x73,0x74,0x75,
        0x6e,0x6f,0x70,0x71,0x76,0x77,0x78,0x79};
    for(unsigned i=0;i<ids.size();++i)put32(data,widgets+i*4,ids[i]);
    constexpr unsigned animations=widgets+64;
    constexpr std::array<std::array<std::uint32_t,4>,49> records={{
        {0,1,591,0},{4,0,0,0},{5,0,0,0},{6,0,0,0},{7,0,0,0},{1,1,591,0},
        {8,0,0,0},{55,0,0,0},{10,0,0,0},{11,0,0,0},{62,0,0,0},{12,0,0,0},
        {63,0,0,0},{0xffffffffu,0xffffffffu,0,0},
        {2,1,591,0},{56,1,573,0},{13,0,0,0},{14,0,0,0},{64,2,0,0},{110,0,0,0},
        {65,2,0,0},{111,0,0,0},{66,2,0,0},{112,0,0,0},{67,2,0,0},{113,0,0,0},
        {114,2,0,0},{118,0,0,0},{115,2,0,0},{119,0,0,0},{116,2,0,0},{120,0,0,0},
        {117,2,0,0},{121,0,0,0},{15,0,0,0},{16,0,0,0},{68,0,0,0},
        {0xffffffffu,0xffffffffu,0,0},
        {17,0,0,0},{57,1,129,0},{18,0,0,0},{19,0,0,0},{69,0,0,0},
        {0xffffffffu,0xffffffffu,0,0},
        {3,1,591,0},{58,1,564,0},{20,0,0,0},{21,0,0,0},
        {0xffffffffu,0xffffffffu,0,0}
    }};
    for(unsigned i=0;i<records.size();++i)
        for(unsigned j=0;j<4;++j)put32(data,animations+i*16+j*4,records[i][j]);
    constexpr unsigned streams=animations+49*16;
    constexpr std::array<std::uint32_t,8> constants={12,24,6,18,0xfffffc18u,10000,0,0x3f800000u};
    for(unsigned i=0;i<constants.size();++i)put32(data,streams+i*4,constants[i]);
    return data;
}
}

int main() {
    auto bytes=fixture();
    ResultsUiData results{Bytes(bytes)};
    assert(results.definition().type()==11);
    assert(results.definition().glyph_capacity()==33);
    assert(results.definition().sprite_capacity()==17);
    assert(results.widget_ids()[0]==0x40&&results.widget_ids()[15]==0x79);
    assert(results.animation_records().size()==49);
    assert(results.animation_table(0).size()==14);
    assert(results.animation_table(1).size()==24);
    assert(results.animation_table(2).size()==6);
    assert(results.animation_table(3).size()==5);
    assert((results.stream_constants()[0]==std::array<std::uint32_t,4>{12,24,6,18}));
    bool bounds=false;try { (void)results.animation_table(4); } catch(const std::runtime_error&) {bounds=true;}
    assert(bounds);

    auto malformed=bytes; malformed[0]='X';
    bool rejected=false;try { ResultsUiData bad{Bytes(malformed)}; } catch(const std::runtime_error&) {rejected=true;}
    assert(rejected);
    malformed=bytes; put32(malformed,8,2);
    rejected=false;try { ResultsUiData bad{Bytes(malformed)}; } catch(const std::runtime_error&) {rejected=true;}
    assert(rejected);
    malformed=bytes; put32(malformed,header,2);
    rejected=false;try { ResultsUiData bad{Bytes(malformed)}; } catch(const std::runtime_error&) {rejected=true;}
    assert(rejected);
    malformed=bytes; malformed[56]^=1;
    rejected=false;try { ResultsUiData bad{Bytes(malformed)}; } catch(const std::runtime_error&) {rejected=true;}
    assert(rejected);
    malformed=bytes; malformed.pop_back();
    rejected=false;try { ResultsUiData bad{Bytes(malformed)}; } catch(const std::runtime_error&) {rejected=true;}
    assert(rejected);
}
