#pragma once
#include "okage/assets.hpp"
#include <span>
#include <vector>

namespace okage {
std::int32_t event_variable(Bytes variables, std::uint32_t selector);

std::vector<std::uint32_t> run_event_setup(Bytes script, Bytes variables,
                                         std::span<std::uint8_t> field_state);
}
