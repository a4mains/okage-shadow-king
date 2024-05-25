#pragma once

#include "okage/battle_ui.hpp"
#include "okage/ui_window.hpp"
#include <string>
#include <string_view>

namespace okage {

enum class RestoreStringId : std::uint32_t {
    Slash = 1,
    ProductCode,
    TextPrefix,
    FormatYesNo,
    YesNo,
    SaveSlotPrompt,
    ReadSlotPrompt,
    SlotLabels,
    Empty,
    NameFormat,
    PlayerNameFormat,
    PlayTimeFormat,
    HitPointsFormat,
    LevelFormat,
    NeutralStatus,
    GenericStringFormat,
    CurseStatusFormat,
    SlotNumberFormat,
    MenuNumberFormat,
    ProgressNumberFormat,
    CheckingCardNotice,
    NoCardNotice,
    UnformattedCardNotice,
    CancelSaveNotice,
    FormattingNotice,
    FormatFailedNotice,
    SaveFilePathFormat,
    NewSaveSpaceNotice,
    IconSysPathFormat,
    SaveIcon1PathFormat,
    SaveIcon2PathFormat,
    SaveIcon3PathFormat,
    PathJoinFormat,
    CorruptSaveNotice,
    ReplaceSavePrompt,
    ExistingSaveSpaceNotice,
    SavingNotice,
    DataDirectory,
    SaveCompleteNotice,
    SaveFailedNotice,
    NoSavedDataNotice,
    ReadFilePrompt,
    LoadSuccessNotice,
    LoadFailedNotice,
};

class RestoreUiData {
public:
    explicit RestoreUiData(Bytes);

    BattleUiDefinition definition;
    std::vector<UiWindow::AtlasTuple> atlas; // Includes the all-zero terminator.

    std::string_view string(RestoreStringId) const;

    static std::array<UiStreamCommand, 2> stream_commands();
    static constexpr std::uint32_t type8_dispatch_handler_id = 1;

private:
    std::array<std::string, 44> strings_;
};

} // namespace okage
