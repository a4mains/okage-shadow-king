#pragma once

namespace okage {
inline constexpr char gs_texture_sampling_metal_source[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct GsTextureRegistersMetal { ulong tex0, tex1, clamp, texa; };
struct GsTexelAddressMetal { uint address, shift, bits; };
inline float4 gs_sample_error(device atomic_uint* invalid, uint code) {
    atomic_fetch_min_explicit(invalid, code, memory_order_relaxed);
    return float4(as_type<float>(0x7fc00000u));
}
inline uint gs_spread3(uint x) { return (x & 1u) | ((x & 2u) << 1u) | ((x & 4u) << 2u); }
inline uint gs_xlow(uint x) { return (x & 1u) | ((x & 2u) << 1u); }
inline uint gs_block(uint x, uint y) { return gs_spread3(x) | ((y & 1u) << 1u) | ((y & 2u) << 2u); }
inline uint gs_address32(uint tbp, uint tbw, uint x, uint y) {
    uint page = x / 64u + (y / 32u) * tbw;
    uint px = x % 64u, py = y % 32u, ix = px % 8u, iy = py % 8u;
    uint word = gs_xlow(ix) | ((iy & 1u) << 1u) | ((ix & 4u) << 1u);
    return (tbp * 256u + page * 8192u + gs_block(px / 8u, py / 8u) * 256u +
            (iy / 2u) * 64u + word * 4u) & 0x3fffffu;
}
inline GsTexelAddressMetal gs_raw_texel_address(uint psm, uint tbp, uint tbw, uint x, uint y) {
    if (psm == 19u) {
        uint page = x / 128u + ((y / 64u) * (tbw * 64u)) / 128u;
        uint px = x % 128u, py = y % 64u, ix = px % 16u, iy = py % 16u, ly = iy % 4u;
        uint table_bit = ((ly >> 1u) ^ (py >> 2u)) & 1u;
        uint word = gs_xlow(ix) | ((ly & 1u) << 1u) | ((table_bit ^ ((ix >> 2u) & 1u)) << 3u);
        uint lane = ((ix & 8u) >> 2u) | ((ly & 2u) >> 1u);
        return {(tbp * 256u + page * 8192u + gs_block(px / 16u, py / 16u) * 256u +
                 (iy / 4u) * 64u + word * 4u + lane) & 0x3fffffu, 0u, 8u};
    }
    if (psm == 20u) {
        uint page = x / 128u + ((y / 128u) * (tbw * 64u)) / 128u;
        uint px = x % 128u, py = y % 128u, bx = px % 32u, by = py % 16u;
        uint block = gs_spread3(py / 16u) | (((px / 32u) & 1u) << 1u) | (((px / 32u) & 2u) << 2u);
        uint column = by / 4u, cy = by % 4u, shift = (bx & 0x18u) + ((cy & 2u) << 1u);
        uint sub = ((cy >> 1u) ^ column) & 1u, ix = bx & 7u, iy = cy & 1u;
        uint word = (ix & 1u) | (iy << 1u) | ((ix & 2u) << 1u) | (((ix >> 2u) ^ sub) << 3u);
        return {(tbp * 256u + page * 8192u + block * 256u + column * 64u + word * 4u + shift / 8u) &
                0x3fffffu, shift & 4u, 4u};
    }
    uint address = gs_address32(tbp, tbw, x, y);
    if (psm == 0u || psm == 1u) return {address, 0u, psm == 0u ? 32u : 24u};
    return {address + 3u, psm == 44u ? 4u : 0u, psm == 27u ? 8u : 4u};
}
inline bool gs_read_texel(device const uchar* bytes, device const uchar* known,
                          GsTexelAddressMetal address, bool clut, device atomic_uint* invalid,
                          thread uint& result) {
    result = 0u;
    for (uint bit = 0u; bit < address.bits; bit += 4u) {
        uint offset = address.shift + bit;
        uint a = (address.address + offset / 8u) & 0x3fffffu, nibble = (offset / 4u) & 1u;
        if ((known[a] & (1u << nibble)) == 0u) {
            atomic_fetch_min_explicit(invalid, (a << 1u) | nibble | (clut ? 0x800000u : 0u), memory_order_relaxed);
            return false;
        }
        result |= uint((bytes[a] >> (nibble * 4u)) & 15u) << bit;
    }
    return true;
}
inline uint gs_region_coordinate(int value, uint mode, uint lo, uint hi) {
    return mode == 2u ? uint(clamp(value, int(lo), int(hi))) : (uint(value) & lo) | hi;
}
inline float4 gs_sample_texture(device const uchar* bytes, device const uchar* known,
                               constant GsTextureRegistersMetal& regs, float2 uv,
                               device atomic_uint* invalid) {
    uint psm = uint((regs.tex0 >> 20u) & 63ul), tw = uint((regs.tex0 >> 26u) & 15ul),
         th = uint((regs.tex0 >> 30u) & 15ul);
    bool indexed = psm >= 16u, linear = regs.tex1 == 0x60ul;
    uint um = uint(regs.clamp & 3ul), vm = uint((regs.clamp >> 2u) & 3ul);
    uint umin = uint((regs.clamp >> 4u) & 1023ul), umax = uint((regs.clamp >> 14u) & 1023ul),
         vmin = uint((regs.clamp >> 24u) & 1023ul), vmax = uint((regs.clamp >> 34u) & 1023ul);
    if ((regs.tex1 != 0ul && !linear) || ((regs.tex0 >> 34u) & 1ul) != 1ul ||
        ((regs.tex0 >> 35u) & 3ul) != 0ul || tw > 10u || th > 10u ||
        (psm != 0u && psm != 1u && psm != 19u && psm != 20u && psm != 27u && psm != 36u && psm != 44u) ||
        (indexed && (((regs.tex0 >> 51u) & 15ul) != 0ul || ((regs.tex0 >> 55u) & 1ul) != 0ul ||
                     ((regs.tex0 >> 56u) & 31ul) != 0ul || ((regs.tex0 >> 61u) & 7ul) != 1ul)) ||
        (psm == 1u && (regs.texa & ~(255ul | (1ul << 15u) | (255ul << 32u))) != 0ul) ||
        (regs.clamp >> 44u) != 0ul || (um != 2u && um != 3u) || (vm != 2u && vm != 3u) ||
        (um == 2u && umin > umax) || (vm == 2u && vmin > vmax))
        return gs_sample_error(invalid, 0xfffffffeu);
    float2 position = uv * float2(float(1u << tw), float(1u << th)) - (linear ? .5f : 0.f);
    if (!all(isfinite(position)) || any(position <= -2147483648.f) || any(position >= 2147483648.f))
        return gs_sample_error(invalid, 0xfffffffdu);
    float2 base = floor(position), fraction = position - base;
    int2 origin = int2(base);
    float4 result = float4(0.f);
    for (uint tap = 0u; tap < (linear ? 4u : 1u); ++tap) {
        uint dx = tap & 1u, dy = tap >> 1u;
        float weight = linear ? (dx ? fraction.x : 1.f - fraction.x) * (dy ? fraction.y : 1.f - fraction.y) : 1.f;
        if (weight == 0.f) continue;
        uint x = gs_region_coordinate(origin.x + int(dx), um, umin, umax);
        uint y = gs_region_coordinate(origin.y + int(dy), vm, vmin, vmax);
        GsTexelAddressMetal address = gs_raw_texel_address(psm, uint(regs.tex0 & 0x3ffful),
                                                        uint((regs.tex0 >> 14u) & 63ul), x, y);
        uint pixel;
        if (!gs_read_texel(bytes, known, address, false, invalid, pixel)) return float4(as_type<float>(0x7fc00000u));
        if (indexed) {
            uint word = ((pixel >> 5u) * 2u + ((pixel >> 3u) & 1u)) * 16u +
                        ((pixel >> 4u) & 1u) * 8u + (pixel & 7u);
            address = gs_raw_texel_address(0u, uint((regs.tex0 >> 37u) & 0x3ffful), 1u, word % 16u, word / 16u);
            if (!gs_read_texel(bytes, known, address, true, invalid, pixel)) return float4(as_type<float>(0x7fc00000u));
        }
        float4 rgba = float4(float(pixel & 255u), float((pixel >> 8u) & 255u),
                            float((pixel >> 16u) & 255u), float(pixel >> 24u));
        if (psm == 1u) rgba.a = ((regs.texa >> 15u) & 1ul) != 0ul && (pixel & 0xffffffu) == 0u ? 0.f : float(regs.texa & 255ul);
        result += rgba * weight;
    }
    return result;
}
)MSL";
} // namespace okage
