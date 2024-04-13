#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace okage {
using Bytes = std::span<const std::uint8_t>;
struct XpfEntry {
    std::string name;
    std::size_t offset;
    std::size_t packed_size;
};
struct Decoded {
    std::vector<std::uint8_t> bytes;
    std::size_t consumed;
};
std::vector<XpfEntry> read_xpf(Bytes archive);
Decoded decompress_xpf(Bytes packed, std::size_t output_limit = 64 * 1024 * 1024);
enum class Alpha16 { BlackTransparent, UseBit, InvertBit };
struct Texture {
    std::uint32_t width, height, pixel_format, palette_format;
    std::vector<std::uint8_t> rgba;
};
// Preview RGBA8 output. PS2 alpha 0x80 maps to opaque. Direct 24-bit data is
// displayed opaque; game TEXA/material blending is a separate rendering concern.
Texture decode_xim(Bytes data, Alpha16 alpha16 = Alpha16::BlackTransparent);
Texture decode_xim_gs32(Bytes data);
}
