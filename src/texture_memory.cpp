#include "okage/texture_memory.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::array<std::uint8_t, 32> order_0_4_11{
    0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,
    8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31
};
constexpr std::array<std::uint8_t, 32> order_5_6{
    16,17,20,21,0,1,4,5,18,19,22,23,2,3,6,7,
    24,25,28,29,8,9,12,13,26,27,30,31,10,11,14,15
};
constexpr std::array<std::uint8_t, 32> order_7{
    0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,
    16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31
};
constexpr std::array<std::uint8_t, 32> order_8{
    16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31,
    0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15
};
constexpr std::array<std::uint8_t, 32> order_9{
    0,2,16,18,1,3,17,19,8,10,24,26,9,11,25,27,
    4,6,20,22,5,7,21,23,12,14,28,30,13,15,29,31
};
constexpr std::array<std::uint8_t, 32> order_10{
    16,18,0,2,17,19,1,3,24,26,8,10,25,27,9,11,
    20,22,4,6,21,23,5,7,28,30,12,14,29,31,13,15
};
constexpr std::array<std::uint8_t, 32> order_12{
    0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,
    16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31
};
constexpr GsTextureFormat formats[]{
    {GsPixelStorage::Psmct32, 8,8,8,4,0x0f,order_0_4_11},
    {GsPixelStorage::Psmct24, 8,8,8,4,0x03,order_0_4_11},
    {GsPixelStorage::Psmt8H,  8,8,8,4,0x0c,order_0_4_11},
    {GsPixelStorage::Psmt4Hl, 8,8,8,4,0x04,order_0_4_11},
    {GsPixelStorage::Psmt4Hh, 8,8,8,4,0x08,order_0_4_11},
    {GsPixelStorage::Psmz32, 8,8,8,4,0x0f,order_5_6},
    {GsPixelStorage::Psmz24, 8,8,8,4,0x03,order_5_6},
    {GsPixelStorage::Psmct16,16,8,4,8,0x0f,order_7},
    {GsPixelStorage::Psmz16, 16,8,4,8,0x0f,order_8},
    {GsPixelStorage::Psmct16S,16,8,4,8,0x0f,order_9},
    {GsPixelStorage::Psmz16S,16,8,4,8,0x0f,order_10},
    {GsPixelStorage::Psmt8,  16,16,8,4,0x0f,order_0_4_11},
    {GsPixelStorage::Psmt4,  32,16,4,8,0x0f,order_12}
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

bool is_supported_gs_texture_format(std::uint32_t psm) noexcept {
    for (const auto& f : formats)
        if (static_cast<std::uint32_t>(f.psm) == psm) return true;
    return false;
}

Ps2TextureMemory::Ps2TextureMemory()
    : planes_(std::make_shared<Planes>(
          Planes{std::vector<std::uint8_t>(vram_bytes),
                 std::vector<std::uint8_t>(vram_bytes)})) {}

Ps2TextureSnapshot Ps2TextureMemory::snapshot() const {
    // Copies metadata; planes_ is shared, so the next live write detaches.
    return Ps2TextureSnapshot(std::make_shared<const Ps2TextureMemory>(*this));
}

bool Ps2TextureMemory::shares_storage_with(const Ps2TextureSnapshot& other) const noexcept {
    return other.valid() && shares_storage_with(*other.state_);
}

const Ps2TextureMemory& Ps2TextureSnapshot::get() const {
    if (!state_) throw std::logic_error("Empty texture memory snapshot");
    return *state_;
}

void Ps2TextureMemory::initialize_empty_allocator() {
    if (initialized_) throw std::logic_error("GS allocator already initialized");
    blocks_.fill(Block{});
    next_candidate_.fill(0);
    free_ids_.clear();
    for (TextureAllocationId id = 0; id < allocations_.size(); ++id) {
        allocations_[id].reset();
        free_ids_.push_back(id);
    }
    initialized_ = true;
    ++ownership_epoch_;
}

bool Ps2TextureMemory::allocator_initialized() const noexcept { return initialized_; }

std::uint32_t Ps2TextureMemory::available_count() const noexcept {
    return static_cast<std::uint32_t>(free_ids_.size());
}

std::array<std::uint8_t, 8192> Ps2TextureMemory::occupancy_bytes() const {
    std::array<std::uint8_t, 8192> result{};
    for (std::uint32_t slot = 0; slot < block_count; ++slot) {
        const auto nibble = static_cast<std::uint8_t>(blocks_[slot].mask & 0x0f);
        result[slot / 2] |= static_cast<std::uint8_t>(nibble << ((slot & 1) * 4));
    }
    return result;
}

const std::array<std::uint16_t, 64>& Ps2TextureMemory::search_cursors() const noexcept {
    return next_candidate_;
}

std::vector<TextureAllocationId> Ps2TextureMemory::free_queue_ids() const {
    return {free_ids_.begin(), free_ids_.end()};
}

std::optional<GsTextureAllocation> Ps2TextureMemory::allocate(
    std::uint8_t allocation_class, GsPixelStorage psm,
    std::uint16_t width, std::uint16_t height) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    if (free_ids_.empty()) return std::nullopt;
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
    while (candidate < block_count &&
           (blocks_[candidate].mask & f.occupancy_mask)) ++candidate;
    next_candidate_[std::uint8_t(psm)] = static_cast<std::uint16_t>(candidate);
    while (candidate < block_count) {
        base = candidate & ~alignment_mask;
        if (footprint_free(base)) { found = true; break; }
        candidate = base + alignment;
    }
    if (!found) return std::nullopt;

    const auto tbw_pixels = ceil_div(blocks_wide, f.allocator_blocks_per_row) *
                             f.allocator_blocks_per_row * f.block_width;
    const auto buffer_width_64 = ceil_div(tbw_pixels, 64) & 0x3fu;
    const auto id = free_ids_.front();
    free_ids_.pop_front();
    const auto descriptor =
        (std::uint64_t(base & 0x3fff) << 0) |
        (std::uint64_t(buffer_width_64 & 0x3f) << 14) |
        (std::uint64_t(id & 0x0fff) << 20) |
        (std::uint64_t(static_cast<std::uint8_t>(psm) & 0x3f) << 32) |
        (std::uint64_t(width & 0x0fff) << 38) |
        (std::uint64_t(height & 0x0fff) << 50);
    GsTextureAllocation allocation{id, descriptor, psm, static_cast<std::uint16_t>(base), width, height,
        static_cast<std::uint8_t>(buffer_width_64), allocation_class, 0, next_serial_++};
    allocations_[id] = allocation;
    for (std::uint32_t y = 0; y < blocks_high; ++y) {
        for (std::uint32_t x = 0; x < blocks_wide; ++x) {
            auto& block = blocks_[base + block_offset(f, blocks_wide, x, y)];
            block.mask |= f.occupancy_mask;
            for (std::uint8_t lane = 0; lane < 4; ++lane)
                if (f.occupancy_mask & (1u << lane)) block.owners[lane] = id;
        }
    }
    ++ownership_epoch_;
    return allocation;
}

