#include "okage/input_log.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

#include "okage/sha256.hpp"

namespace okage {
namespace {
std::mutex log_mutex;
std::ofstream log_stream;
std::atomic<bool> log_enabled{false};

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            static const char* hex = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else out += char(c);
    }
    return out + "\"";
}
} // namespace

void enable_input_log(const std::filesystem::path& path) {
    std::lock_guard lock(log_mutex);
    if (log_enabled) throw std::runtime_error("Input log already enabled");
    log_stream.open(path, std::ios::binary | std::ios::trunc);
    if (!log_stream) throw std::runtime_error("Cannot open input log: " + path.string());
    log_enabled = true;
}

bool input_log_enabled() { return log_enabled; }

void record_input_read(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    if (!log_enabled) return;
    const auto digest = sha256_hex(bytes);
    std::error_code ec;
    const auto opened = std::filesystem::absolute(path, ec);
    const auto resolved = std::filesystem::canonical(path, ec);
    std::lock_guard lock(log_mutex);
    log_stream << "{\"path\":" << json_string(opened.string()) << ",\"resolved\":"
               << json_string(ec ? std::string() : resolved.string()) << ",\"bytes\":" << bytes.size()
               << ",\"sha256\":\"" << digest << "\"}\n";
    log_stream.flush();
    if (!log_stream) throw std::runtime_error("Input log write failed");
}

} // namespace okage
