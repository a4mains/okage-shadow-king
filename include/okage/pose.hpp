#pragma once
#include "okage/model.hpp"

namespace okage {
using Matrix4 = std::array<Float4, 4>;
using SineTable = std::array<float, 16384>;
SineTable make_sine_table();
// Instance transform from evaluated channels: translation, Z/Y/X rotation in
// turns, then basis scale. Retail 0x1b9ff8..0x1ba13f with original VU0 routines.
Matrix4 instance_matrix(const Float4& position,const Float4& rotation,const Float4& scale,const SineTable& sine);
Matrix4 camera_matrix(const Float4& position,const Float4& rotation,const Float4& scale,const SineTable& sine);
std::vector<Matrix4> compose_pose(const Matrix4& parent,std::span<const Matrix4> pose);
std::vector<Matrix4> rest_pose(const Model& model, const SineTable& sine);
// XMT rotations already use turns. Four channels precede the bone channels.
std::vector<Matrix4> animated_pose(const Model& model, std::span<const Float4> channels,
                                 const SineTable& sine, bool scale_channels=false);
std::vector<Float4> posed_positions(const RenderBlock& block, std::span<const Matrix4> pose);
}
