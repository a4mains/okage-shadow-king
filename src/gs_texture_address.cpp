#include "okage/gs_texture_address.hpp"

namespace okage {
namespace {
constexpr std::uint32_t gs_address_mask = 0x3fffff;

std::uint32_t spread3(std::uint32_t value) noexcept {
    value &= 7;
    return (value & 1) | ((value & 2) << 1) | ((value & 4) << 2);
}

std::uint32_t spread_column_x_low(std::uint32_t value) noexcept {
    value &= 7;
    return (value & 1u) | ((value & 2u) << 1u);
}

std::uint32_t block_number(std::uint32_t bx, std::uint32_t by) noexcept {
    return spread3(bx) | ((by & 1) << 1) | ((by & 2) << 2);
}

std::uint32_t page_base(std::uint32_t tbp, std::uint32_t page) noexcept {
    return (tbp * 256u + page * 8192u) & gs_address_mask;
}

std::uint32_t address32(std::uint32_t tbp, std::uint32_t tbw,
                        std::uint32_t x, std::uint32_t y) noexcept {
    // Each page is 64x32 and 8 KiB. Keep the page-row multiplication before
    // the addition so the calculation follows the register's pixel stride.
    const auto page = x / 64u + (y / 32u) * tbw;
    const auto px = x % 64u;
    const auto py = y % 32u;

    const auto block = block_number(px / 8u, py / 8u);
    const auto ix = px % 8u;
    const auto iy = py % 8u;

    const auto column_word = spread_column_x_low(ix) | ((iy & 1u) << 1) | ((ix & 4u) << 1u);
    const auto band = (iy / 2u) * 64u;
    return (page_base(tbp, page) + block * 256u + band + column_word * 4u) & gs_address_mask;
}

std::uint32_t address8(std::uint32_t tbp, std::uint32_t tbw,
                       std::uint32_t x, std::uint32_t y) noexcept {
    const auto page = x / 128u + ((y / 64u) * (tbw * 64u)) / 128u;
    const auto px = x % 128u;
    const auto py = y % 64u;

    const auto block = block_number(px / 16u, py / 16u);
    const auto ix = px % 16u;
    const auto iy = py % 16u;

    const auto band = (iy / 4u) * 64u;
    const auto local_y = iy % 4u;
    const auto table_bit = ((local_y >> 1u) ^ (py >> 2u)) & 1u;
    const auto column_word = spread_column_x_low(ix) | ((local_y & 1u) << 1u) |
        ((table_bit ^ ((ix >> 2u) & 1u)) << 3u);
    const auto byte_lane = ((ix & 8u) >> 2u) | ((local_y & 2u) >> 1u);
    return (page_base(tbp, page) + block * 256u + band + column_word * 4u + byte_lane) & gs_address_mask;
}
} // namespace

std::optional<std::uint32_t> gs_texture_byte_address(
    std::uint32_t psm,
    std::uint32_t tbp,
    std::uint32_t tbw,
    std::uint32_t x,
    std::uint32_t y) noexcept {
    if (tbp > 0x3fffu || tbw > 0x3fu || x > 2047u || y > 2047u) return std::nullopt;
    if (psm == static_cast<std::uint32_t>(GsTexturePsm::Psmct32))
        return address32(tbp, tbw, x, y);
    if (psm == static_cast<std::uint32_t>(GsTexturePsm::Psmt8))
        return address8(tbp, tbw, x, y);
    return std::nullopt;
}
} // namespace okage
