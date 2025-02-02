#include "okage/assets.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <set>
#include <stdexcept>

namespace okage {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
std::uint32_t le32(Bytes bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) fail("Truncated 32-bit field");
    return std::uint32_t(bytes[at]) | std::uint32_t(bytes[at+1]) << 8 |
           std::uint32_t(bytes[at+2]) << 16 | std::uint32_t(bytes[at+3]) << 24;
}
struct Stream {
    Bytes data;
    std::size_t at = 4;
    unsigned mask = 0, control = 0;
    unsigned byte() {
        if (at == data.size()) fail("Truncated XPF compressed stream");
        return data[at++];
    }
    unsigned bit() {
        if (!mask) { control = byte(); mask = 0x80; }
        unsigned value = (control & mask) != 0;
        mask >>= 1;
        return value;
    }
};
}

std::vector<XpfEntry> read_xpf(Bytes bytes) {
    if (bytes.size() < 16 || le32(bytes, 0) != 0x58465058)
        fail("Not an XPFX archive");
    const std::size_t base = le32(bytes, 4), count = le32(bytes, 8);
    if (count > (bytes.size() - 16) / 32 || base != 16 + count * 32 || le32(bytes, 12))
        fail("Unsupported or malformed XPF directory");
    std::vector<XpfEntry> entries;
    std::set<std::string> names;
    std::size_t end = base;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = 16 + i * 32;
        std::string name;
        for (std::size_t j = 0; j < 24 && bytes[at+j]; ++j) {
            auto c = bytes[at+j];
            if (c < 32 || c >= 127 || c == '/' || c == '\\' || c == ':')
                fail("Unsafe XPF member name");
            name.push_back(static_cast<char>(c));
        }
        if (name.empty() || name == "." || name == "..") fail("Invalid XPF member name");
        auto key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        });
        if (!names.insert(key).second) fail("Duplicate XPF member name");
        const std::size_t relative = le32(bytes, at + 24), size = le32(bytes, at + 28);
        if (relative > bytes.size() - base || size > bytes.size() - base - relative)
            fail("XPF member outside archive");
        if (base + relative != end) fail("Non-contiguous XPF member layout");
        entries.push_back({name, base + relative, size});
        end += size;
    }
    if (end != bytes.size()) fail("Unaccounted bytes at end of XPF archive");
    return entries;
}

Decoded decompress_xpf(Bytes packed, std::size_t output_limit) {
    if (packed.size() < 6) fail("Missing XPF compressed header or end marker");
    // Retail helper 0x001bc9e8 reads BE32; the decoder's return value is BE24.
    // The observed corpus has a zero leading byte. Reject unverified high flags.
    if (packed[0]) fail("Unsupported XPF decompressed-size high byte");
    const std::size_t expected = std::size_t(packed[1]) << 16 |
                                 std::size_t(packed[2]) << 8 | packed[3];
    if (expected > output_limit) fail("XPF output exceeds configured limit");
    Decoded result;
    result.bytes.reserve(expected);
    Stream stream{packed};
    for (;;) {
        if (!stream.bit()) {
            if (result.bytes.size() == expected) fail("Literal exceeds declared XPF size");
            result.bytes.push_back(static_cast<std::uint8_t>(stream.byte()));
            continue;
        }
        bool wide = stream.bit();
        int offset = static_cast<int>(stream.byte());
        if (!wide && !offset) {
            if (result.bytes.size() != expected) fail("XPF end marker before declared size");
            result.consumed = stream.at;
            return result;
        }
        offset -= 256;
        if (wide) {
            for (int i = 0; i < 4; ++i) offset = offset * 2 + static_cast<int>(stream.bit());
            offset -= 255;
        }
        std::size_t length = 1;
        while (stream.bit()) {
            if (length > expected / 2) fail("XPF match length exceeds declared size");
            length = length * 2 + stream.bit();
        }
        ++length;
        const auto distance = static_cast<std::size_t>(-offset);
        if (distance > result.bytes.size()) fail("XPF match precedes output start");
        if (length > expected - result.bytes.size()) fail("XPF match exceeds declared size");
        // Overlapping matches repeat their newly written bytes.
        while (length--) result.bytes.push_back(result.bytes[result.bytes.size() - distance]);
    }
}

