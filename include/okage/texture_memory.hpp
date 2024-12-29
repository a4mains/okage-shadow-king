#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace okage {

enum class GsPixelStorage : std::uint8_t {
    Psmct32 = 0x00, Psmct24 = 0x01, Psmt8H = 0x1b,
    Psmt4Hl = 0x24, Psmt4Hh = 0x2c, Psmz32 = 0x30,
    Psmz24 = 0x31, Psmt8 = 0x13, Psmct16 = 0x02,
    Psmz16 = 0x32, Psmt4 = 0x14, Psmct16S = 0x0a,
    Psmz16S = 0x3a
};
using TextureAllocationId = std::uint32_t;

struct GsTextureFormat {
    GsPixelStorage psm{};
    // Source record fields +8/+0xc are texel dimensions of one allocator
    // footprint block; +0/+4 are allocator packing dimensions.
    std::uint8_t block_width{};
    std::uint8_t block_height{};
    std::uint8_t allocator_blocks_per_row{};
    std::uint8_t allocator_group_height{};
    std::uint8_t occupancy_mask{};
    std::array<std::uint8_t, 32> block_order{};
};

const GsTextureFormat& gs_texture_format(GsPixelStorage psm);
bool is_supported_gs_texture_format(std::uint32_t psm) noexcept;

struct GsTextureAllocation {
    TextureAllocationId id{};
    std::uint64_t descriptor{};
    GsPixelStorage psm{};
    std::uint16_t base_block{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint8_t buffer_width_64{};
    std::uint8_t allocation_class{};
    std::uint16_t references{};
};

struct GsBlockAddress {
    std::uint32_t byte_address{};
    std::uint32_t block_index{};
    bool inside_vram{};
};

struct GsByteObservation {
    std::optional<std::uint8_t> value;
    std::optional<TextureAllocationId> allocation_owner;
    bool allocation_state_known{};
};

class Ps2TextureMemory {
public:
    static constexpr std::uint32_t block_bytes = 256;
    static constexpr std::uint32_t block_count = 0x4000;
    static constexpr std::uint32_t vram_bytes = block_bytes * block_count;

    Ps2TextureMemory();
    void initialize_empty_allocator();
    bool allocator_initialized() const noexcept;
    std::uint32_t available_count() const noexcept;
    std::array<std::uint8_t, 8192> occupancy_bytes() const;
    const std::array<std::uint16_t, 64>& search_cursors() const noexcept;
    std::vector<TextureAllocationId> free_queue_ids() const;

    std::optional<GsTextureAllocation> allocate(
        std::uint8_t allocation_class, GsPixelStorage psm,
        std::uint16_t width, std::uint16_t height);
    bool retain(TextureAllocationId id);
    bool adjust_references(TextureAllocationId id, std::int32_t delta);
    bool free_allocation(TextureAllocationId id);
    bool release(TextureAllocationId id);
    std::optional<GsTextureAllocation> allocation(TextureAllocationId id) const;

    GsBlockAddress block_address(TextureAllocationId id,
                                 std::uint32_t x, std::uint32_t y) const;
    std::optional<TextureAllocationId> owner_of_byte(std::uint32_t address) const;
    std::optional<TextureAllocationId> owner_of_nibble(std::uint32_t address,
                                                         std::uint8_t lane) const;

    void write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes);
    GsByteObservation observe_byte(std::uint32_t address) const;

private:
    struct Block {
        std::uint8_t mask{};
        std::array<std::optional<TextureAllocationId>, 4> owners{};
    };
    bool initialized_{};
    std::array<Block, block_count> blocks_{};
    std::array<std::uint16_t, 64> next_candidate_{};
    std::array<std::optional<GsTextureAllocation>, 4096> allocations_{};
    std::deque<TextureAllocationId> free_ids_;
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint8_t> defined_;

    GsTextureAllocation& require_live(TextureAllocationId id);
    const GsTextureAllocation& require_live(TextureAllocationId id) const;
};

} // namespace okage
