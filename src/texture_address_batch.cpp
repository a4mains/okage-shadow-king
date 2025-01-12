#include "okage/texture_address_batch.hpp"
#include "okage/gs_texture_address.hpp"

#include <limits>
#include <stdexcept>

namespace okage {
namespace {
void check_bytes(std::size_t count, std::size_t stride) {
    if (count > std::numeric_limits<std::size_t>::max() / stride)
        throw std::length_error("texture address batch byte size overflow");
}

inline TextureAddressResult fast_address(const TextureAddressQuery& q) noexcept {
    if (q.tbp > 16383u || q.tbw > 63u || q.x > 2047u || q.y > 2047u ||
        (q.psm != 0u && q.psm != 19u)) return {0u, 0u};

    std::uint32_t page, block, offset;
    if (q.psm == 0u) {
        page = (q.x >> 6u) + (q.y >> 5u) * q.tbw;
        block = ((q.x >> 3u) & 1u) | ((q.x >> 2u) & 4u) |
                ((q.x >> 1u) & 16u) | ((q.y >> 2u) & 2u) |
                ((q.y >> 1u) & 8u);
        offset = ((q.x & 1u) << 2u) | ((q.x & 2u) << 3u) |
                 ((q.x & 4u) << 3u) | ((q.y & 1u) << 3u) |
                 ((q.y & 6u) << 5u);
    } else {
        // Multiplication precedes division: odd TBW alternates page-row stride.
        page = (q.x >> 7u) + (((q.y >> 6u) * q.tbw) >> 1u);
        block = ((q.x >> 4u) & 1u) | ((q.x >> 3u) & 4u) |
                ((q.x >> 2u) & 16u) | ((q.y >> 3u) & 2u) |
                ((q.y >> 2u) & 8u);
        offset = ((q.x & 1u) << 2u) | ((q.x & 2u) << 3u) |
                 ((q.y & 1u) << 3u) |
                 ((((q.y >> 1u) ^ (q.y >> 2u) ^ (q.x >> 2u)) & 1u) << 5u) |
                 ((q.y & 12u) << 4u) | ((q.x & 8u) >> 2u) |
                 ((q.y & 2u) >> 1u);
    }
    return {((q.tbp << 8u) + (page << 13u) + (block << 8u) + offset) & 0x3fffffu, 1u};
}
} // namespace

void validate_texture_address_batch(std::size_t count, std::size_t output_count,
                                   std::size_t capacity) {
    check_bytes(count, sizeof(TextureAddressQuery));
    check_bytes(count, sizeof(TextureAddressResult));
    check_bytes(output_count, sizeof(TextureAddressResult));
    check_bytes(capacity, sizeof(TextureAddressQuery));
    check_bytes(capacity, sizeof(TextureAddressResult));
    if (capacity > texture_address_batch_limit || count > texture_address_batch_limit)
        throw std::length_error("texture address batch exceeds finite limit");
    if (count > capacity)
        throw std::length_error("texture address batch exceeds backend capacity");
    if (output_count < count)
        throw std::invalid_argument("texture address batch output is too short");
}

void texture_address_batch_cpu(std::span<const TextureAddressQuery> input,
                               std::span<TextureAddressResult> output) {
    validate_texture_address_batch(input.size(), output.size());
    for (std::size_t i = 0; i < input.size(); ++i) output[i] = fast_address(input[i]);
}

void texture_address_batch_scalar(std::span<const TextureAddressQuery> input,
                                  std::span<TextureAddressResult> output) {
    validate_texture_address_batch(input.size(), output.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& q = input[i];
        const auto address = gs_texture_byte_address(q.psm, q.tbp, q.tbw, q.x, q.y);
        output[i] = {address.value_or(0u), address.has_value() ? 1u : 0u};
    }
}
} // namespace okage