GsTextureAllocation& Ps2TextureMemory::require_live(TextureAllocationId id) {
    if (id >= allocations_.size() || !allocations_[id])
        throw std::out_of_range("Unknown GS texture allocation");
    return *allocations_[id];
}
const GsTextureAllocation& Ps2TextureMemory::require_live(TextureAllocationId id) const {
    if (id >= allocations_.size() || !allocations_[id])
        throw std::out_of_range("Unknown GS texture allocation");
    return *allocations_[id];
}

bool Ps2TextureMemory::retain(TextureAllocationId id) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    return adjust_references(id, 1);
}

bool Ps2TextureMemory::adjust_references(TextureAllocationId id, std::int32_t delta) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    auto& a = require_live(id);
    const auto sum_bits = std::uint32_t(a.references) + static_cast<std::uint32_t>(delta);
    const auto signed_sum = std::bit_cast<std::int32_t>(sum_bits);
    a.references = static_cast<std::uint16_t>(std::clamp(signed_sum, 0, 0x7fff));
    ++ownership_epoch_;
    return true;
}

bool Ps2TextureMemory::free_allocation(TextureAllocationId id) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    auto& a = require_live(id);
    if (a.references > 0) return false;
    const auto& f = gs_texture_format(a.psm);
    const auto blocks_wide = ceil_div(a.width, f.block_width);
    const auto blocks_high = ceil_div(a.height, f.block_height);
    for (std::uint32_t y = 0; y < blocks_high; ++y) {
        for (std::uint32_t x = 0; x < blocks_wide; ++x) {
            auto& block = blocks_[a.base_block + block_offset(f, blocks_wide, x, y)];
            block.mask &= static_cast<std::uint8_t>(~f.occupancy_mask);
            for (std::uint8_t lane = 0; lane < 4; ++lane)
                if (f.occupancy_mask & (1u << lane)) block.owners[lane].reset();
        }
    }
    for (auto& cursor : next_candidate_)
        if (a.base_block < cursor) cursor = a.base_block;
    allocations_[id].reset();
    free_ids_.push_back(id);
    ++ownership_epoch_;
    return true;
}

