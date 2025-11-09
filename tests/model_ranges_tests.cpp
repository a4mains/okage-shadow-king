#include "okage/model_ranges.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {
using okage::ModelRangeAllocator;
using okage::ModelRangeLease;
using Ranges = std::vector<ModelRangeAllocator::FreeRange>;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
ModelRangeLease take(ModelRangeAllocator& owner, unsigned count, unsigned first) {
    const auto lease = owner.allocate(count);
    require(lease && lease->first == first && lease->count == count && owner.live(*lease),
            "allocation does not match source range choice");
    return *lease;
}
void free_ranges(const ModelRangeAllocator& owner, const Ranges& expected) {
    require(owner.free_ranges() == expected, "free-range order or coalescing differs");
    unsigned available = 0;
    for (const auto& [first, count] : expected) {
        require(count > 0 && first < 128 && count <= 128 - first, "invalid free range");
        available += count;
    }
    require(owner.available_count() == available, "available total differs from ranges");
}
void rejected_release(ModelRangeAllocator& owner, const ModelRangeLease& lease) {
    const auto before = owner.free_ranges();
    const auto available = owner.available_count();
    require(!owner.live(lease) && !owner.release(lease), "invalid lease accepted");
    require(before == owner.free_ranges() && available == owner.available_count(),
            "rejected lease changed allocator");
}
void invalid_count(ModelRangeAllocator& owner, unsigned count) {
    const auto before = owner.free_ranges();
    const auto available = owner.available_count();
    bool rejected = false;
    try { (void)owner.allocate(count); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && before == owner.free_ranges() && available == owner.available_count(),
            "invalid count was not rejected without mutation");
}
}  // namespace

int main() {
    static_assert(!std::is_copy_constructible_v<ModelRangeAllocator>);
    static_assert(!std::is_copy_assignable_v<ModelRangeAllocator>);
    static_assert(!std::is_move_constructible_v<ModelRangeAllocator>);
    static_assert(!std::is_move_assignable_v<ModelRangeAllocator>);
    try {
        {
            ModelRangeAllocator owner;
            free_ranges(owner, {{0, 128}});  // No implicit embedded allocations.
            invalid_count(owner, 0);
            invalid_count(owner, 129);
            invalid_count(owner, std::numeric_limits<unsigned>::max());
            rejected_release(owner, {});
            const auto embedded0 = take(owner, 1, 0);
            const auto embedded1 = take(owner, 1, 1);
            const auto wal = take(owner, 1, 2);
            require(embedded0.serial == 1 && embedded1.serial == 2 && wal.serial == 3,
                    "invalid allocations consumed host serials");
            free_ranges(owner, {{3, 125}});
            require(owner.release(wal), "WAL release failed");
            free_ranges(owner, {{2, 126}});
            const auto reentry = take(owner, 1, 2);
            require(reentry.serial != wal.serial && owner.live(embedded0) && owner.live(embedded1),
                    "reentry resets surviving embedded groups or incarnation");
            rejected_release(owner, wal);
            require(owner.live(reentry), "stale release destroyed reentry range");
        }
        {
            ModelRangeAllocator owner;
            const auto all = take(owner, 128, 0);
            free_ranges(owner, {});
            require(!owner.allocate(1) && owner.live(all), "exhaustion changed live range");
            free_ranges(owner, {});
            require(owner.release(all), "full range release failed");
            free_ranges(owner, {{0, 128}});
            const auto reused = take(owner, 128, 0);
            require(reused.serial == all.serial + 1, "exhaustion consumed serial");
            rejected_release(owner, all);  // Exact same first/count reused.
            require(owner.live(reused), "old lease retired new identical range");
            require(owner.release(reused), "new range release failed");
            rejected_release(owner, reused);  // Duplicate free.
            free_ranges(owner, {{0, 128}});
        }
        {
            ModelRangeAllocator owner;
            const auto a = take(owner, 5, 0);
            const auto b = take(owner, 7, 5);
            const auto c = take(owner, 3, 12);
            const auto d = take(owner, 9, 15);
            const auto e = take(owner, 104, 24);
            require(owner.release(d) && owner.release(b), "fragmentation setup failed");
            free_ranges(owner, {{5, 7}, {15, 9}});
            const auto eight = take(owner, 8, 15);  // Skip inadequate low hole.
            const auto six = take(owner, 6, 5);  // Return to lowest adequate hole.
            free_ranges(owner, {{11, 1}, {23, 1}});
            require(!owner.allocate(2), "fragmented space wrongly treated as contiguous");
            require(owner.release(c), "adjacent predecessor merge failed");
            free_ranges(owner, {{11, 4}, {23, 1}});
            require(owner.release(six), "successor merge failed");
            free_ranges(owner, {{5, 10}, {23, 1}});
            require(owner.release(eight), "two-sided merge failed");
            free_ranges(owner, {{5, 19}});
            require(owner.release(a), "front merge failed");
            free_ranges(owner, {{0, 24}});
            require(owner.release(e), "final merge failed");
            free_ranges(owner, {{0, 128}});
        }
        {
            ModelRangeAllocator owner;
            const auto low = take(owner, 10, 0);
            const auto separator = take(owner, 2, 10);
            const auto exact = take(owner, 8, 12);
            (void)take(owner, 108, 20);
            require(owner.release(exact) && owner.release(low), "first-fit setup failed");
            (void)take(owner, 8, 0);  // First-fit, not best-fit at12.
            free_ranges(owner, {{8, 2}, {12, 8}});
            require(owner.live(separator), "unrelated live range changed");
        }
        {
            ModelRangeAllocator a, b;
            const auto from_a = take(a, 4, 0);
            const auto from_b = take(b, 4, 0);
            require(from_a.serial == from_b.serial && from_a.owner_identity != from_b.owner_identity,
                    "owners share lease identity");
            rejected_release(b, from_a);
            require(b.live(from_b), "cross-owner rejection changed valid lease");
            for (unsigned field = 0; field < 4; ++field) {
                auto altered = from_a;
                if (field == 0) altered.first = 1;
                if (field == 1) altered.count = 3;
                if (field == 2) ++altered.serial;
                if (field == 3) altered.owner_identity = from_b.owner_identity;
                rejected_release(a, altered);
                require(a.live(from_a), "altered lease changed original range");
            }
            auto outside = from_a;
            outside.first = std::numeric_limits<unsigned>::max();
            rejected_release(a, outside);
            outside = from_a;
            outside.count = std::numeric_limits<unsigned>::max();
            rejected_release(a, outside);
        }
        {
            alignas(ModelRangeAllocator) std::array<std::byte, sizeof(ModelRangeAllocator)> storage;
            auto* owner = std::construct_at(reinterpret_cast<ModelRangeAllocator*>(storage.data()));
            const auto old = take(*owner, 6, 0);
            std::destroy_at(owner);
            owner = std::construct_at(reinterpret_cast<ModelRangeAllocator*>(storage.data()));
            const auto current = take(*owner, 6, 0);
            require(old.serial == current.serial && old.owner_identity != current.owner_identity,
                    "reconstructed owner reused identity");
            rejected_release(*owner, old);
            require(owner->live(current), "old owner lease retired reconstructed owner range");
            std::destroy_at(owner);
        }
        std::cout << "model_ranges_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "model_ranges_tests: " << error.what() << '\n';
        return 1;
    }
}
