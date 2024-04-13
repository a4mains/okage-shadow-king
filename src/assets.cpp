#include "okage/assets.hpp"
#include <algorithm>
#include <array>
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
}
