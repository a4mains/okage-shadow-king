#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace okage {

inline constexpr std::size_t save_payload_size = 0xd78;
inline constexpr std::size_t source_save_file_size = save_payload_size + 12;
using SavePayload = std::array<std::uint8_t, save_payload_size>;

class SaveFormatError : public std::runtime_error {
public:
    enum class Reason { bad_magic, wrong_block_size, wrong_file_size, checksum_mismatch };
    SaveFormatError(Reason reason, const char* message);
    Reason reason() const noexcept { return reason_; }
private:
    Reason reason_;
};

// Exact retail helper 0x16d838. The source's 256-entry table leaves entry 255
// zero-initialized; its 32-bit recurrence is intentionally not masked to 16 bits.
std::uint32_t source_save_checksum(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encode_source_save(const SavePayload& payload);
SavePayload decode_source_save(std::span<const std::uint8_t> file);

class SaveIoError : public std::runtime_error {
public:
    SaveIoError(std::string operation, const std::filesystem::path& path, int error);
    int error_number() const noexcept { return error_; }
private:
    int error_;
};

class NativeSaveStore {
public:
    explicit NativeSaveStore(std::filesystem::path directory);
    std::optional<SavePayload> read(unsigned slot) const;
    void write(unsigned slot, const SavePayload& payload) const;
    const std::filesystem::path& directory() const noexcept { return directory_; }
private:
    static std::string filename(unsigned slot);
    std::filesystem::path directory_;
};

} // namespace okage
