#include "okage/battle_projectiles.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at){
    if(at+4>b.size())throw std::runtime_error("Battle projectile data truncated");
    std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(b[at+i])<<(8*i);return v;
}
}
BattleProjectileData::BattleProjectileData(Bytes b) {
    static constexpr char magic[8]={'O','K','B','P','R','J','1','\0'};
    if(b.size()<92||std::memcmp(b.data(),magic,8))throw std::runtime_error("Battle projectile artifact magic");
    if(word(b,8)!=1)throw std::runtime_error("Battle projectile artifact version");
    base_=word(b,12);const auto count=word(b,16);stride_=word(b,20);const auto size=word(b,24);
    if(stride_!=16||count==0||count>256||size!=count*16||b.size()!=92+size)
        throw std::runtime_error("Battle projectile artifact extent");
    for(unsigned i=0;i<count;++i) {
        const auto at=92+i*16;BattleProjectile p;
        p.type=std::int16_t(word(b,at)&0xffff);p.duration=std::int16_t(word(b,at)>>16);
        for(unsigned k=0;k<3;++k){p.offset[k]=std::bit_cast<float>(word(b,at+4+k*4));
            if(!std::isfinite(p.offset[k]))throw std::runtime_error("Battle projectile nonfinite offset");}
        p.offset[3]=0;
        if(p.duration<=0)throw std::runtime_error("Battle projectile nonpositive duration");
        records_.push_back(p);
    }
}
const BattleProjectile& BattleProjectileData::at_source_pointer(std::uint32_t pointer) const {
    if(pointer<base_||(pointer-base_)%stride_||(pointer-base_)/stride_>=records_.size())
        throw std::runtime_error("Battle descriptor projectile pointer is outside the owned table");
    return records_[(pointer-base_)/stride_];
}
}
