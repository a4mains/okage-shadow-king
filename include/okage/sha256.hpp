#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace okage {

// Portable FIPS 180-4 SHA-256 (no platform crypto APIs).
class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    // Finalizes; the object must not be updated afterwards.
    std::array<std::uint8_t, 32> finish();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{0};
    std::uint64_t length_{0};
};

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);
// Lowercase hexadecimal digest.
std::string sha256_hex(std::span<const std::uint8_t> data);

} // namespace okage
