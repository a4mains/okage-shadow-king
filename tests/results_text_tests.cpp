#include "okage/results_text.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace okage;
namespace {
constexpr unsigned header=80;
constexpr std::array<std::string_view,5> source_strings={
    "000,000,00", "0", "%03d,%03d,%03d",
    "\\f[1]/\\c[%d]%s\\c[0]%s", "\\f[1]\\c[%d]%s\\c[0]%s"
};
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void put32(std::vector<std::uint8_t>& bytes,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes[at+i]=std::uint8_t(value>>(i*8));
}
void put_hex(std::vector<std::uint8_t>& bytes,unsigned at,std::string_view hex) {
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<hex.size()/2;++i) {
        const auto high=digits.find(hex[i*2]),low=digits.find(hex[i*2+1]);
        require(high!=digits.npos&&low!=digits.npos,"hex fixture digit");
        bytes[at+i]=std::uint8_t((high<<4)|low);
    }
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> bytes(header+5*64);
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','T','X','T','1',0};
    std::copy(magic.begin(),magic.end(),bytes.begin());
    put32(bytes,8,1);put32(bytes,12,320);
    put_hex(bytes,16,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    put_hex(bytes,48,"0000000000000000000000000000000000000000000000000000000000000000");
    for(unsigned i=0;i<source_strings.size();++i)
        std::copy(source_strings[i].begin(),source_strings[i].end(),bytes.begin()+header+i*64);
    return bytes;
}
}

int main() {
    auto bytes=fixture();ResultsTextData text{Bytes(bytes)};
    const auto color=std::to_string(0x1effff7fu);
    const auto plain=[&](std::string_view prefix,std::string_view suffix) {
        return "\\f[1]\\c["+color+"]"+std::string(prefix)+"\\c[0]"+std::string(suffix);
    };
    const auto summary=[&](std::string_view prefix,std::string_view suffix) {
        return "\\f[1]/\\c["+color+"]"+std::string(prefix)+"\\c[0]"+std::string(suffix);
    };
    require(text.money(-1)==plain("000,000,00","0"),"negative money uses source zero layout");
    require(text.money(0)==plain("000,000,00","0"),"zero money uses source zero layout");
    require(text.money(1)==plain("000,000,00","1"),"money low digits");
    require(text.money(1000)==plain("000,00","1,000"),"money middle group");
    require(text.money(1000000)==plain("00","1,000,000"),"money high group");
    require(text.money(123456789)==plain("","123,456,789"),"money full groups");
    require(text.money(2147483647)==plain("","147,483,647"),"money signed maximum groups");
    require(text.money_summary(1000000)==summary("00","1,000,000"),"summary format includes source slash");

    auto malformed=bytes;malformed[0]='X';
    bool rejected=false;try {ResultsTextData bad{Bytes(malformed)};} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"reject bad magic");
    malformed=bytes;put32(malformed,8,2);
    rejected=false;try {ResultsTextData bad{Bytes(malformed)};} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"reject bad version");
    malformed=bytes;malformed[16]^=1;
    rejected=false;try {ResultsTextData bad{Bytes(malformed)};} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"reject bad ELF hash");
    malformed=bytes;malformed.pop_back();
    rejected=false;try {ResultsTextData bad{Bytes(malformed)};} catch(const std::runtime_error&) {rejected=true;}
    require(rejected,"reject malformed extent");
}
