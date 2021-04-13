#pragma once
#include "okage/model.hpp"

namespace okage {
// 0x1b9be0 multiplies the current camera position by the STG sky factors.
Float4 sky_follow_position(const Float4& camera,const Float4& factors);

std::array<float,2> texture_coordinates(const TextureState& state,
    std::uint32_t width,std::uint32_t height,std::array<float,2> source);
// Recovered 0x17be50 GS CLAMP value. Both axes select region clamp (2) for
// source mode 1, region repeat (3) otherwise. Preserve original field packing.
std::uint64_t texture_clamp(const TextureState& state,std::uint32_t width,std::uint32_t height);

std::vector<std::array<unsigned,3>> triangle_corners(std::size_t count);
}
