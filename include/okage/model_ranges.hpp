#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace okage {

struct ModelRangeLease {
    unsigned first{};
    unsigned count{};
    // Host lifetime guards only: neither value is a retail model/texture ID.
    std::uint64_t owner_identity{};
    std::uint64_t serial{};
    friend bool operator==(const ModelRangeLease&, const ModelRangeLease&) = default;
};

class ModelRangeAllocator {
public:
    static constexpr unsigned capacity = 128;
    using FreeRange = std::pair<unsigned, unsigned>;  // first, count

    ModelRangeAllocator();
    ModelRangeAllocator(const ModelRangeAllocator&) = delete;
    ModelRangeAllocator& operator=(const ModelRangeAllocator&) = delete;
    ModelRangeAllocator(ModelRangeAllocator&&) = delete;
    ModelRangeAllocator& operator=(ModelRangeAllocator&&) = delete;

    std::optional<ModelRangeLease> allocate(unsigned count);
    // Reject stale, cross-owner, duplicate or altered leases before mutation.
    bool release(const ModelRangeLease& lease);
    bool live(const ModelRangeLease& lease) const noexcept;
    unsigned available_count() const noexcept { return available_count_; }
    // Sorted by first; references/iterators may be invalidated by mutation.
    const std::vector<FreeRange>& free_ranges() const noexcept { return free_ranges_; }

private:
    struct Allocation {
        unsigned count{};
        std::uint64_t serial{};
    };
    const std::uint64_t owner_identity_;
    std::uint64_t serial_{};
    unsigned available_count_{capacity};
    std::array<Allocation, capacity> allocations_{};
    std::vector<FreeRange> free_ranges_;
};

}  // namespace okage
