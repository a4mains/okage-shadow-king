#pragma once
#include "okage/assets.hpp"
#include <span>

namespace okage {
void commit_battle_result_rewards(std::span<std::uint8_t> variables,Bytes reward);
}
