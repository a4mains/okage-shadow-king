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

constexpr std::uint8_t psmt4_block_table[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};

GsTexelAddress address4(std::uint32_t tbp, std::uint32_t tbw,
                        std::uint32_t x, std::uint32_t y) noexcept {
    const std::uint32_t page = x / 128u + ((y / 128u) * (tbw * 64u)) / 128u;
    const std::uint32_t px = x % 128u, py = y % 128u;
    const std::uint32_t block = psmt4_block_table[py / 16u][px / 32u];
    const std::uint32_t bx = px % 32u, by = py % 16u;
    const std::uint32_t column = by / 4u, cy = by % 4u;
    const std::uint32_t shift = (bx & 0x18u) + ((cy & 2u) << 1);  // 0..28, multiple of 4
    const std::uint32_t sub = ((cy >> 1) ^ column) & 1u;
    const std::uint32_t ix = bx & 7u, iy = cy & 1u;
    const std::uint32_t word = (ix & 1u) | (iy << 1) | ((ix & 2u) << 1) | (((ix >> 2) ^ sub) << 3);
    const std::uint32_t base = (tbp * 256u + page * 8192u) + block * 256u + column * 64u + word * 4u;
    return GsTexelAddress{(base + shift / 8u) & gs_address_mask, static_cast<std::uint8_t>(shift & 4u), 4};
}

} // namespace

std::optional<GsTexelAddress> gs_texel_address(
    std::uint32_t psm,
    std::uint32_t tbp,
    std::uint32_t tbw,
    std::uint32_t x,
    std::uint32_t y) noexcept {
    if (tbp > 0x3fffu || tbw > 0x3fu || x > 2047u || y > 2047u) return std::nullopt;
    switch (psm) {
        case 0: return GsTexelAddress{address32(tbp, tbw, x, y), 0, 32};
        case 1: return GsTexelAddress{address32(tbp, tbw, x, y), 0, 24};
        case 19: return GsTexelAddress{address8(tbp, tbw, x, y), 0, 8};
        case 20: return address4(tbp, tbw, x, y);
        // The high formats alias byte 3 of the aligned PSMCT32 word.
        case 27: return GsTexelAddress{address32(tbp, tbw, x, y) + 3u, 0, 8};
        case 36: return GsTexelAddress{address32(tbp, tbw, x, y) + 3u, 0, 4};
        case 44: return GsTexelAddress{address32(tbp, tbw, x, y) + 3u, 4, 4};
        default: return std::nullopt;
    }
}

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
