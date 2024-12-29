#include "okage/texture_memory.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open TXAL input");
    const auto end = input.tellg();
    if (end < 0 || end > 16 + 8192 * 24)
        throw std::runtime_error("TXAL input exceeds corpus bound");
    Bytes bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("short TXAL input");
    return bytes;
}

std::uint32_t word(const Bytes& bytes, std::size_t index) {
    const auto offset = index * 4;
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("truncated TXAL word");
    return std::uint32_t(bytes[offset]) |
        (std::uint32_t(bytes[offset + 1]) << 8) |
        (std::uint32_t(bytes[offset + 2]) << 16) |
        (std::uint32_t(bytes[offset + 3]) << 24);
}

std::string sha256(std::span<const std::uint8_t> bytes) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) out << std::setw(2) << unsigned(byte);
    return out.str();
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void run(const std::filesystem::path& input_path,
         const std::filesystem::path& output_path) {
    require(!std::filesystem::exists(output_path) &&
            !std::filesystem::is_symlink(output_path),
            "TXAL output must be fresh");
    const auto bytes = read_file(input_path);
    require(bytes.size() >= 16 && bytes.size() % 4 == 0 &&
            word(bytes, 0) == 0x4c415854 && word(bytes, 1) == 1 &&
            word(bytes, 2) > 0 && word(bytes, 2) <= 8192 && word(bytes, 3) == 6 &&
            bytes.size() == 16 + std::size_t(word(bytes, 2)) * 24,
            "TXAL header/version/row extent mismatch");

    std::array<std::uint64_t, 8192> descriptors{};
    std::array<bool, 8192> live{};
    std::array<bool, 8192> declared{};
    std::unique_ptr<okage::Ps2TextureMemory> memory;
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create TXAL output");
    output << "{\"schema\":\"okage-texture-allocator-v1\",\"input_sha256\":\""
           << sha256(bytes) << "\",\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\","
           << "\"scope\":\"Native source-derived block allocator model; synthetic operation sequence only, no GS transfers or retail lifecycle proof\",\"rows\":"
           << word(bytes, 2) << "}\n";

    for (std::uint32_t i = 0; i < word(bytes, 2); ++i) {
        const auto at = 4 + std::size_t(i) * 6;
        const auto op = word(bytes, at), slot = word(bytes, at + 1);
        require(op <= 4 && slot < descriptors.size(), "TXAL operation/slot bound");
        if (op == 0) {
            require(slot == 0 && word(bytes, at + 2) == 0 && word(bytes, at + 3) == 0 &&
                    word(bytes, at + 4) == 0 && word(bytes, at + 5) == 0,
                    "TXAL initialize operands");
            memory = std::make_unique<okage::Ps2TextureMemory>();
            memory->initialize_empty_allocator();
            descriptors.fill(0);
            live.fill(false);
            declared.fill(false);
        } else {
            require(memory != nullptr, "TXAL operation before initialize");
            if (op == 1) {
                require(word(bytes, at + 2) < 4 &&
                        okage::is_supported_gs_texture_format(word(bytes, at + 3)) &&
                        word(bytes, at + 4) > 0 && word(bytes, at + 4) <= 1024 &&
                        word(bytes, at + 5) > 0 && word(bytes, at + 5) <= 1024,
                        "TXAL allocation arguments");
                require(!live[slot], "TXAL allocation overwrites live slot");
                const auto psm = static_cast<okage::GsPixelStorage>(word(bytes, at + 3));
                const auto allocation = memory->allocate(
                    static_cast<std::uint8_t>(word(bytes, at + 2)), psm,
                    static_cast<std::uint16_t>(word(bytes, at + 4)),
                    static_cast<std::uint16_t>(word(bytes, at + 5)));
                descriptors[slot] = allocation ? allocation->descriptor : 0;
                live[slot] = descriptors[slot] != 0;
                declared[slot] = true;
            } else {
                require(declared[slot], "TXAL slot has no prior allocation attempt");
                require(word(bytes, at + 3) == 0 && word(bytes, at + 4) == 0 &&
                        word(bytes, at + 5) == 0 && (op == 2 || word(bytes, at + 2) == 0),
                        "TXAL operation trailing operands");
                require(live[slot] || descriptors[slot] == 0,
                        "TXAL operation uses stale descriptor");
                const auto delta = std::bit_cast<std::int32_t>(word(bytes, at + 2));
                if (op == 2) {
                    require(delta >= -65535 && delta <= 65535, "TXAL ref delta bound");
                    if (descriptors[slot]) {
                        const auto id = static_cast<okage::TextureAllocationId>(
                            (descriptors[slot] >> 20) & 0x0fff);
                        memory->adjust_references(id, delta);
                    }
                } else if (op == 3) {
                    if (descriptors[slot]) {
                        const auto id = static_cast<okage::TextureAllocationId>(
                            (descriptors[slot] >> 20) & 0x0fff);
                        if (memory->free_allocation(id)) live[slot] = false;
                    }
                }
            }
        }

        const auto descriptor = descriptors[slot];
        std::uint16_t references = 0;
        if (descriptor) {
            const auto allocation = memory->allocation(static_cast<okage::TextureAllocationId>(
                (descriptor >> 20) & 0x0fff));
            if (allocation) references = allocation->references;
        }
        const auto occupancy = memory ? memory->occupancy_bytes() :
            std::array<std::uint8_t, 8192>{};
        output << "{\"row\":" << i << ",\"op\":" << op << ",\"slot\":" << slot
               << ",\"descriptor\":[" << std::uint32_t(descriptor) << ','
               << std::uint32_t(descriptor >> 32) << "],\"live\":"
               << (live[slot] ? "true" : "false") << ",\"available\":"
               << (memory ? memory->available_count() : 0) << ",\"references\":" << references
               << ",\"occupancy_sha256\":\"" << sha256(occupancy) << "\",\"cursors\":[";
        if (memory) {
            const auto& cursors = memory->search_cursors();
            for (std::size_t j = 0; j < cursors.size(); ++j)
                output << (j ? "," : "") << cursors[j];
        } else {
            for (std::size_t j = 0; j < 64; ++j) output << (j ? ",0" : "0");
        }
        output << "]}\n";
        if (!output) throw std::runtime_error("TXAL output write failed");
    }
    output << "{\"model\":\"source-metadata-only\"}\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: okage-texture-memory-corpus INPUT.bin OUTPUT.jsonl");
        run(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "okage-texture-memory-corpus: " << error.what() << '\n';
        return 1;
    }
}
