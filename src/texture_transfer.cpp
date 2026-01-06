#include "okage/texture_transfer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "okage/gs_texture_address.hpp"

namespace okage {
namespace {

[[noreturn]] void reject(const std::string& why) {
    throw std::invalid_argument("GS transfer: " + why);
}

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    if (psm == 0) return 32;
    if (psm == 1) return 24;
    if (psm == 19 || psm == 0x1b) return 8;
    if (psm == 0x14 || psm == 0x24 || psm == 0x2c) return 4;
    reject("unsupported pixel storage format " + std::to_string(psm));
}

constexpr std::uint32_t clut_scratch_bytes = 2048;

} // namespace

GsTransferResult transfer_local_to_local(Ps2TextureMemory& memory,
                                         const GsLocalToLocal& t) {
    const auto source_psm = static_cast<std::uint32_t>((t.source_descriptor >> 32) & 63);
    const auto destination_psm = static_cast<std::uint32_t>((t.destination_descriptor >> 32) & 63);
    if (source_psm != 0 || destination_psm != 0)
        reject("local-to-local supports only PSM0 to PSM0");
    if (t.direction != 0) reject("local-to-local TRXPOS direction is not recovered");
    const auto source_tbp = static_cast<std::uint32_t>(t.source_descriptor & 0x3fff);
    const auto destination_tbp = static_cast<std::uint32_t>(t.destination_descriptor & 0x3fff);
    const auto source_tbw = static_cast<std::uint32_t>((t.source_descriptor >> 14) & 63);
    const auto destination_tbw = static_cast<std::uint32_t>((t.destination_descriptor >> 14) & 63);
    if (!source_tbw || !destination_tbw) reject("local-to-local zero buffer stride is not modeled");
    if (!t.rrw || !t.rrh) reject("empty local-to-local rectangle is not modeled");
    if (t.rrw > 2048 || t.rrh > 2048 || t.ssax > 2047 || t.ssay > 2047 ||
        t.dsax > 2047 || t.dsay > 2047 || t.rrw > 2048 - t.ssax ||
        t.rrh > 2048 - t.ssay || t.rrw > 2048 - t.dsax || t.rrh > 2048 - t.dsay)
        reject("local-to-local rectangle exceeds supported coordinates");

    const auto pixels = t.rrw * t.rrh; // bounded above by2048*2048
    struct Pixel { std::uint32_t source, destination; };
    std::vector<Pixel> addresses(pixels);
    std::vector<bool> source_words(Ps2TextureMemory::vram_bytes / 4);
    for (std::uint32_t i = 0; i < pixels; ++i) {
        const auto x = i % t.rrw, y = i / t.rrw;
        const auto source = gs_texture_byte_address(0, source_tbp, source_tbw, t.ssax + x, t.ssay + y);
        const auto destination = gs_texture_byte_address(0, destination_tbp, destination_tbw, t.dsax + x, t.dsay + y);
        if (!source || !destination || *source > Ps2TextureMemory::vram_bytes - 4 ||
            *destination > Ps2TextureMemory::vram_bytes - 4 || (*source & 3) || (*destination & 3))
            reject("invalid local-to-local physical word address");
        addresses[i] = {*source, *destination};
        source_words[*source / 4] = true;
    }
    for (const auto& pixel : addresses)
        if (source_words[pixel.destination / 4])
            reject("overlapping local-to-local transfer ordering is not recovered");

    const auto source = memory.snapshot();
    const auto bytes = source.physical_bytes(), masks = source.known_nibbles();
    for (const auto& pixel : addresses)
        memory.write_masked_bytes(pixel.destination, bytes.subspan(pixel.source, 4),
                                   masks.subspan(pixel.source, 4));
    return {std::size_t(pixels) * 4, pixels};
}

