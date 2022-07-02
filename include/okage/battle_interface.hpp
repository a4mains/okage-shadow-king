#pragma once
#include "okage/battle_hud.hpp"
#include "okage/ui_window.hpp"
#include <functional>

namespace okage {
struct BattleUiShared {
    BattleHud hud;
    std::array<std::shared_ptr<UiWindowStream>,2> streams{
        std::make_shared<UiWindowStream>(UiWindowStream{1,{}}),
        std::make_shared<UiWindowStream>(UiWindowStream{2,{}})};
    std::array<std::shared_ptr<UiWindow>,8> registry{};
    std::vector<UiWindow::AtlasTuple> atlas{UiWindow::AtlasTuple{}};
    int atlas_texture=-1;
    bool atlas_dirty=false;
    unsigned current_stream=0,stream_cursor=0;
};
class BattleInterface {
public:
    BattleInterface(BattleUiShared&,const BattleUiData&,std::shared_ptr<const void> texture,
                    std::uint32_t parent_flags,const std::function<void(unsigned)>& constructor);
    ~BattleInterface();
    BattleInterface(const BattleInterface&)=delete;
    BattleInterface& operator=(const BattleInterface&)=delete;
    const auto& words() const {return words_;}
    const auto& handles() const {return handles_;}
    const UiWindow& window(unsigned layer) const;
private:
    void release();
    BattleUiShared& shared_;
    std::array<std::uint32_t,0xef0/4> words_{};
    std::array<int,2> handles_{-1,-1};
};
}
