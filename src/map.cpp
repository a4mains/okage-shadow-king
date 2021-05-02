#include "okage/map.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void require(bool value,const char* why) { if (!value) throw std::runtime_error(why); }
struct Reader {
    Bytes bytes;
    void range(std::size_t at,std::size_t size) const {
        require(at<=bytes.size() && size<=bytes.size()-at,"XMP record outside input");
    }
    std::uint32_t word(std::size_t at) const {
        range(at,4);return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8)|
            (std::uint32_t(bytes[at+2])<<16)|(std::uint32_t(bytes[at+3])<<24);
    }
};
struct Rounding {
    int previous=std::fegetround();
    Rounding() { require(!std::fesetround(FE_TOWARDZERO),"Cannot set map rounding mode"); }
    ~Rounding() { std::fesetround(previous); }
};
}
MapFile read_xmp(Bytes bytes) {
    Reader r{bytes};r.range(0,64);MapFile result{};
    for (unsigned i=0;i<16;++i) result.header[i]=r.word(i*4);
    const std::size_t objects=result.header[2],count=result.header[3];
    const std::size_t definitions=result.header[10],definitionCount=result.header[11];
    require(definitionCount<=640,"XMP exceeds retail definition table");
    require(count<=bytes.size()/80,"XMP placement count exceeds input");
    require(objects>=64 && definitions>=64 && objects%4==0 && definitions%4==0,"Invalid XMP table offset");
    r.range(objects,count*80);r.range(definitions,definitionCount*148);
    require(!count || !definitionCount || objects+count*80<=definitions || definitions+definitionCount*148<=objects,
            "Overlapping XMP tables");
    for (std::size_t i=0;i<definitionCount;++i) {
        const auto at=definitions+i*148;MapDefinition d{};
        for (unsigned j=0;j<37;++j) d.words[j]=r.word(at+j*4);
        unsigned end=0;while (end<28 && bytes[at+end]) ++end;
        require(end>0 && end<28,"Invalid XMP model name");
        d.name.assign(reinterpret_cast<const char*>(bytes.data()+at),end);
        // References are archive member names, never arbitrary filesystem paths.
        require(d.name!="." && d.name!=".." && d.name.find_first_of("/\\:")==std::string::npos,
                "Unsafe XMP member name");
        result.definitions.push_back(std::move(d));
    }
    for (std::size_t i=0;i<count;++i) {
        MapPlacement p{};for (unsigned j=0;j<20;++j) p.words[j]=r.word(objects+i*80+j*4);
        require(p.definition()<definitionCount,"XMP placement references missing definition");
        for (unsigned j=4;j<16;++j) require(std::isfinite(__builtin_bit_cast(float, (p.words[j]))),"Non-finite XMP transform");
        result.placements.push_back(p);
    }
    return result;
}
MapTransform map_transform(const MapPlacement& p,const MapDefinition& d) {
    for (unsigned j=4;j<16;++j) require(std::isfinite(__builtin_bit_cast(float, (p.words[j]))),"Non-finite XMP transform");
    auto f=[&](unsigned word){return __builtin_bit_cast(float, (p.words[word]));};
    Rounding rounding;
    const auto turns=__builtin_bit_cast(float, (std::uint32_t(0x3b360b61)));
    MapTransform result{{f(4),-f(5),-f(6),1},
        {f(8)*turns,1.0f-f(9)*turns,1.0f-f(10)*turns,f(11)},
        {f(12),f(13),f(14),f(15)},p.flags()|d.flags()};
    const auto& s=result.scale;
    if (((s[0]==s[1] && s[2]==1) || (s[0]!=s[1] && s[2]==0)) && (result.flags&0xffff)==0x80)
        result.flags|=0x100000;
    return result;
}
Matrix4 map_instance_matrix(const MapTransform& transform,const Float4& camera_rotation,const SineTable& sine) {
    const auto rotation=(transform.flags&1) ? Float4{camera_rotation[0],camera_rotation[1],0,1} : transform.rotation;
    return instance_matrix(transform.position,rotation,transform.scale,sine);
}
std::vector<int> map_instance_ids(const MapFile& map) {
    std::vector<int> result;unsigned front=0,back=2048;
    for(const auto& p:map.placements) {
        if(p.handler()==1){result.push_back(-1);continue;}
        require(p.handler()==0,"Map handler instance allocation is unrecovered");
        const auto transform=map_transform(p,map.definitions.at(p.definition()));
        if(transform.flags&0x100000) {
            require(back>0&&--back>front,"Map instance table exhausted");
            result.push_back(int(back));
        } else {
            require(front<back,"Map instance table exhausted");
            result.push_back(int(front++));
        }
    }
    return result;
}
}
