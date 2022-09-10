#pragma once
#include "okage/battle_interface.hpp"

namespace okage {
// Inspection only. Source states are serialized, never loaded into a Session.
std::string battle_ui_update_snapshot(const BattleInterface&,const BattleUiShared&,
    const RuwResource&,Bytes variables,const UiPartySelection&);
}