static Texture decode_xim_impl(Bytes data, Alpha16 alpha16, bool raw_alpha) {
    if (data.size() < 32) fail("Truncated XIM header");
    const auto psm = (le32(data, 0) >> 20) & 63;
    const auto cpsm = (le32(data, 4) >> 19) & 15;
    const bool indexed = psm >= 16;
    if(raw_alpha && !(psm==0 || ((psm==19||psm==20||psm==27)&&cpsm==0)))
        fail("Raw XIM alpha requires PSMCT32 pixels or palette");
    unsigned bits;
    switch (psm) {
        case 0: bits = 32; break;
        case 1: bits = 24; break;
        case 19: bits = 8; break;
        case 27: bits = 8; break;
        case 20: bits = 4; break;
        default: fail("Unsupported XIM pixel format");
    }
    std::size_t block = 16, colors = 0;
    if (indexed) {
        const auto palette_bytes = le32(data, 16);
        const auto palette_count = le32(data, 24);
        colors = le32(data, 28);
        if (palette_count != 1 || (colors != 16 && colors != 256))
            fail("Unsupported XIM palette count or size");
        if (cpsm != 0 && cpsm != 2) fail("Unsupported XIM palette format");
        if (palette_bytes != 16 + colors * (cpsm == 2 ? 2 : 4))
            fail("Invalid XIM palette block extent");
        block += palette_bytes;
    }
    if (block > data.size() || data.size() - block < 16) fail("Missing XIM pixel block");
    // Retail 0x0018f6a8 reads height at +8 and width at +12. The +0 field
    // is not reliably a length: 766 members in the corpus carry stale values.
    const auto height = le32(data, block + 8), width = le32(data, block + 12);
    if (!width || !height || width > 4096 || height > 4096) fail("Invalid XIM dimensions");
    const std::size_t pixels = std::size_t(width) * height;
    const auto payload = block + 16;
    if (data.size() - payload != (pixels * bits + 7) / 8)
        fail("XIM dimensions do not match pixel payload");
    Texture result{width, height, psm, cpsm, {}};
    result.rgba.reserve(pixels * 4);
    auto alpha = [raw_alpha](unsigned value) { return static_cast<std::uint8_t>(raw_alpha?value:std::min(255u, value * 2)); };
    for (std::size_t i = 0; i < pixels; ++i) {
        std::array<std::uint8_t, 4> color{};
        if (!indexed) {
            const auto at = payload + i * (bits / 8);
            color = {data[at], data[at+1], data[at+2], bits == 32 ? alpha(data[at+3]) : std::uint8_t(255)};
        } else {
            unsigned index = bits == 8 ? data[payload+i] : (data[payload+i/2] >> ((i % 2)*4)) & 15;
            if (index >= colors) fail("XIM palette index out of range");
            const auto at = 32 + index * (cpsm == 2 ? 2 : 4);
            if (cpsm == 0) {
                color = {data[at], data[at+1], data[at+2], alpha(data[at+3])};
            } else {
                const auto value = unsigned(data[at]) | unsigned(data[at+1]) << 8;
                color[0] = (value & 31) << 3;
                color[1] = ((value >> 5) & 31) << 3;
                color[2] = ((value >> 10) & 31) << 3;
                bool opaque = (color[0] | color[1] | color[2]) != 0;
                if (alpha16 == Alpha16::UseBit) opaque = (value & 0x8000) != 0;
                if (alpha16 == Alpha16::InvertBit) opaque = (value & 0x8000) == 0;
                color[3] = opaque ? 255 : 0;
            }
        }
        result.rgba.insert(result.rgba.end(), color.begin(), color.end());
    }
    return result;
}
Texture decode_xim(Bytes data, Alpha16 alpha16) {return decode_xim_impl(data,alpha16,false);}
Texture decode_xim_gs32(Bytes data) {return decode_xim_impl(data,Alpha16::BlackTransparent,true);}

