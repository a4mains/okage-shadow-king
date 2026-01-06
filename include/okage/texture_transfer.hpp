#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "okage/assets.hpp"
#include "okage/texture_memory.hpp"

namespace okage {

struct GsHostToLocal {
    std::uint32_t psm{};
    std::uint32_t tbp{};
    std::uint32_t tbw{};
    std::uint32_t dsax{};
    std::uint32_t dsay{};
    std::uint32_t rrw{};
    std::uint32_t rrh{};
};

struct GsTransferResult {
    std::size_t bytes_written{};
    std::uint32_t pixels_written{};
    std::uint8_t partial_pixel_bytes{};
};

GsTransferResult transfer_host_to_local(Ps2TextureMemory& memory,
                                        const GsHostToLocal& transfer,
                                        std::span<const std::uint8_t> data);

struct GsLocalToLocal {
    std::uint64_t source_descriptor{};
    std::uint64_t destination_descriptor{};
    std::uint32_t ssax{};
    std::uint32_t ssay{};
    std::uint32_t dsax{};
    std::uint32_t dsay{};
    std::uint32_t rrw{};
    std::uint32_t rrh{};
    std::uint32_t direction{}; // TRXPOS.DIR, not TRXDIR (always2 here).
};

GsTransferResult transfer_local_to_local(Ps2TextureMemory& memory,
                                         const GsLocalToLocal& transfer);

std::uint32_t retail_transfer_qwc(std::uint32_t psm, std::int32_t width, std::int32_t height);

GsTransferResult upload_retail_rect(Ps2TextureMemory& memory,
                                    const GsHostToLocal& rect,
                                    std::span<const std::uint8_t> source);

GsTransferResult upload_allocation_image(Ps2TextureMemory& memory,
                                         TextureAllocationId id,
                                         std::span<const std::uint8_t> source);

GsTransferResult upload_clut(Ps2TextureMemory& memory, TextureAllocationId id,
                             std::uint32_t entry_count,
                             std::span<const std::uint8_t> palette);

struct XimPaletteAttempt {
    std::uint32_t level{};
    // nullopt: allocation failed (retail null descriptor); no upload happened.
    std::optional<TextureAllocationId> palette;
    bool uploaded{};
};
struct XimUploadResult {
    TextureAllocationId image{};
    std::vector<XimPaletteAttempt> palette_attempts;
    bool all_palettes_allocated() const {
        for (const auto& a : palette_attempts) if (!a.palette) return false;
        return true;
    }
};
std::optional<XimUploadResult> upload_xim(Ps2TextureMemory& memory, const OwnedRawXim& xim);

// Pieces of the above, exposed for source-fidelity tests.
std::uint32_t clut_scratch_word_index(std::uint32_t entry);
std::uint32_t clut_rect_rows(std::uint32_t entry_count);
std::vector<std::uint8_t> clut_scratch(std::uint32_t entry_count,
                                       std::span<const std::uint8_t> palette);

} // namespace okage
