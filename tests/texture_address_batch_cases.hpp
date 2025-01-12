#pragma once

#include "okage/gs_texture_address.hpp"
#include "okage/texture_address_batch.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace okage::test {
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Bounded and deterministic; no assets or reference process are required.
inline std::vector<TextureAddressQuery> focused_texture_address_queries() {
    std::vector<TextureAddressQuery> queries;
    constexpr std::array<std::uint32_t, 27> edges = {
        0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33,
        63, 64, 65, 127, 128, 129, 255, 256, 511, 1023, 1024, 2046, 2047};
    for (auto psm : {0u, 19u})
        for (auto tbp : {0u, 1u, 16382u, 16383u})
            for (auto tbw : {0u, 1u, 2u, 3u, 63u})
                for (auto y : edges)
                    for (auto x : edges) queries.push_back({psm, tbp, tbw, x, y});
    // Exercise every in-page swizzle combination in each supported format.
    for (auto psm : {0u, 19u})
        for (std::uint32_t y = 0; y < 128; ++y)
            for (std::uint32_t x = 0; x < 256; ++x)
                queries.push_back({psm, 16383u, 3u, x, y});
    // Reproducible non-grid combinations across the entire legal field range.
    std::uint32_t state = 0xa341316cu;
    auto next = [&state] {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return state;
    };
    for (unsigned i = 0; i < 8192; ++i) {
        const auto psm = (next() & 1u) ? 19u : 0u;
        queries.push_back({psm, next() & 16383u, next() & 63u,
                           next() & 2047u, next() & 2047u});
    }
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    for (auto psm : {0u, 19u}) {
        for (unsigned field = 0; field != 5; ++field) {
            for (unsigned kind = 0; kind != 2; ++kind) {
                TextureAddressQuery q{psm, 1u, 3u, 64u, 128u};
                switch (field) {
                case 0: q.psm = kind ? max : 1u; break;
                case 1: q.tbp = kind ? max : 16384u; break;
                case 2: q.tbw = kind ? max : 64u; break;
                case 3: q.x = kind ? max : 2048u; break;
                case 4: q.y = kind ? max : 2048u; break;
                }
                queries.push_back(q);
            }
        }
    }
    queries.push_back({max, max, max, max, max});
    for (auto psm : {18u, 20u, 63u, 64u}) queries.push_back({psm, 0, 0, 0, 0});
    return queries;
}

inline void check_texture_address_results(std::span<const TextureAddressQuery> input,
                                          std::span<const TextureAddressResult> output,
                                          const std::string& label) {
    require(output.size() >= input.size(), label + ": missing results");
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& q = input[i];
        const auto expected = gs_texture_byte_address(q.psm, q.tbp, q.tbw, q.x, q.y);
        const TextureAddressResult result{expected.value_or(0u), expected ? 1u : 0u};
        require(output[i] == result, label + ": mismatch at query " + std::to_string(i));
    }
}

template<class Execute>
inline std::size_t check_texture_address_batch(Execute execute, const std::string& label) {
    auto queries = focused_texture_address_queries();
    std::size_t checked = 0;
    constexpr TextureAddressResult poison{0xfedcba98u, 0x89abcdefu};
    constexpr TextureAddressResult guard{0x12345678u, 0x87654321u};
    std::vector<TextureAddressResult> storage(queries.size() + 4, guard);
    for (unsigned iteration = 0; iteration != 3; ++iteration) {
        for (auto count : {std::size_t{0}, std::size_t{1}, std::size_t{31},
                           std::size_t{33}, queries.size()}) {
            std::fill(storage.begin(), storage.end(), guard);
            std::fill(storage.begin() + 1, storage.begin() + 1 + count, poison);
            auto input = std::span<const TextureAddressQuery>(queries).first(count);
            auto output = std::span<TextureAddressResult>(storage).subspan(1, count + 2);
            execute(input, output);
            check_texture_address_results(input, output, label);
            require(storage.front() == guard, label + ": prefix guard changed");
            for (std::size_t i = count + 1; i < storage.size(); ++i)
                require(storage[i] == guard, label + ": trailing guard changed");
            checked += count;
        }
        // Force the next execution to observe new values in every input field.
        for (auto& q : queries) {
            q.psm = q.psm == 0u ? 19u : (q.psm == 19u ? 0u : 18u);
            q.tbp = (q.tbp + 109u) & 16383u;
            q.tbw = (q.tbw + 7u) & 63u;
            q.x = (q.x + 67u) & 2047u;
            q.y = (q.y + 131u) & 2047u;
        }
        // Keep invalid queries mixed into the dispatch-tail cases after mutation.
        queries[1].psm = 1u;
        queries[17].tbp = 16384u;
        queries[30].y = 2048u;
        queries[32].tbw = 64u;
    }
    return checked;
}
} // namespace okage::test
