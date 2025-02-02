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

struct RawXimLayout {
    std::uint32_t pixel_format = 0;          // PSM: bits 20..25 of le32@0
    std::uint32_t palette_format = 0;        // CPSM: (le32@4 >> 19) & 15
    std::uint32_t width = 0, height = 0;     // image header +12 / +8 (NOT +0)
    bool indexed = false;                    // PSM >= 16
    std::uint32_t bits_per_pixel = 0;
    std::uint32_t palette_level_count = 0;   // le32@0x18 (0 if not indexed)
    std::uint32_t palette_color_count = 0;   // le32@0x1c (0 if not indexed)
    std::uint32_t palette_block_bytes = 0;   // le32@0x10: header(16)+levels
    std::size_t palette_header_offset = 0;   // 0x10 if indexed, else 0
    std::size_t palette_data_offset = 0;     // 0x20 if indexed, else 0
    std::size_t palette_level_stride = 0;    // bytes per level (colors*bytes/color)
    std::size_t palette_levels_extent = 0;   // level_count * stride
    std::size_t image_header_offset = 0;     // 0x10 + palette_block_bytes (indexed) else 0x10
    std::size_t image_payload_offset = 0;    // image_header_offset + 16
    std::size_t image_payload_size = 0;      // == ceil(width*height*bits/8)
    std::uint32_t header_words[4] = {};      // le32 at 0,4,8,12 verbatim
    std::uint32_t palette_unused_word = 0;   // le32@0x14 verbatim (indexed only)
    std::uint32_t image_header_words[4] = {};// le32 at image header +0,4,8,12 verbatim
};
RawXimLayout parse_xim_layout(Bytes data);

enum class XimUploadStatus { Supported, UnsupportedImageFormat, UnsupportedPaletteFormat, UnsupportedPaletteShape };
struct XimUploadSupport { XimUploadStatus status; std::string reason; };
XimUploadSupport xim_upload_support(const RawXimLayout& layout);
const char* to_string(XimUploadStatus status);

class RawXimView {
public:
    explicit RawXimView(Bytes data) : data_(data), layout_(parse_xim_layout(data)) {}
    Bytes bytes() const { return data_; }
    const RawXimLayout& layout() const { return layout_; }
    Bytes image() const;
    Bytes palette_level(std::size_t index) const;  // throws if index >= level count
private:
    Bytes data_;
    RawXimLayout layout_;
};
class OwnedRawXim {
public:
    explicit OwnedRawXim(Bytes data);
    OwnedRawXim(const OwnedRawXim&) = default;
    OwnedRawXim& operator=(const OwnedRawXim&) = default;
    OwnedRawXim(OwnedRawXim&& other) noexcept;
    OwnedRawXim& operator=(OwnedRawXim&& other) noexcept;
    Bytes bytes() const { return bytes_; }
    const RawXimLayout& layout() const { return layout_; }
    Bytes image() const;
    Bytes palette_level(std::size_t index) const;
private:
    std::vector<std::uint8_t> bytes_;
    RawXimLayout layout_;
};
}
