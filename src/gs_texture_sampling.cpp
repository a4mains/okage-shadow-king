#include "okage/gs_texture_sampling.hpp"
#include "okage/gs_texture_address.hpp"
#include "okage/texture_transfer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace okage {
namespace {
[[noreturn]] void unsupported(const char* why) {
    throw std::invalid_argument(std::string("GS texture sample: unsupported ") + why);
}
std::uint32_t read(const Ps2TextureSnapshot& memory, const GsTexelAddress& address, const char* role) {
    std::uint32_t result = 0;
    for (unsigned bit = 0; bit < address.bits; bit += 4) {
        const auto offset = address.shift + bit;
        const auto byte = (address.byte_address + offset / 8) & 0x3fffff;
        const auto nibble = (offset / 4) & 1;
        const auto observation = memory.observe_nibble(byte, nibble);
        if (!observation.known) {
            std::ostringstream message;
            message << "GS texture sample: unknown " << role << " nibble address=0x"
                    << std::hex << byte << " nibble=" << nibble;
            throw std::runtime_error(message.str());
        }
        result |= std::uint32_t(observation.value) << bit;
    }
    return result;
}
struct Axis { unsigned mode, min, max; };
unsigned coordinate(std::int32_t value, const Axis& axis) {
    if (axis.mode == 1 || axis.mode == 2) return std::clamp(value, std::int32_t(axis.min), std::int32_t(axis.max));
    return (std::uint32_t(value) & axis.min) | axis.max;
}
Axis axis(std::uint64_t clamp, unsigned mode_shift, unsigned min_shift, unsigned max_shift,unsigned exponent) {
    Axis result{unsigned((clamp >> mode_shift) & 3), unsigned((clamp >> min_shift) & 1023),
                unsigned((clamp >> max_shift) & 1023)};
    if (result.mode == 1) { result.min=0; result.max=(1u<<exponent)-1; }
    if (result.mode != 1 && result.mode != 2 && result.mode != 3) unsupported("CLAMP mode");
    if (result.mode == 2 && result.min > result.max) unsupported("CLAMP bounds");
    return result;
}
std::int32_t origin(float normalized, unsigned exponent, bool linear, float& fraction) {
    const float position = normalized * float(1u << exponent) - (linear ? .5f : 0.f);
    // Avoid undefined float-to-integer conversion and leave room for the +1 tap.
    if (!std::isfinite(position) || double(position) < double(std::numeric_limits<std::int32_t>::min()) + 1 ||
        double(position) > double(std::numeric_limits<std::int32_t>::max()) - 1)
        unsupported("ST coordinate range");
    const float floor = std::floor(position);
    fraction = linear ? (std::floor(position * 16.f) - floor * 16.f) / 16.f : position - floor;
    return static_cast<std::int32_t>(floor);
}
} // namespace
GsTextureSample sample_gs_texture(const Ps2TextureSnapshot& memory, const GsTextureRegisters& regs,
                                  float u, float v) {
    if (!memory.valid()) unsupported("empty snapshot");
    if (regs.tex1 != 0 && regs.tex1 != 0x60) unsupported("TEX1 filtering/LOD");
    if (((regs.tex0 >> 34) & 1) != 1 || ((regs.tex0 >> 35) & 3) != 0) unsupported("TCC/TFX");
    const unsigned tw = (regs.tex0 >> 26) & 15, th = (regs.tex0 >> 30) & 15;
    if (tw > 10 || th > 10) unsupported("TEX0 dimensions");
    const unsigned psm = (regs.tex0 >> 20) & 63;
    if (psm != 0 && psm != 1 && psm != 19 && psm != 20 && psm != 27 && psm != 36 && psm != 44)
        unsupported("PSM");
    const bool indexed = psm >= 16;
    if (indexed && (((regs.tex0 >> 51) & 15) != 0 || ((regs.tex0 >> 55) & 1) != 0 ||
                    ((regs.tex0 >> 56) & 31) != 0 || ((regs.tex0 >> 61) & 7) != 1))
        unsupported("CLUT CPSM/CSM/CSA/CLD");
    if (psm == 1 && (regs.texa & ~(std::uint64_t(255) | (std::uint64_t(1) << 15) | (std::uint64_t(255) << 32))))
        unsupported("TEXA fields");
    if (regs.clamp >> 44) unsupported("CLAMP reserved fields");
    const auto horizontal = axis(regs.clamp, 0, 4, 14,tw), vertical = axis(regs.clamp, 2, 24, 34,th);
    const bool linear = regs.tex1 == 0x60;
    float fx, fy;
    const auto x = origin(u, tw, linear, fx), y = origin(v, th, linear, fy);
    GsTextureSample result{};
    for (unsigned tap = 0; tap < (linear ? 4u : 1u); ++tap) {
        const unsigned dx = tap & 1, dy = tap >> 1;
        const float weight = linear ? (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) : 1;
        if (weight == 0) continue;
        const auto address = gs_texel_address(psm, regs.tex0 & 0x3fff, (regs.tex0 >> 14) & 63,
            coordinate(x + std::int32_t(dx), horizontal), coordinate(y + std::int32_t(dy), vertical));
        if (!address) unsupported("texel address");
        auto pixel = read(memory, *address, "texture");
        if (indexed) {
            const auto word = clut_scratch_word_index(pixel);
            const auto palette = gs_texel_address(0, (regs.tex0 >> 37) & 0x3fff, 1, word % 16, word / 16);
            pixel = read(memory, *palette, "CLUT");
        }
        std::array<float, 4> rgba{float(pixel & 255), float((pixel >> 8) & 255),
                                  float((pixel >> 16) & 255), float(pixel >> 24)};
        if (psm == 1) rgba[3] = ((regs.texa >> 15) & 1) && !(pixel & 0xffffff) ? 0 : float(regs.texa & 255);
        for (unsigned component = 0; component < 4; ++component) result.rgba[component] += rgba[component] * weight;
    }
    return result;
}
} // namespace okage