bool Ps2TextureMemory::release(TextureAllocationId id) {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    adjust_references(id, -1);
    return free_allocation(id);
}

std::optional<GsTextureAllocation> Ps2TextureMemory::allocation(TextureAllocationId id) const {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    if (id >= allocations_.size()) return std::nullopt;
    return allocations_[id];
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
    const auto first_lane = address % 4 < 3 ? 0 : 2;
    const auto last_lane = address % 4 < 3 ? 1 : 3;
    const auto owner = blocks_[address / block_bytes].owners[first_lane];
    if (owner != blocks_[address / block_bytes].owners[last_lane])
        throw std::logic_error("GS byte has partial or distinct sub-byte allocation owners");
    return owner;
}

std::optional<TextureAllocationId> Ps2TextureMemory::owner_of_lane(
    std::uint32_t address, std::uint8_t lane) const {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    if (address >= vram_bytes) throw std::out_of_range("GS byte address outside VRAM");
    if (lane >= 4) throw std::out_of_range("GS occupancy lane must be 0..3");
    return blocks_[address / block_bytes].owners[lane];
}

void Ps2TextureMemory::write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    if (address > vram_bytes || bytes.size() > vram_bytes - address)
        throw std::out_of_range("GS byte write outside VRAM");
    if (bytes.empty()) return;
    auto& planes = writable_planes();
    std::copy(bytes.begin(), bytes.end(), planes.bytes.begin() + address);
    std::fill(planes.defined.begin() + address,
              planes.defined.begin() + address + bytes.size(), 3);
    ++content_generation_;
}

Ps2TextureMemory::Planes& Ps2TextureMemory::writable_planes() {
    if (!planes_) {  // moved-from instance being reused
        planes_ = std::make_shared<Planes>(Planes{std::vector<std::uint8_t>(vram_bytes),
                                                  std::vector<std::uint8_t>(vram_bytes)});
    } else if (planes_.use_count() != 1) {
        planes_ = std::make_shared<Planes>(*planes_);
    }
    return *planes_;
}

void Ps2TextureMemory::write_masked_bytes(
    std::uint32_t address, std::span<const std::uint8_t> bytes,
    std::span<const std::uint8_t> known_masks) {
    if (bytes.size() != known_masks.size())
        throw std::invalid_argument("GS masked write has unequal byte/mask lengths");
    if (address > vram_bytes || bytes.size() > vram_bytes - address)
        throw std::out_of_range("GS masked write outside VRAM");
    if (std::any_of(known_masks.begin(), known_masks.end(),
                    [](std::uint8_t mask) { return mask > 3; }))
        throw std::invalid_argument("GS masked write mask exceeds two nibbles");
    if (bytes.empty()) return;
    auto& planes = writable_planes();
    std::copy(bytes.begin(), bytes.end(), planes.bytes.begin() + address);
    std::copy(known_masks.begin(), known_masks.end(), planes.defined.begin() + address);
    ++content_generation_;
}

void Ps2TextureMemory::write_nibble(std::uint32_t address, std::uint8_t nibble, std::uint8_t value) {
    if (address >= vram_bytes) throw std::out_of_range("GS nibble write outside VRAM");
    if (nibble > 1) throw std::out_of_range("GS nibble selector must be 0 or 1");
    auto& planes = writable_planes();
    const unsigned shift = nibble * 4u;
    planes.bytes[address] = static_cast<std::uint8_t>(
        (planes.bytes[address] & ~(0x0fu << shift)) | ((value & 0x0fu) << shift));
    planes.defined[address] |= static_cast<std::uint8_t>(1u << nibble);
    ++content_generation_;
}

