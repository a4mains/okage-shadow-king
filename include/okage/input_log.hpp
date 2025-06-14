#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace okage {

void enable_input_log(const std::filesystem::path& path);
bool input_log_enabled();
void record_input_read(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

} // namespace okage
