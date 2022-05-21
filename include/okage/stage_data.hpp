#pragma once
#include "okage/assets.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace okage {

// Raw authored STG setting payload. Values remain bit patterns because the
// generic STG owner does not assign meaning or float semantics to opcodes.
struct StageSettingRecord {
    std::uint32_t kind = 0;
    std::array<std::uint32_t,4> value_bits{};
};

// Seven source-order little-endian words from one 0x1c-byte STG record.
struct StageRecord28 {
    std::array<std::uint32_t,7> words{};
};

class StageData {
public:
    static constexpr std::size_t original_header_size = 0x278;

    static StageData parse(Bytes input);

    Bytes bytes() const noexcept { return owned_; }
    Bytes header() const noexcept { return Bytes(owned_).first(original_header_size); }
    const std::vector<StageSettingRecord>& settings() const noexcept { return settings_; }
    const std::vector<StageRecord28>& camera_records() const noexcept { return camera_records_; }
    const std::vector<StageRecord28>& camera_settings() const noexcept { return camera_settings_; }
    const std::vector<StageRecord28>& entrances() const noexcept { return entrances_; }

private:
    std::vector<std::uint8_t> owned_;
    std::vector<StageSettingRecord> settings_;
    std::vector<StageRecord28> camera_records_;
    std::vector<StageRecord28> camera_settings_;
    std::vector<StageRecord28> entrances_;
};

} // namespace okage
