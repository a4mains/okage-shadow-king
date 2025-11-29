#pragma once
#include "okage/texture_memory.hpp"
#include <array>
#include <cstdint>

namespace okage {
struct GsTextureRegisters { std::uint64_t tex0, tex1, clamp, texa; };
struct GsTextureSample { std::array<float, 4> rgba; };

GsTextureSample sample_gs_texture(const Ps2TextureSnapshot&,
                                  const GsTextureRegisters&, float u, float v);
} // namespace okage
