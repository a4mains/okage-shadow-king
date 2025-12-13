#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
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
    std::uint64_t serial{};
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
    bool owner_split{};
};

struct GsNibbleObservation {
    bool known{};
    std::uint8_t value{};  // 0..15; 0 and meaningless when !known
    std::optional<TextureAllocationId> allocation_owner;
    bool allocation_state_known{};
};

struct GsMaskedByteObservation {
    // bit0: low nibble known, bit1: high nibble known.
    std::uint8_t known_mask{};
    // Known nibbles in place; the bits of an unknown nibble are 0.
    std::uint8_t value{};
    std::array<std::optional<TextureAllocationId>, 2> nibble_owner{};
    // Engaged only when both nibbles have the same engaged owner.
    std::optional<TextureAllocationId> whole_byte_owner;
    // True when the nibbles' owner states differ (different ids, or one owned
    // and one free).
    bool owner_split{};
    bool allocation_state_known{};
};

struct TextureMemoryIdentity {
    std::uint64_t content_generation{};
    std::uint64_t ownership_epoch{};
    friend bool operator==(const TextureMemoryIdentity&,
                           const TextureMemoryIdentity&) = default;
};

class Ps2TextureSnapshot;

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
    std::optional<TextureAllocationId> owner_of_lane(std::uint32_t address,
                                                    std::uint8_t lane) const;

    void write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes);
    void write_nibble(std::uint32_t address, std::uint8_t nibble, std::uint8_t value);
    GsByteObservation observe_byte(std::uint32_t address) const;
    GsNibbleObservation observe_nibble(std::uint32_t address, std::uint8_t nibble) const;
    std::size_t invalidate_owned_nibbles(TextureAllocationId id);
    GsMaskedByteObservation observe_byte_masked(std::uint32_t address) const;
    std::vector<std::uint32_t> known_byte_addresses() const;
    std::optional<TextureAllocationId> owner_of_nibble(std::uint32_t address,
                                                       std::uint8_t nibble) const;

    TextureMemoryIdentity identity() const noexcept {
        return {content_generation_, ownership_epoch_};
    }
    Ps2TextureSnapshot snapshot() const;

    const void* storage_identity() const noexcept { return planes_.get(); }
    bool shares_storage_with(const Ps2TextureMemory& other) const noexcept {
        return planes_ && planes_ == other.planes_;
    }
    bool shares_storage_with(const Ps2TextureSnapshot& other) const noexcept;

private:
    friend class Ps2TextureSnapshot;
    struct Planes {
        std::vector<std::uint8_t> bytes;
        // Per-byte nibble definedness: bit0 low nibble, bit1 high nibble.
        std::vector<std::uint8_t> defined;
    };
    Planes& writable_planes();
    struct Block {
        std::uint8_t mask{};
        std::array<std::optional<TextureAllocationId>, 4> owners{};
    };
    bool initialized_{};
    std::array<Block, block_count> blocks_{};
    std::array<std::uint16_t, 64> next_candidate_{};
    std::array<std::optional<GsTextureAllocation>, 4096> allocations_{};
    std::deque<TextureAllocationId> free_ids_;
    std::shared_ptr<Planes> planes_;
    std::uint64_t content_generation_{};
    std::uint64_t ownership_epoch_{};
    std::uint64_t next_serial_{1};

    GsTextureAllocation& require_live(TextureAllocationId id);
    const GsTextureAllocation& require_live(TextureAllocationId id) const;
};

class Ps2TextureSnapshot {
public:
    Ps2TextureSnapshot() = default;
    bool valid() const noexcept { return static_cast<bool>(state_); }
    std::span<const std::uint8_t> physical_bytes() const { return get().planes_->bytes; }
    std::span<const std::uint8_t> known_nibbles() const { return get().planes_->defined; }
    TextureMemoryIdentity identity() const { return get().identity(); }
    bool allocator_initialized() const { return get().allocator_initialized(); }
    std::uint32_t available_count() const { return get().available_count(); }
    std::array<std::uint8_t, 8192> occupancy_bytes() const { return get().occupancy_bytes(); }
    const std::array<std::uint16_t, 64>& search_cursors() const { return get().search_cursors(); }
    std::vector<TextureAllocationId> free_queue_ids() const { return get().free_queue_ids(); }
    std::optional<GsTextureAllocation> allocation(TextureAllocationId id) const {
        return get().allocation(id);
    }
    GsBlockAddress block_address(TextureAllocationId id, std::uint32_t x, std::uint32_t y) const {
        return get().block_address(id, x, y);
    }
    std::optional<TextureAllocationId> owner_of_byte(std::uint32_t a) const {
        return get().owner_of_byte(a);
    }
    std::optional<TextureAllocationId> owner_of_lane(std::uint32_t a, std::uint8_t lane) const {
        return get().owner_of_lane(a, lane);
    }
    GsByteObservation observe_byte(std::uint32_t a) const { return get().observe_byte(a); }
    GsNibbleObservation observe_nibble(std::uint32_t a, std::uint8_t nibble) const {
        return get().observe_nibble(a, nibble);
    }
    GsMaskedByteObservation observe_byte_masked(std::uint32_t a) const {
        return get().observe_byte_masked(a);
    }
    std::optional<TextureAllocationId> owner_of_nibble(std::uint32_t a, std::uint8_t nibble) const {
        return get().owner_of_nibble(a, nibble);
    }
    std::vector<std::uint32_t> known_byte_addresses() const { return get().known_byte_addresses(); }
    const void* storage_identity() const { return get().storage_identity(); }
    bool shares_storage_with(const Ps2TextureMemory& m) const { return get().shares_storage_with(m); }
private:
    friend class Ps2TextureMemory;
    explicit Ps2TextureSnapshot(std::shared_ptr<const Ps2TextureMemory> s)
        : state_(std::move(s)) {}
    const Ps2TextureMemory& get() const;
    std::shared_ptr<const Ps2TextureMemory> state_;
};

} // namespace okage
