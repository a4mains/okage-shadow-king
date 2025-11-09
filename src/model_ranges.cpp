#include "okage/model_ranges.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
std::uint64_t new_owner_identity() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Model range owner identity exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}
}  // namespace

ModelRangeAllocator::ModelRangeAllocator() : owner_identity_(new_owner_identity()) {
    free_ranges_.reserve(capacity);
    free_ranges_.emplace_back(0, capacity);
}

std::optional<ModelRangeLease> ModelRangeAllocator::allocate(unsigned count) {
    if (count == 0 || count > capacity)
        throw std::invalid_argument("Model range count must be in 1..128");

    const auto range = std::find_if(free_ranges_.begin(), free_ranges_.end(),
                                    [count](const FreeRange& r) { return r.second >= count; });
    if (range == free_ranges_.end()) return std::nullopt;
    if (serial_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Model range lease serial exhausted");

    const unsigned first = range->first;
    const auto serial = serial_ + 1;
    if (range->second == count) {
        free_ranges_.erase(range);
    } else {
        range->first += count;
        range->second -= count;
    }
    allocations_[first] = {count, serial};
    available_count_ -= count;
    serial_ = serial;
    return ModelRangeLease{first, count, owner_identity_, serial};
}

bool ModelRangeAllocator::live(const ModelRangeLease& lease) const noexcept {
    if (lease.owner_identity != owner_identity_ || lease.serial == 0 ||
        lease.first >= capacity || lease.count == 0 || lease.count > capacity - lease.first)
        return false;
    const auto& allocation = allocations_[lease.first];
    return allocation.serial == lease.serial && allocation.count == lease.count;
}

bool ModelRangeAllocator::release(const ModelRangeLease& lease) {
    if (!live(lease)) return false;
    const unsigned count = allocations_[lease.first].count;  // 1cfca8 stored length.
    auto at = std::lower_bound(free_ranges_.begin(), free_ranges_.end(), lease.first,
                               [](const FreeRange& range, unsigned first) { return range.first < first; });
    at = free_ranges_.insert(at, {lease.first, count});

    // 1cf990 inserts in address order and merges predecessor, then successor.
    if (at != free_ranges_.begin()) {
        auto previous = at - 1;
        if (previous->first + previous->second == at->first) {
            previous->second += at->second;
            free_ranges_.erase(at);
            at = previous;
        }
    }
    if (at + 1 != free_ranges_.end() && at->first + at->second == (at + 1)->first) {
        at->second += (at + 1)->second;
        free_ranges_.erase(at + 1);
    }
    allocations_[lease.first] = {};
    available_count_ += count;
    return true;
}

}  // namespace okage