namespace {
std::size_t checked_add(std::size_t a, std::size_t b, const char* what) {
    if (a > SIZE_MAX - b) throw std::runtime_error(std::string("XIM size arithmetic overflow: ") + what);
    return a + b;
}
std::size_t checked_mul(std::size_t a, std::size_t b, const char* what) {
    if (a && b > SIZE_MAX / a) throw std::runtime_error(std::string("XIM size arithmetic overflow: ") + what);
    return a * b;
}
Bytes xim_slice(Bytes data, std::size_t offset, std::size_t size) {
    if (offset > data.size() || size > data.size() - offset) fail("XIM span outside buffer");
    return data.subspan(offset, size);
}
}
RawXimLayout parse_xim_layout(Bytes data) {
    RawXimLayout l;
    if (data.size() < 16) fail("XIM truncated: main header");
    for (unsigned i = 0; i < 4; ++i) l.header_words[i] = le32(data, i * 4);
    l.pixel_format = (l.header_words[0] >> 20) & 63;
    l.palette_format = (l.header_words[1] >> 19) & 15;
    switch (l.pixel_format) {
        case 0: l.bits_per_pixel = 32; break;
        case 1: l.bits_per_pixel = 24; break;
        case 19: case 27: l.bits_per_pixel = 8; break;
        case 20: l.bits_per_pixel = 4; break;
        default: fail("XIM unsupported pixel format");
    }
    l.indexed = l.pixel_format >= 16;
    l.image_header_offset = 16;
    if (l.indexed) {
        if (data.size() < 32) fail("XIM truncated: palette header");
        l.palette_header_offset = 16;
        l.palette_data_offset = 32;
        l.palette_block_bytes = le32(data, 16);
        l.palette_unused_word = le32(data, 20);
        l.palette_level_count = le32(data, 24);
        l.palette_color_count = le32(data, 28);
        if (l.palette_format != 0 && l.palette_format != 2) fail("XIM unsupported palette format");
        if (!l.palette_level_count) fail("XIM palette level count is zero");
        if (!l.palette_color_count || l.palette_color_count > 256)
            fail("XIM palette color count is zero or exceeds 256");
        if (l.palette_block_bytes < 16) fail("XIM palette block smaller than its header");
        l.palette_level_stride = checked_mul(l.palette_color_count, l.palette_format == 2 ? 2 : 4, "palette level stride");
        l.palette_levels_extent = checked_mul(l.palette_level_count, l.palette_level_stride, "palette level count * stride");
        if (l.palette_block_bytes - 16 != l.palette_levels_extent)
            fail("XIM palette block extent does not match level count * stride");
        const auto palette_end = checked_add(l.palette_data_offset, l.palette_levels_extent, "palette end");
        if (palette_end > data.size()) fail("XIM palette extent outside buffer");
        l.image_header_offset = checked_add(16, l.palette_block_bytes, "image header offset");
    }
    const auto header_end = checked_add(l.image_header_offset, 16, "image header end");
    if (header_end > data.size()) fail("XIM truncated: image header");
    for (unsigned i = 0; i < 4; ++i) l.image_header_words[i] = le32(data, l.image_header_offset + i * 4);
    l.height = l.image_header_words[2];
    l.width = l.image_header_words[3];
    if (!l.width || !l.height) fail("XIM zero image dimension");
    if (l.width > 4096 || l.height > 4096) fail("XIM image dimension exceeds 4096");
    l.image_payload_offset = header_end;
    l.image_payload_size = data.size() - header_end;
    const auto bits = checked_mul(checked_mul(l.width, l.height, "pixel count"), l.bits_per_pixel, "bit count");
    if (l.image_payload_size != checked_add(bits, 7, "payload rounding") / 8)
        fail("XIM payload size does not match dimensions");
    return l;
}
XimUploadSupport xim_upload_support(const RawXimLayout& l) {
    using S = XimUploadStatus;
    if (l.pixel_format == 0) return {S::Supported, "PSMCT32 direct image"};
    if (l.pixel_format != 19) return {S::UnsupportedImageFormat, "no recovered upload address helper for PSM " + std::to_string(l.pixel_format)};
    if (l.palette_format != 0) return {S::UnsupportedPaletteFormat, "no recovered 16->32 palette conversion evidence for CPSM " + std::to_string(l.palette_format)};
    if (l.palette_level_count < 1 || (l.palette_color_count != 16 && l.palette_color_count != 256))
        return {S::UnsupportedPaletteShape, "palette colors " + std::to_string(l.palette_color_count) + " not 16 or 256"};
    return {S::Supported, "PSMT8 with CPSM0 palette"};
}
const char* to_string(XimUploadStatus s) {
    switch (s) {
        case XimUploadStatus::Supported: return "Supported";
        case XimUploadStatus::UnsupportedImageFormat: return "UnsupportedImageFormat";
        case XimUploadStatus::UnsupportedPaletteFormat: return "UnsupportedPaletteFormat";
        case XimUploadStatus::UnsupportedPaletteShape: return "UnsupportedPaletteShape";
    }
    return "Unknown";
}
static Bytes xim_image_span(Bytes d, const RawXimLayout& l) { return xim_slice(d, l.image_payload_offset, l.image_payload_size); }
static Bytes xim_level_span(Bytes d, const RawXimLayout& l, std::size_t i) {
    if (i >= l.palette_level_count) fail("XIM palette level index out of range");
    return xim_slice(d, l.palette_data_offset + i * l.palette_level_stride, l.palette_level_stride);
}
Bytes RawXimView::image() const { return xim_image_span(data_, layout_); }
Bytes RawXimView::palette_level(std::size_t i) const { return xim_level_span(data_, layout_, i); }
OwnedRawXim::OwnedRawXim(Bytes data) : bytes_(data.begin(), data.end()), layout_(parse_xim_layout(data)) {}
OwnedRawXim::OwnedRawXim(OwnedRawXim&& o) noexcept : bytes_(std::move(o.bytes_)), layout_(o.layout_) { o.bytes_.clear(); o.layout_ = {}; }
OwnedRawXim& OwnedRawXim::operator=(OwnedRawXim&& o) noexcept {
    if (this != &o) { bytes_ = std::move(o.bytes_); layout_ = o.layout_; o.bytes_.clear(); o.layout_ = {}; }
    return *this;
}
Bytes OwnedRawXim::image() const { return xim_image_span(bytes_, layout_); }
Bytes OwnedRawXim::palette_level(std::size_t i) const { return xim_level_span(bytes_, layout_, i); }
}
