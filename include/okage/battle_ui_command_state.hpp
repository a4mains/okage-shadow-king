#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace okage {
class UiWindow;
struct BattleUiCommandState {
    std::array<std::uint32_t,8> changed{}; //1fd840
    std::uint32_t command=0;              //1fd860
    std::array<std::uint32_t,8> values{};  //1fd868
    void import_layer(std::span<const std::uint32_t> layer,
                      std::uint32_t& menu_state,std::int32_t& input_inhibit,
                      UiWindow&);
    static void move_layer_y(std::span<std::uint32_t> layer,
                             std::uint32_t menu_state,UiWindow&);
};
}
