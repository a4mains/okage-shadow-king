#include "okage/texture_memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::array<std::uint8_t, 32> block_order{
    0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,
    8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31
};
constexpr GsTextureFormat formats[]{
    {GsPixelStorage::Psmct32, 8, 8, 8, 4, 0x0f, block_order},
    {GsPixelStorage::Psmt8, 16, 16, 8, 4, 0x0f, block_order}
};

std::uint32_t ceil_div(std::uint32_t n, std::uint32_t d) {
    return n / d + (n % d != 0);
}

std::uint32_t block_offset(const GsTextureFormat& f, std::uint32_t blocks_wide,
                           std::uint32_t block_x, std::uint32_t block_y) {
    const std::uint32_t group_width = f.allocator_blocks_per_row;
    const std::uint32_t group_height = f.allocator_group_height;
    const std::uint32_t linear =
        (block_x & (group_width - 1)) + (block_x / group_width) * 32 +
        (block_y & (group_height - 1)) * group_width +
        (block_y / group_height) * ceil_div(blocks_wide, group_width) * 32;
    return (linear & ~31u) + f.block_order[linear & 31u];
}
}

const GsTextureFormat& gs_texture_format(GsPixelStorage psm) {
    for (const auto& f : formats) if (f.psm == psm) return f;
    throw std::invalid_argument("Unsupported GS pixel storage format");
}

Ps2TextureMemory::Ps2TextureMemory()
    : bytes_(vram_bytes), defined_(vram_bytes) {}

void Ps2TextureMemory::initialize_empty_allocator() {
    if (initialized_) throw std::logic_error("GS allocator already initialized");
    blocks_.fill(Block{});
    next_candidate_.fill(0);
    initialized_ = true;
}

bool Ps2TextureMemory::allocator_initialized() const noexcept { return initialized_; }

std::optional<GsTextureAllocation> Ps2TextureMemory::allocate(
    std::uint8_t allocation_class, GsPixelStorage psm,
    std::uint16_t width, std::uint16_t height) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    if (allocation_class >= 4 || width == 0 || height == 0) return std::nullopt;
    const auto& f = gs_texture_format(psm);
    const auto blocks_wide = ceil_div(width, f.block_width);
    const auto blocks_high = ceil_div(height, f.block_height);
    const std::uint32_t alignment = allocation_class < 2 ? 1 : 32;
    const std::uint32_t alignment_mask = alignment - 1;
    auto candidate = std::uint32_t(next_candidate_[std::uint8_t(psm)]);

    auto footprint_free = [&](std::uint32_t base) {
        for (std::uint32_t y = 0; y < blocks_high; ++y) {
            for (std::uint32_t x = 0; x < blocks_wide; ++x) {
                const auto slot = base + block_offset(f, blocks_wide, x, y);
                if (slot >= block_count || (blocks_[slot].mask & f.occupancy_mask)) return false;
            }
        }
        return true;
    };

    std::uint32_t base = 0;
    bool found = false;
    while (candidate < block_count) {
        while (candidate < block_count &&
               (blocks_[candidate].mask & f.occupancy_mask)) ++candidate;
        if (candidate >= block_count) break;
        base = candidate & ~alignment_mask;
        if (footprint_free(base)) { found = true; break; }
        candidate = base + alignment;
    }
    if (!found) return std::nullopt;

    if (next_id_ == 0) throw std::overflow_error("Texture allocation id exhausted");
    const auto id = next_id_++;
    const auto tbw_pixels = ceil_div(blocks_wide, f.allocator_blocks_per_row) *
                             f.allocator_blocks_per_row * f.block_width;
    const auto bytes_per_pixel = psm == GsPixelStorage::Psmct32 ? 4u : 1u;
    const auto row_bytes = tbw_pixels * bytes_per_pixel;
    const auto buffer_width_64 = ceil_div(row_bytes, 64) & 0x3fu;
    GsTextureAllocation allocation{id, psm, static_cast<std::uint16_t>(base), width, height,
        static_cast<std::uint8_t>(buffer_width_64), allocation_class, 0};
    allocations_.push_back(allocation);
    for (std::uint32_t y = 0; y < blocks_high; ++y) {
        for (std::uint32_t x = 0; x < blocks_wide; ++x) {
            auto& block = blocks_[base + block_offset(f, blocks_wide, x, y)];
            block.mask |= f.occupancy_mask;
            block.owner = id;
        }
    }
    next_candidate_[std::uint8_t(psm)] = static_cast<std::uint16_t>(candidate);
    return allocation;
}

