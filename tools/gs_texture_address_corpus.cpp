#include "okage/gs_texture_address.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
constexpr std::uint32_t corpus_magic = 0x52444147u; // "GADR" as LE bytes.
constexpr std::uint32_t corpus_version = 1;
constexpr std::uint32_t input_stride_words = 5;
constexpr std::uint32_t maximum_rows = 1'000'000;
constexpr std::uint64_t maximum_input_bytes = 16u + std::uint64_t{maximum_rows} * 20u;

std::uint32_t u32le(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void append_u32le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open input corpus");
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0) throw std::runtime_error("cannot size input corpus");
    if (static_cast<std::uint64_t>(end) > maximum_input_bytes)
        throw std::runtime_error("input corpus exceeds 1,000,000-row limit");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("cannot read input corpus");
    return data;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST) throw std::runtime_error("output path already exists");
        throw std::runtime_error("cannot create output corpus: " + std::string(std::strerror(errno)));
    }
    std::size_t written = 0;
    while (written < data.size()) {
        const auto result = ::write(fd, data.data() + written, data.size() - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            const int error = result == 0 ? EIO : errno;
            ::close(fd);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw std::runtime_error("cannot write output corpus: " + std::string(std::strerror(error)));
        }
        written += static_cast<std::size_t>(result);
    }
    if (::close(fd) != 0) {
        const int error = errno;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw std::runtime_error("cannot close output corpus: " + std::string(std::strerror(error)));
    }
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: gs-texture-address-corpus INPUT.gadr OUTPUT.bin\n";
        return 2;
    }
    try {
        const std::filesystem::path output_path(argv[2]);
        const auto input = read_file(argv[1]);
        if (input.size() < 16) throw std::runtime_error("short GADR header");
        const auto magic = u32le(input.data());
        const auto version = u32le(input.data() + 4);
        const auto count = u32le(input.data() + 8);
        const auto stride = u32le(input.data() + 12);
        if (magic != corpus_magic) throw std::runtime_error("wrong GADR magic");
        if (version != corpus_version) throw std::runtime_error("unsupported GADR version");
        if (stride != input_stride_words) throw std::runtime_error("GADR row stride must be five words");
        if (count == 0 || count > maximum_rows)
            throw std::runtime_error("GADR row count must be between 1 and 1,000,000");
        const auto expected_size = std::uint64_t{16} + std::uint64_t{count} * 20u;
        if (expected_size != input.size())
            throw std::runtime_error("GADR row count/extent mismatch");

        std::vector<std::uint8_t> output;
        output.reserve(std::size_t(count) * 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto* row = input.data() + 16 + std::size_t(i) * 20;
            const auto psm = u32le(row);
            const auto tbp = u32le(row + 4);
            const auto tbw = u32le(row + 8);
            const auto x = u32le(row + 12);
            const auto y = u32le(row + 16);
            const auto address = okage::gs_texture_byte_address(psm, tbp, tbw, x, y);
            if (!address) throw std::runtime_error("unsupported parameter in GADR row " + std::to_string(i));
            append_u32le(output, *address);
        }
        write_file(output_path, output);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gs-texture-address-corpus: " << e.what() << '\n';
        return 1;
    }
}
