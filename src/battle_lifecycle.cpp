#include "okage/battle_lifecycle.hpp"
#include <stdexcept>
namespace okage {
namespace {
unsigned flags(const BattleState& root) {
    unsigned result=0;for(unsigned i=0;i<4;++i)result|=unsigned(root.bytes[0x1c+i])<<(8*i);return result;
}
void flags(BattleState& root,unsigned value) {
    for(unsigned i=0;i<4;++i)root.bytes[0x1c+i]=std::uint8_t(value>>(8*i));
}
}
void BattleRootLifetime::wrapper_update(const BattleState& root,
    const std::function<void(GameModeSelection)>& request) const {
    if(failure_)std::rethrow_exception(failure_);
    if(!linked_)throw std::runtime_error("Battle wrapper revisited an unlinked root");
    if(flags(root)&0x80000000u) {
        if(!request)throw std::runtime_error("Battle wrapper mode-request owner missing");
        request({2,0xffffffffu,0});
    }
}
void BattleRootLifetime::traverse_root(BattleState& root,const std::function<void()>& normal,
    const std::function<void()>& destroy,const std::function<void()>& release) {
    if(failure_)std::rethrow_exception(failure_);
    if(!linked_)return;
    try {
    if(!(flags(root)&0x80000000u))normal();
    if(!(flags(root)&0x80000000u))return;
    const auto next=flags(root)^0x40000000u;flags(root,next);
    if(next&0x40000000u)destroy();
    else {release();linked_=false;}
    } catch(...) {failure_=std::current_exception();throw;}
}
}