GsTransferResult transfer_host_to_local(Ps2TextureMemory& memory,
                                        const GsHostToLocal& t,
                                        std::span<const std::uint8_t> data) {
    const auto bits = bits_per_pixel(t.psm);
    if (t.tbp > 0x3fff) reject("TBP exceeds 14 bits");
    if (t.tbw > 63) reject("TBW exceeds 6 bits");
    if (!t.rrw || !t.rrh) reject("empty transfer rectangle");
    if (t.rrw > 2048 || t.rrh > 2048) reject("transfer rectangle exceeds 2048");
    if (t.dsax > 2047 || t.dsay > 2047) reject("destination origin exceeds 2047");
    if (data.size() % 16) reject("payload is not a whole number of quadwords");
    const std::uint64_t rect_pixels = std::uint64_t(t.rrw) * t.rrh;
    // PSMCT24: a final pixel with only 1..2 payload bytes still counts (see below).
    const std::uint64_t pixels64 = (std::uint64_t(data.size()) * 8 + bits - 1) / bits;
    if (pixels64 > rect_pixels) reject("payload exceeds the transfer rectangle");
    const auto pixels = static_cast<std::uint32_t>(pixels64);

    if (bits == 4) {
        std::vector<GsTexelAddress> slots(pixels);
        for (std::uint32_t i = 0; i < pixels; ++i) {
            const std::uint32_t x = t.dsax + i % t.rrw, y = t.dsay + i / t.rrw;
            const auto slot = gs_texel_address(t.psm, t.tbp, t.tbw, x, y);
            if (!slot) reject("pixel address outside the supported coordinate range");
            slots[i] = *slot;
        }
        for (std::uint32_t i = 0; i < pixels; ++i)
            memory.write_nibble(slots[i].byte_address, slots[i].shift / 4u,
                                static_cast<std::uint8_t>(data[i / 2] >> ((i & 1u) * 4u)));
        return {data.size(), pixels};
    }

    if (t.psm == 0x1b) {
        std::vector<std::uint32_t> high(pixels);
        for (std::uint32_t i = 0; i < pixels; ++i) {
            const auto address = gs_texel_address(t.psm, t.tbp, t.tbw, t.dsax + i % t.rrw, t.dsay + i / t.rrw);
            if (!address) reject("pixel address outside the supported coordinate range");
            high[i] = address->byte_address;
        }
        for (std::uint32_t i = 0; i < pixels; ++i) memory.write_bytes(high[i], data.subspan(i, 1));
        return {data.size(), pixels};
    }

    const std::uint32_t bpp = bits / 8;
    // Resolve every address first so a rejected pixel leaves memory untouched.
    std::vector<std::uint32_t> addresses(pixels);
    for (std::uint32_t i = 0; i < pixels; ++i) {
        const auto address = gs_texel_address(
            t.psm, t.tbp, t.tbw, t.dsax + i % t.rrw, t.dsay + i / t.rrw);
        if (!address) reject("pixel address outside the supported coordinate range");
        addresses[i] = address->byte_address;
    }
    std::uint8_t partial = 0;
    for (std::uint32_t i = 0; i < pixels; ++i) {
        const auto pixel = data.subspan(std::size_t(i) * bpp);
        if (pixel.size() >= bpp) {
            memory.write_bytes(addresses[i], pixel.first(bpp));
        } else {
            std::uint8_t padded[3] = {};
            std::copy(pixel.begin(), pixel.end(), padded);
            memory.write_bytes(addresses[i], padded);
            partial = static_cast<std::uint8_t>(pixel.size());
        }
    }
    return {data.size(), pixels, partial};
}

std::uint32_t retail_transfer_qwc(std::uint32_t psm, std::int32_t width, std::int32_t height) {
    if (width < 0 || height < 0) reject("negative dimension");
    // Unsigned 64-bit: n <= (2^31-1)^2 < 2^62, so n*3 < 2^64 cannot wrap.
    const std::uint64_t n = std::uint64_t(static_cast<std::uint32_t>(width)) *
                            static_cast<std::uint32_t>(height);
    std::uint64_t qwc = 0;
    switch (psm) {
        case 0x00: case 0x30: qwc = n >> 2; break;
        case 0x01: case 0x31: qwc = (n * 3) >> 4; break;
        case 0x02: case 0x0a: case 0x32: case 0x3a: qwc = n >> 3; break;
        case 0x13: case 0x1b: qwc = n >> 4; break;
        case 0x14: case 0x24: case 0x2c: qwc = n >> 5; break;
        default: return 0;
    }
    if (qwc > 0xffffffffull) reject("quadword count does not fit the 32-bit count");
    return static_cast<std::uint32_t>(qwc);
}

GsTransferResult upload_retail_rect(Ps2TextureMemory& memory, const GsHostToLocal& rect,
                                    std::span<const std::uint8_t> source) {
    bits_per_pixel(rect.psm);
    if (rect.rrw > 0x7fffffff || rect.rrh > 0x7fffffff) reject("dimension exceeds int");
    const auto qwc = retail_transfer_qwc(rect.psm, static_cast<std::int32_t>(rect.rrw),
                                         static_cast<std::int32_t>(rect.rrh));
    const std::uint64_t bytes = std::uint64_t(qwc) * 16;
    if (!qwc) reject("retail quadword count is zero");
    if (source.size() < bytes) reject("source holds fewer bytes than the quadwords sent");
    return transfer_host_to_local(memory, rect, source.first(bytes));
}

