#pragma once
#include "okage/model.hpp"

namespace okage {
using Matrix4 = std::array<Float4, 4>;
using SineTable = std::array<float, 16384>;
SineTable make_sine_table();
std::vector<Matrix4> rest_pose(const Model& model, const SineTable& sine);
// XMT rotations already use turns. Four channels precede the bone channels.
std::vector<Matrix4> animated_pose(const Model& model, std::span<const Float4> channels,
                                 const SineTable& sine, bool scale_channels=false);
std::vector<Float4> posed_positions(const RenderBlock& block, std::span<const Matrix4> pose);
}
