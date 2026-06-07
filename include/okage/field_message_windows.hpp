#pragma once
#include "okage/battle_interface.hpp"
#include "okage/dialogue.hpp"
#include "okage/ruw.hpp"
#include "okage/text.hpp"
#include "okage/ui_window.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace okage {
class FieldMessageWindowData {
public:
    explicit FieldMessageWindowData(Bytes);
    const std::array<std::uint32_t,28>& row(unsigned layer) const {return rows_.at(layer);}
    std::span<const UiWindow::AtlasTuple> atlas() const {return atlas_;}
    const std::array<std::uint32_t,3>& cursor_selectors() const {return selectors_;} //0x229010
private:
    std::array<std::array<std::uint32_t,28>,2> rows_{};
    std::vector<UiWindow::AtlasTuple> atlas_;
    std::array<std::uint32_t,3> selectors_{};
};

struct FieldMessageGlobals {
    std::int32_t decoration_stage_memo=-1; //0x2dbc60 (0x147fe0)
    std::string last_speaker;              //0x2dbb60 (0x146ee8)
    std::int32_t speaker_ticks=0;          //0x2dbc30
    std::int32_t body_settle=0;            //0x2dbc38
    std::int32_t cursor_counter=0;         //0x2dbbe0
    std::int32_t choice_row=-1;            //0x1fcb70
    std::array<std::string,3> narration_source; //0x2db740 rows
    std::array<std::int32_t,3> narration_ticks{}; //0x1fcbb8
    // BSS layer streams 0x2dbb40/0x2dbc40: empty in the ELF; each layer
    // constructor rewrites its own stream, and 0x198e38 scans the old one.
    std::shared_ptr<UiWindowStream> decoration_stream=std::make_shared<UiWindowStream>(UiWindowStream{1,{}});
    std::shared_ptr<UiWindowStream> body_stream=std::make_shared<UiWindowStream>(UiWindowStream{2,{}});
};

// Values 0x146ee8 reads from the body text object and controller, taken from
// the native logical dialogue model at the source updater position.
struct FieldMessageBodyState {
    std::string_view speaker;                        //window+0x1a70 (19ae30)
    const std::array<char,256>* replacement=nullptr; //window+0x1b70 (19afb0)
    std::string_view description;                    //window+0x1d70 (19ae60)
    std::string_view player_name;                    //1252a8(2)+0x334
    unsigned status=1;              //0x1a5790
    std::int32_t choice_cursor=0;   //0x19b418
    bool layer_ready=false;         //layer+0x50 & 0x10 (0x11fda0)
    std::int32_t window_type=-1;    //node+0xed8 (0x11fd68)
    std::uint32_t choice_accepted=0;//0x1fcb74 (ChoiceMenu::accepted)
    bool choice_armed=false;        //0x1fcb68 (DialogueController::choice_armed)
    std::int32_t question_flash=0,question_flash_row=0; //0x1fcb78/0x1fcb7c
    const QuestionChoice* question=nullptr;
    std::int32_t level=0,money=0;   //1252a8(2)+0x80, +0x0c
};
struct FieldMessageServices {
    std::function<std::string(unsigned)> system_message; //0x123530
    std::function<std::string(std::int32_t)> number2;    //0x124200
    std::function<std::string(std::int32_t)> money;      //0x123a90
};

class FieldMessageWindows {
public:
    enum class Construction {ScriptPass,ManagerPhase8};
    FieldMessageWindows(BattleUiShared&,const FieldMessageWindowData&,std::shared_ptr<RuwResource>,
                        std::shared_ptr<const void> texture,Construction,std::int32_t message_id,
                        struct FieldMessageGlobals&);
    ~FieldMessageWindows(); //0x120b48 -> 0x199458 for both windows.
    FieldMessageWindows(const FieldMessageWindows&)=delete;
    FieldMessageWindows& operator=(const FieldMessageWindows&)=delete;

    // Window effects of the shared controller stage word before the native
    // stage update of this traversal (0x148958 for A, then 0x148dc8 for B).
    void controllers(unsigned stage,std::int32_t window_type);
    // 0x147fe0 (A) then 0x146ee8/0x146288/0x146730 (B) RUW and window writes.
    void updaters(const FieldMessageBodyState&,FieldMessageGlobals&,const FieldMessageServices&);

    struct Pass {
        UiLayoutShared& shared;
        std::shared_ptr<const std::vector<Font>> fonts;
        std::span<const float> sine;
        std::function<std::uint32_t()> random;
        std::array<std::uint32_t,16>& accumulators;
        const UiPacketBuildContext& packets;
    };
    void resource_pass(const Pass&,const DialogueOpacity& decoration,
                       const std::function<void(const TextPlacementHooks*)>& body_frame);

    bool decoration_first_in_registry() const {return slot_a_<slot_b_;}
    unsigned decoration_slot() const {return slot_a_;}
    unsigned body_slot() const {return slot_b_;}
    const UiWindow& decoration() const;
    const UiWindow& body() const;
    unsigned decoration_layouts() const {return a_layouts_;}
    unsigned body_passes() const {return b_passes_;}
    // Per-pass RNG accounting of the last resource pass, in call order.
    struct PassDraws {unsigned a_draws=0,b_draws=0,b_glyphs=0;bool a_laid_out=false,b_constructor=false,b_pass=false;};
    const PassDraws& last_pass() const {return last_;}
private:
    UiWindow& live(unsigned slot,const UiWindow* expected) const;
    void bind_decoration(std::size_t group);
    BattleUiShared& ui_;
    std::shared_ptr<RuwResource> ruw_;
    std::array<std::uint32_t,28> row_a_{},row_b_{};
    unsigned slot_a_=0,slot_b_=0;
    const UiWindow* window_a_=nullptr;const UiWindow* window_b_=nullptr;
    std::array<std::uint32_t,8> layer_a_xyz_{};
    bool body_text_mode_=false; //0x148dc8 case1 ran: B left its constructor stream (mode 1).
    std::int32_t decoration_counter_=0; //layer A +0xa8
    unsigned a_layouts_=0,b_passes_=0;
    PassDraws last_;
};
}
