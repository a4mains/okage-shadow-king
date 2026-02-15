#include "okage/battle_projectiles.hpp"
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected rejection");}
void put(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=std::uint8_t(v>>(8*i));}
std::vector<std::uint8_t> artifact(unsigned count){
    std::vector<std::uint8_t> b(92+count*16);std::memcpy(b.data(),"OKBPRJ1\0",8);
    put(b,8,1);put(b,12,0x237418);put(b,16,count);put(b,20,16);put(b,24,count*16);
    for(unsigned i=0;i<count;++i){put(b,92+i*16,(30u<<16)|(1+i%2));
        put(b,96+i*16,std::bit_cast<std::uint32_t>(float(i)));put(b,100+i*16,std::bit_cast<std::uint32_t>(-2.5f));}
    return b;
}
}
int main(){try{
    const auto bytes=artifact(22);okage::BattleProjectileData data(bytes);
    require(data.size()==22,"record count");
    const auto& second=data.at_source_pointer(0x237428);
    require(second.type==2&&second.duration==30&&second.offset[0]==1&&second.offset[1]==-2.5f&&second.offset[3]==0,"record decode");
    for(std::uint32_t bad:{0u,0x237408u,0x23741cu,0x237578u})rejects([&]{(void)data.at_source_pointer(bad);});
    auto broken=bytes;broken[0]='X';rejects([&]{okage::BattleProjectileData{broken};});
    broken=bytes;put(broken,92,0);rejects([&]{okage::BattleProjectileData{broken};});
    broken=bytes;broken.pop_back();rejects([&]{okage::BattleProjectileData{broken};});
    std::cout<<"Battle projectile table decode and source-pointer bounds passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