std::size_t Ps2TextureMemory::invalidate_owned_nibbles(TextureAllocationId id) {
    require_live(id);
    std::size_t changed = 0;
    for (std::uint32_t block = 0; block < block_count; ++block) {
        const auto& owners = blocks_[block].owners;
        if (owners[0] != id && owners[2] != id && owners[3] != id) continue;
        for (std::uint32_t a = block * block_bytes; a < (block + 1) * block_bytes; ++a)
            for (std::uint8_t n = 0; n < 2; ++n) {
                if (owner_of_nibble(a, n) != id) continue;
                if (!planes_ || !((planes_->defined[a] >> n) & 1u)) continue;
                writable_planes().defined[a] &= static_cast<std::uint8_t>(~(1u << n));
                ++changed;
            }
    }
    if (changed) ++content_generation_;
    return changed;
}

std::optional<TextureAllocationId> Ps2TextureMemory::owner_of_nibble(
    std::uint32_t address, std::uint8_t nibble) const {
    if (!initialized_) throw std::logic_error("GS allocator state is unknown");
    if (address >= vram_bytes) throw std::out_of_range("GS byte address outside VRAM");
    if (nibble > 1) throw std::out_of_range("GS nibble selector must be 0 or 1");
    const auto& owners = blocks_[address / block_bytes].owners;
    if (address % 4 == 3) return owners[2 + nibble];
    if (owners[0] != owners[1]) throw std::logic_error("GS occupancy lanes 0/1 disagree");
    return owners[0];
}

GsByteObservation Ps2TextureMemory::observe_byte(std::uint32_t address) const {
    if (address >= vram_bytes) throw std::out_of_range("GS byte read outside VRAM");
    const bool defined = planes_ && planes_->defined[address] == 3;
    const auto value = defined ? std::optional<std::uint8_t>(planes_->bytes[address]) : std::nullopt;
    if (!initialized_) return {value, std::nullopt, false, false};
    const auto lo = owner_of_nibble(address, 0);
    const auto hi = owner_of_nibble(address, 1);
    if (lo != hi) return {value, std::nullopt, true, true};
    return {value, lo, true, false};
}

std::vector<std::uint32_t> Ps2TextureMemory::known_byte_addresses() const {
    std::vector<std::uint32_t> result;
    if (!planes_) return result;
    for (std::uint32_t a = 0; a < vram_bytes; ++a)
        if (planes_->defined[a] & 3u) result.push_back(a);
    return result;
}

GsNibbleObservation Ps2TextureMemory::observe_nibble(std::uint32_t address, std::uint8_t nibble) const {
    if (address >= vram_bytes) throw std::out_of_range("GS byte read outside VRAM");
    if (nibble > 1) throw std::out_of_range("GS nibble selector must be 0 or 1");
    GsNibbleObservation r;
    r.known = planes_ && ((planes_->defined[address] >> nibble) & 1u);
    if (r.known) r.value = static_cast<std::uint8_t>((planes_->bytes[address] >> (nibble * 4u)) & 0x0fu);
    r.allocation_state_known = initialized_;
    if (initialized_) r.allocation_owner = owner_of_nibble(address, nibble);
    return r;
}

GsMaskedByteObservation Ps2TextureMemory::observe_byte_masked(std::uint32_t address) const {
    if (address >= vram_bytes) throw std::out_of_range("GS byte read outside VRAM");
    GsMaskedByteObservation r;
    const unsigned known = planes_ ? planes_->defined[address] & 3u : 0u;
    r.known_mask = static_cast<std::uint8_t>(known);
    if (planes_) {
        const unsigned mask = ((known & 1u) ? 0x0fu : 0u) | ((known & 2u) ? 0xf0u : 0u);
        r.value = static_cast<std::uint8_t>(planes_->bytes[address] & mask);
    }
    r.allocation_state_known = initialized_;
    if (initialized_) {
        r.nibble_owner = {owner_of_nibble(address, 0), owner_of_nibble(address, 1)};
        r.owner_split = r.nibble_owner[0] != r.nibble_owner[1];
        if (!r.owner_split) r.whole_byte_owner = r.nibble_owner[0];
    }
    return r;
}

} // namespace okage
