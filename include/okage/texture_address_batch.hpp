#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <type_traits>

namespace okage {
// Native uint32 words, packed strides 20 and 8 bytes, no implicit bool ABI.
struct TextureAddressQuery { std::uint32_t psm, tbp, tbw, x, y; };
struct TextureAddressResult {
    std::uint32_t address; // zero for every invalid query
    std::uint32_t valid;   // exactly 0 or 1
    bool operator==(const TextureAddressResult&) const = default;
};
static_assert(std::is_standard_layout_v<TextureAddressQuery> && sizeof(TextureAddressQuery) == 20);
static_assert(offsetof(TextureAddressQuery, y) == 16 && alignof(TextureAddressQuery) == 4);
static_assert(std::is_standard_layout_v<TextureAddressResult> && sizeof(TextureAddressResult) == 8);
static_assert(offsetof(TextureAddressResult, valid) == 4 && alignof(TextureAddressResult) == 4);
inline constexpr std::size_t texture_address_batch_limit = 1u << 20;

void validate_texture_address_batch(std::size_t count, std::size_t output_count,
                                   std::size_t capacity = texture_address_batch_limit);
void texture_address_batch_cpu(std::span<const TextureAddressQuery> input,
                               std::span<TextureAddressResult> output);
void texture_address_batch_scalar(std::span<const TextureAddressQuery> input,
                                  std::span<TextureAddressResult> output);

struct TextureAddressBatchTiming {
    // Available only after successful completion; NaN means unavailable.
    double gpu_seconds = std::numeric_limits<double>::quiet_NaN();
};
class TextureAddressBatchBackend {
public:
    virtual ~TextureAddressBatchBackend() = default;
    virtual std::string name() const = 0;
    virtual TextureAddressBatchTiming execute(std::span<const TextureAddressQuery>,
                                              std::span<TextureAddressResult>) = 0;
};
std::unique_ptr<TextureAddressBatchBackend> make_metal_texture_address_batch(
    std::size_t capacity, bool diagnostic = false);
} // namespace okage
