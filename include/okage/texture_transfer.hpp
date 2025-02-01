#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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
};

GsTransferResult transfer_host_to_local(Ps2TextureMemory& memory,
                                        const GsHostToLocal& transfer,
                                        std::span<const std::uint8_t> data);

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

// Pieces of the above, exposed for source-fidelity tests.
std::uint32_t clut_scratch_word_index(std::uint32_t entry);
std::uint32_t clut_rect_rows(std::uint32_t entry_count);
std::vector<std::uint8_t> clut_scratch(std::uint32_t entry_count,
                                       std::span<const std::uint8_t> palette);

} // namespace okage
