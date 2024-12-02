#include "okage/stage_data.hpp"
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::uint32_t stage_magic = 0x00475453;
constexpr std::size_t setting_record_size = 20;
constexpr std::size_t record_size = 0x1c;
constexpr std::uint64_t max_settings = 4096;
constexpr std::uint64_t max_records = 30;

std::uint32_t read32(Bytes bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4)
        throw std::runtime_error("STG word outside input");
    return std::uint32_t(bytes[at]) |
           (std::uint32_t(bytes[at + 1]) << 8) |
           (std::uint32_t(bytes[at + 2]) << 16) |
           (std::uint32_t(bytes[at + 3]) << 24);
}

void check_table(std::uint64_t begin, std::uint64_t count, std::uint64_t stride,
                 std::uint64_t limit, std::uint64_t maximum, const char* what) {
    if (begin < 0x20 || (begin & 3) || count > maximum ||
        count > (std::numeric_limits<std::uint64_t>::max() - begin) / stride ||
        begin + count * stride > limit)
        throw std::runtime_error(what);
}

StageRecord28 record28(Bytes bytes, std::size_t at) {
    StageRecord28 result;
    for (std::size_t i = 0; i < result.words.size(); ++i)
        result.words[i] = read32(bytes, at + i * 4);
    return result;
}
}

StageData StageData::parse(Bytes input) {
    if (input.size() < 0x20)
        throw std::runtime_error("STG is shorter than its table directory");
    if (read32(input, 0) != stage_magic)
        throw std::runtime_error("Invalid STG magic");

    const std::uint64_t settings_begin = read32(input, 8);
    const std::uint64_t settings_count = read32(input, 12);
    const std::uint64_t camera_begin = read32(input, 16);
    const std::uint64_t camera_count = read32(input, 20);
    const std::uint64_t entrance_begin = read32(input, 24);
    const std::uint64_t entrance_count = read32(input, 28);
    const std::uint64_t file_size = input.size();

    check_table(settings_begin, settings_count, setting_record_size, file_size,
                max_settings, "Invalid STG settings table extent");
    check_table(camera_begin, camera_count, record_size, file_size,
                max_records, "Invalid STG camera record extent");
    check_table(entrance_begin, entrance_count, record_size, file_size,
                max_records, "Invalid STG entrance table extent");

    const std::uint64_t settings_end = settings_begin + settings_count * setting_record_size;
    if(settings_end>original_header_size)
        throw std::runtime_error("STG settings exceed persistent header copy");
    const std::uint64_t camera_end = camera_begin + camera_count * record_size;
    if (settings_end > camera_begin)
        throw std::runtime_error("Overlapping STG settings and camera records");
    if (camera_end > entrance_begin)
        throw std::runtime_error("Overlapping STG camera records and entrance table");
    const std::uint64_t middle_bytes = entrance_begin - camera_end;
    if (middle_bytes % record_size || middle_bytes / record_size > max_records)
        throw std::runtime_error("Invalid STG intermediate camera-settings span");

    StageData result;
    result.owned_.assign(input.begin(), input.end());
    const Bytes owned(result.owned_);
    result.settings_.reserve(static_cast<std::size_t>(settings_count));
    for (std::uint64_t i = 0; i < settings_count; ++i) {
        const auto at = static_cast<std::size_t>(settings_begin + i * setting_record_size);
        StageSettingRecord setting;
        setting.kind = read32(owned, at);
        for (std::size_t j = 0; j < setting.value_bits.size(); ++j)
            setting.value_bits[j] = read32(owned, at + 4 + j * 4);
        result.settings_.push_back(setting);
    }
    result.camera_records_.reserve(static_cast<std::size_t>(camera_count));
    for (std::uint64_t i = 0; i < camera_count; ++i)
        result.camera_records_.push_back(record28(owned, static_cast<std::size_t>(camera_begin + i * record_size)));
    const std::uint64_t middle_count = middle_bytes / record_size;
    result.camera_settings_.reserve(static_cast<std::size_t>(middle_count));
    for (std::uint64_t i = 0; i < middle_count; ++i)
        result.camera_settings_.push_back(record28(owned, static_cast<std::size_t>(camera_end + i * record_size)));
    result.entrances_.reserve(static_cast<std::size_t>(entrance_count));
    for (std::uint64_t i = 0; i < entrance_count; ++i)
        result.entrances_.push_back(record28(owned, static_cast<std::size_t>(entrance_begin + i * record_size)));
    return result;
}

} // namespace okage