GsTransferResult upload_allocation_image(Ps2TextureMemory& memory, TextureAllocationId id,
                                         std::span<const std::uint8_t> source) {
    const auto alloc = memory.allocation(id);
    if (!alloc) reject("allocation " + std::to_string(id) + " is not live");
    GsHostToLocal rect;
    rect.psm = static_cast<std::uint32_t>(alloc->psm);
    rect.tbp = alloc->base_block;
    rect.tbw = alloc->buffer_width_64;
    rect.rrw = alloc->width;
    rect.rrh = alloc->height;
    return upload_retail_rect(memory, rect, source);
}

std::uint32_t clut_scratch_word_index(std::uint32_t i) {
    return ((i >> 5) * 2 + ((i >> 3) & 1)) * 16 + ((i >> 4) & 1) * 8 + (i & 7);
}

std::uint32_t clut_rect_rows(std::uint32_t n) { return ((n + 31) >> 5) << 1; }

std::vector<std::uint8_t> clut_scratch(std::uint32_t n, std::span<const std::uint8_t> palette) {
    if (n < 1 || n > clut_scratch_bytes / 4) reject("CLUT entry count outside 1..512");
    if (palette.size() < std::size_t(n) * 4) reject("palette holds fewer than entry_count words");
    std::vector<std::uint8_t> scratch(clut_scratch_bytes, 0);
    for (std::uint32_t i = 0; i < n; ++i)
        std::copy_n(palette.begin() + std::size_t(i) * 4, 4,
                    scratch.begin() + std::size_t(clut_scratch_word_index(i)) * 4);
    return scratch;
}

GsTransferResult upload_clut(Ps2TextureMemory& memory, TextureAllocationId id,
                             std::uint32_t n, std::span<const std::uint8_t> palette) {
    const auto alloc = memory.allocation(id);
    if (!alloc) reject("allocation " + std::to_string(id) + " is not live");
    const auto rows = clut_rect_rows(n);
    if (alloc->psm != GsPixelStorage::Psmct32 || alloc->width != 16 || alloc->height < rows)
        reject("CLUT allocation must be PSM0, 16 pixels wide and at least as tall as the rectangle");
    const auto scratch = clut_scratch(n, palette);
    GsHostToLocal rect;
    rect.psm = 0;
    rect.tbp = alloc->base_block;
    rect.tbw = alloc->buffer_width_64;
    rect.rrw = 16;
    rect.rrh = rows;
    // Retail goes through the same floor-QWC wrapper: 16*rows pixels = 4*rows quadwords.
    return upload_retail_rect(memory, rect,
                              std::span<const std::uint8_t>(scratch).first(std::size_t(rows) * 64));
}

std::optional<XimUploadResult> upload_xim(Ps2TextureMemory& memory, const OwnedRawXim& xim) {
    const auto& l = xim.layout();
    const auto support = xim_upload_support(l);
    if (support.status != XimUploadStatus::Supported)
        reject("XIM upload unsupported: " + support.reason);
    if (l.width > 2048 || l.height > 2048) reject("XIM dimension exceeds the 2048-pixel transfer limit");
    if (!retail_transfer_qwc(l.pixel_format, static_cast<std::int32_t>(l.width),
                             static_cast<std::int32_t>(l.height)))
        reject("XIM image is smaller than one quadword; retail sends no image data");
    XimUploadResult result;
    const auto image = memory.allocate(1, static_cast<GsPixelStorage>(l.pixel_format),
                                       static_cast<std::uint16_t>(l.width),
                                       static_cast<std::uint16_t>(l.height));
    if (!image) return std::nullopt;
    result.image = image->id;
    upload_allocation_image(memory, image->id, xim.image());
    // 18f6a8 reads the palette block whenever the header PSM is >= 0x10 (19, 20
    // and the 11fb10/18f500 override 27 alike).
    const bool indexed = l.indexed;
    for (std::uint32_t i = 0; indexed && i < l.palette_level_count; ++i) {
        XimPaletteAttempt attempt{i, std::nullopt, false};
        const auto palette = memory.allocate(0, GsPixelStorage::Psmct32, 16,
                                             static_cast<std::uint16_t>(clut_rect_rows(l.palette_color_count)));
        if (palette) {
            attempt.palette = palette->id;
            upload_clut(memory, palette->id, l.palette_color_count, xim.palette_level(i));
            attempt.uploaded = true;
        }
        result.palette_attempts.push_back(attempt);
    }
    return result;
}

} // namespace okage
