#include "okage/ui_text_number.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value){if(!value)throw std::runtime_error("UI decimal assertion failed");}
void expect(std::string_view text,std::uint32_t expected) {
    require(__builtin_bit_cast(std::uint32_t, (okage::parse_ui_text_number(text)))==expected);
}
void rejects(std::string_view text) {
    try{(void)okage::parse_ui_text_number(text);}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected unsupported UI decimal");
}
void values() {
    expect("0.76",0x3f428f5c);expect("0.620",0x3f1eb852);
    expect("0.8",0x3f4ccccd);expect("0.75",0x3f400000);expect("1.0",0x3f800000);
    expect("  \t+000.62000\r\n",0x3f1eb852);expect("-.5",0xbf000000);
    expect("000123.4500",0x42f6e666);expect("000000001",0x3f800000);
    expect("1.",0x3f800000);expect("125e-2",0x3fa00000);
    expect("12.5E+1",0x42fa0000);expect("0.001e3",0x3f800000);
    expect("-0",0);expect("-0.000e+2",0);expect("+0.0",0);
    // Integer half-ULP ties exercise both even directions, including sign.
    expect("16777217",0x4b800000);expect("16777219",0x4b800002);
    expect("-16777217",0xcb800000);expect("-16777219",0xcb800002);
    // Decimal values bracketing the1.0 float midpoint, and exact rational ties.
    expect("1.00000005",0x3f800000);expect("1.00000006",0x3f800001);
    expect("999999999",0x4e6e6b28);
    expect("1e-22",0x1af1c901);expect("1e22",0x64078678);
    expect("999999999e22",0x72fc6f7c);
}
void bad() {
    for(auto s:{""," ","+",".","1e","1e+","1e-","nan","inf","0x1p0",
                "1,2","1]","1 2","1.2.3","--1","1234567891","1e23","1e-23",
                "1000000000","1.0000000596046448","1e129",".00000000000000000000001"})rejects(s);
    rejects(std::string(129,'0'));rejects(std::string("1\0",2));rejects(std::string("1\xff",2));
}
}
int main() {
    try {
        std::fenv_t saved;require(std::fegetenv(&saved)==0);
        for(int mode:{FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD}) {
            require(std::fesetround(mode)==0);std::feclearexcept(FE_ALL_EXCEPT);
            std::feraiseexcept(FE_INVALID|FE_DIVBYZERO|FE_INEXACT);
            const auto flags=std::fetestexcept(FE_ALL_EXCEPT);
            values();bad();require(std::fegetround()==mode&&std::fetestexcept(FE_ALL_EXCEPT)==flags);
        }
        std::fesetenv(&saved);
        std::cout<<"UI decimal grammar, nearest-even conversion and fenv invariants passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
