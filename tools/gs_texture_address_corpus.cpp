#include "okage/gs_texture_address.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t corpus_magic = 0x52444147u; // "GADR" as LE bytes.
constexpr std::uint32_t corpus_version = 1;
constexpr std::uint32_t input_stride_words = 5;

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
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("cannot read input corpus");
    return data;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || (!data.empty() && !stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))))
        throw std::runtime_error("cannot write output corpus");
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: gs-texture-address-corpus INPUT.gadr OUTPUT.bin\n";
        return 2;
    }
    try {
        const auto input = read_file(argv[1]);
        if (input.size() < 16) throw std::runtime_error("short GADR header");
        const auto magic = u32le(input.data());
        const auto version = u32le(input.data() + 4);
        const auto count = u32le(input.data() + 8);
        const auto stride = u32le(input.data() + 12);
        if (magic != corpus_magic) throw std::runtime_error("wrong GADR magic");
        if (version != corpus_version) throw std::runtime_error("unsupported GADR version");
        if (stride != input_stride_words) throw std::runtime_error("GADR row stride must be five words");
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
        write_file(argv[2], output);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gs-texture-address-corpus: " << e.what() << '\n';
        return 1;
    }
}
