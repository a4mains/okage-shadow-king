#pragma once

#include <cstdint>
#include <optional>

namespace okage {

// Supported GS color formats for byte-address lookup. These are the raw PSM
// register values, not the XIM format names.
enum class GsTexturePsm : std::uint8_t {
    Psmct32 = 0,
    Psmt8 = 19,
};

struct GsTexelAddress {
    std::uint32_t byte_address;
    std::uint8_t shift;
    std::uint8_t bits;
};

std::optional<GsTexelAddress> gs_texel_address(
    std::uint32_t psm,
    std::uint32_t tbp,
    std::uint32_t tbw,
    std::uint32_t x,
    std::uint32_t y) noexcept;

std::optional<std::uint32_t> gs_texture_byte_address(
    std::uint32_t psm,
    std::uint32_t tbp,
    std::uint32_t tbw,
    std::uint32_t x,
    std::uint32_t y) noexcept;

} // namespace okage