GsTextureAllocation& Ps2TextureMemory::require_live(TextureAllocationId id) {
    auto it = std::find_if(allocations_.begin(), allocations_.end(),
        [=](const auto& a) { return a.id == id; });
    if (it == allocations_.end()) throw std::out_of_range("Unknown GS texture allocation");
    return *it;
}
const GsTextureAllocation& Ps2TextureMemory::require_live(TextureAllocationId id) const {
    auto it = std::find_if(allocations_.begin(), allocations_.end(),
        [=](const auto& a) { return a.id == id; });
    if (it == allocations_.end()) throw std::out_of_range("Unknown GS texture allocation");
    return *it;
}

bool Ps2TextureMemory::retain(TextureAllocationId id) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    auto& a = require_live(id);
    if (a.references < 0x7fff) ++a.references;
    return true;
}

bool Ps2TextureMemory::release(TextureAllocationId id) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    auto& a = require_live(id);
    if (a.references) --a.references;
    if (a.references) return false;
    const auto& f = gs_texture_format(a.psm);
    const auto blocks_wide = ceil_div(a.width, f.block_width);
    const auto blocks_high = ceil_div(a.height, f.block_height);
    for (std::uint32_t y = 0; y < blocks_high; ++y) {
        for (std::uint32_t x = 0; x < blocks_wide; ++x) {
            auto& block = blocks_[a.base_block + block_offset(f, blocks_wide, x, y)];
            block.mask &= static_cast<std::uint8_t>(~f.occupancy_mask);
            if (block.mask == 0) block.owner.reset();
        }
    }
    for (auto& cursor : next_candidate_)
        if (a.base_block < cursor) cursor = a.base_block;
    allocations_.erase(std::remove_if(allocations_.begin(), allocations_.end(),
        [=](const auto& item) { return item.id == id; }), allocations_.end());
    return true;
}

std::optional<GsTextureAllocation> Ps2TextureMemory::allocation(TextureAllocationId id) const {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    auto it = std::find_if(allocations_.begin(), allocations_.end(),
        [=](const auto& a) { return a.id == id; });
    if (it == allocations_.end()) return std::nullopt;
    return *it;
}

GsBlockAddress Ps2TextureMemory::block_address(TextureAllocationId id,
                                                std::uint32_t x, std::uint32_t y) const {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    const auto& a = require_live(id);
    const auto& f = gs_texture_format(a.psm);
    const auto blocks_wide = ceil_div(a.width, f.block_width);
    const auto bx = x / f.block_width;
    const auto by = y / f.block_height;
    const std::uint64_t slot = std::uint64_t(a.base_block) + block_offset(f, blocks_wide, bx, by);
    const bool in_vram = slot < block_count;
    const auto address = slot * block_bytes;
    return {address <= std::numeric_limits<std::uint32_t>::max()
                ? static_cast<std::uint32_t>(address) : UINT32_MAX,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(slot, UINT32_MAX)), in_vram};
}

std::optional<TextureAllocationId> Ps2TextureMemory::owner_of_byte(std::uint32_t address) const {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    if (address >= vram_bytes) throw std::out_of_range("GS byte address outside VRAM");
    return blocks_[address / block_bytes].owner;
}

void Ps2TextureMemory::write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    if (address > vram_bytes || bytes.size() > vram_bytes - address)
        throw std::out_of_range("GS byte write outside VRAM");
    std::copy(bytes.begin(), bytes.end(), bytes_.begin() + address);
    std::fill(defined_.begin() + address, defined_.begin() + address + bytes.size(), 1);
}

GsByteObservation Ps2TextureMemory::observe_byte(std::uint32_t address) const {
    if (address >= vram_bytes) throw std::out_of_range("GS byte read outside VRAM");
    const auto block = address / block_bytes;
    const auto value = defined_[address] ? std::optional<std::uint8_t>(bytes_[address]) : std::nullopt;
    return {value, initialized_ ? blocks_[block].owner : std::nullopt, initialized_};
}

} // namespace okage
