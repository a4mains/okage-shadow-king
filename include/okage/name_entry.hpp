#pragma once
#include "okage/battle_interface.hpp"

namespace okage {
class NameEntryData {
public:
    explicit NameEntryData(Bytes);
    BattleUiDefinition definition;
    std::vector<UiWindow::AtlasTuple> atlas;
    std::array<double,4> spacing{};
    std::array<std::uint8_t,4> default_name{};
    std::string empty,space;
};
struct NameEntryShared {
    std::shared_ptr<UiWindowStream> stream=std::make_shared<UiWindowStream>(UiWindowStream{1,{}});
    std::array<char,2> selected_character{};
    std::uint32_t flag=0;
};
class NameEntryInterface {
public:
    NameEntryInterface(BattleUiShared&,NameEntryShared&,const NameEntryData&,
        std::shared_ptr<RuwResource>,std::shared_ptr<const void> texture,std::uint32_t parent_flags);
    ~NameEntryInterface();
    NameEntryInterface(const NameEntryInterface&)=delete;
    NameEntryInterface& operator=(const NameEntryInterface&)=delete;
    void first_update(BattleUiPad&,BattleUiPadInput,std::span<std::uint8_t> variables,
                      const std::function<void(unsigned)>& sound);
    void second_update(std::span<std::uint8_t> variables,const GameDataTables&);
    void destroy_windows();
    bool dead() const {return (words_[0x1c/4]&0x80000000u)!=0;}
    unsigned handle() const;
    const auto& words() const {return words_;}
private:
    void bind(unsigned group);
    UiWindow& window();
    BattleUiShared& shared_;
    NameEntryShared& name_;
    const NameEntryData& data_;
    std::shared_ptr<RuwResource> resource_;
    std::array<std::uint32_t,0xef0/4> words_{};
    int handle_=-1;
};
}
