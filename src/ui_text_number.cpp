#include "okage/ui_text_number.hpp"
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace okage {
namespace {
using Wide=__uint128_t;
void need(bool yes,const char* message) {
    if(!yes)throw std::runtime_error(message);
}
bool space(char c) {return c==' '||(c>='\t'&&c<='\r');}
bool digit(char c) {return c>='0'&&c<='9';}
int width(Wide n) {int result=0;for(;n;n>>=1)++result;return result;}
Wide shift(Wide n,unsigned bits) {
    need(bits<128&&width(n)+int(bits)<=128,"UI decimal intermediate exceeds supported width");
    return n<<bits;
}
std::uint64_t rounded_ratio(Wide numerator,Wide denominator) {
    const Wide quotient=numerator/denominator,remainder=numerator%denominator;
    // Compare r with d-r instead of computing 2*r (which could overflow).
    return std::uint64_t(quotient+(remainder>denominator-remainder||
        (remainder==denominator-remainder&&(quotient&1))));
}
float convert(std::uint32_t mantissa,int decimal_exponent,bool negative) {
    if(!mantissa)return __builtin_bit_cast(float, (std::uint32_t(0)));
    Wide numerator=mantissa,denominator=1;
    for(int i=0;i<decimal_exponent;++i)numerator*=10;
    for(int i=0;i>decimal_exponent;--i)denominator*=10;
    int exponent=width(numerator)-width(denominator);
    if(exponent>=0) {
        if(numerator<shift(denominator,unsigned(exponent)))--exponent;
    } else if(shift(numerator,unsigned(-exponent))<denominator)--exponent;
    const int scaling=52-exponent;
    auto significand=rounded_ratio(scaling>=0?shift(numerator,unsigned(scaling)):numerator,
        scaling>=0?denominator:shift(denominator,unsigned(-scaling)));
    if(significand==(std::uint64_t(1)<<53)){significand>>=1;++exponent;}
    auto single=significand>>29;
    const auto remainder=significand&((std::uint64_t(1)<<29)-1);
    const auto midpoint=std::uint64_t(1)<<28;
    if(remainder>midpoint||(remainder==midpoint&&(single&1)))++single;
    if(single==(std::uint64_t(1)<<24)){single>>=1;++exponent;}
    need(exponent>=-126&&exponent<=127,"UI decimal non-normal result unsupported");
    const auto bits=(negative?0x80000000u:0u)|(std::uint32_t(exponent+127)<<23)|
                    (std::uint32_t(single)&0x7fffffu);
    return __builtin_bit_cast(float, (bits));
}
}
float parse_ui_text_number(std::string_view field) {
    static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559);
    need(!field.empty()&&field.size()<=128,"UI decimal field extent unsupported");
    for(unsigned char c:field)need(c>0&&c<128,"UI decimal requires non-NUL ASCII");
    while(!field.empty()&&space(field.front()))field.remove_prefix(1);
    while(!field.empty()&&space(field.back()))field.remove_suffix(1);
    need(!field.empty(),"UI decimal has no digits");
    bool negative=false;
    if(field.front()=='+'||field.front()=='-') {
        negative=field.front()=='-';field.remove_prefix(1);
    }
    std::string digits;std::size_t at=0;unsigned fractional=0;
    while(at<field.size()&&digit(field[at]))digits.push_back(field[at++]);
    if(at<field.size()&&field[at]=='.') {
        ++at;
        while(at<field.size()&&digit(field[at])){digits.push_back(field[at++]);++fractional;}
    }
    need(!digits.empty(),"UI decimal has no digits");
    int authored_exponent=0;
    if(at<field.size()&&(field[at]=='e'||field[at]=='E')) {
        ++at;bool minus=false;
        if(at<field.size()&&(field[at]=='+'||field[at]=='-'))minus=field[at++]=='-';
        const auto begin=at;
        while(at<field.size()&&digit(field[at])) {
            authored_exponent=authored_exponent*10+field[at++]-'0';
            need(authored_exponent<=128,"UI decimal authored exponent unsupported");
        }
        need(at!=begin,"UI decimal exponent has no digits");
        if(minus)authored_exponent=-authored_exponent;
    }
    need(at==field.size(),"UI decimal trailing syntax unsupported");
    while(fractional&&!digits.empty()&&digits.back()=='0'){digits.pop_back();--fractional;}
    const auto first=digits.find_first_not_of('0');
    if(first==std::string::npos)digits.clear();else digits.erase(0,first);
    need(digits.size()<=9,"UI decimal significant digits exceed fast-path scope");
    const int exponent=authored_exponent-int(fractional);
    need(exponent>=-22&&exponent<=22,"UI decimal effective exponent outside fast-path scope");
    std::uint32_t mantissa=0;for(char c:digits)mantissa=mantissa*10+unsigned(c-'0');
    return convert(mantissa,exponent,negative);
}
}
