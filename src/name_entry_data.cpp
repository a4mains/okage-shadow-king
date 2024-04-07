#include "okage/name_entry.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace okage {
NameEntryData::NameEntryData(Bytes bytes) {
    const auto fail=[] {throw std::runtime_error("Unsupported name entry import");};
    const auto word=[&](unsigned at){if(at+4>bytes.size())fail();return unsigned(bytes[at])|(unsigned(bytes[at+1])<<8)|(unsigned(bytes[at+2])<<16)|(unsigned(bytes[at+3])<<24);};
    constexpr std::array<std::uint8_t,8> magic{'O','K','N','M','E','D','2',0};
    if(bytes.size()!=265||!std::equal(magic.begin(),magic.end(),bytes.begin())||word(8)!=2||word(12)!=1||word(16)!=28||word(20)!=112)fail();
    constexpr std::string_view hex="0123456789abcdef",elf="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",source="b8356ed62e594a4c2b1e9dc550df48200987ddd861fd8862057fa1cb3820be74";
    for(unsigned i=0;i<64;++i){if(hex[(bytes[24+i/2]>>(i%2?0:4))&15]!=elf[i]||hex[(bytes[56+i/2]>>(i%2?0:4))&15]!=source[i])fail();}
    for(unsigned i=0;i<28;++i)definition.words[i]=word(88+i*4);
    for(unsigned i:{0u,21u,22u,23u})if(definition.words[i]!=1)fail();
    if(definition.type()!=9||definition.glyph_capacity()!=32||definition.sprite_capacity()!=16||definition.words[27])fail();
    UiWindow::AtlasTuple tuple{};for(unsigned i=0;i<4;++i)if(word(200+i*4))fail();atlas.push_back(tuple);
    for(unsigned i=0;i<4;++i){const auto bits=std::uint64_t(word(216+i*8))|(std::uint64_t(word(220+i*8))<<32);spacing[i]=std::bit_cast<double>(bits);if(!std::isfinite(spacing[i]))fail();}
    std::copy_n(bytes.begin()+248,4,default_name.begin());
    if(default_name[3])fail();unsigned at=252;
    for(auto* out:{&empty,&space}){const auto n=word(at);at+=4;if(n==0||n>64||at+n>bytes.size()||bytes[at+n-1])fail();if(std::find(bytes.begin()+at,bytes.begin()+at+n-1,0)!=bytes.begin()+at+n-1)fail();out->assign(reinterpret_cast<const char*>(bytes.data()+at),n-1);at+=n;}
    if(at!=bytes.size())fail();
}
}
