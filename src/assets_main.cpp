#include "okage/assets.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > 128 * 1024 * 1024) throw std::runtime_error("Input exceeds 128 MiB limit");
    std::vector<std::uint8_t> bytes(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}
void write(const fs::path& path, okage::Bytes bytes) {
    if (fs::is_symlink(path)) throw std::runtime_error("Refusing symlink output");
    if (fs::exists(path)) {
        const auto old = read(path);
        if (old.size() != bytes.size() || !std::equal(old.begin(), old.end(), bytes.begin()))
            throw std::runtime_error("Refusing different existing output: " + path.string());
        return;
    }
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot write " + path.string());
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("Usage: okage-assets list ARCHIVE | extract ARCHIVE OUTDIR | decode PACKED OUTFILE | texture XIM PAMFILE");
        const std::string command = argv[1];
        if (command == "texture-check" && argc == 3) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
                auto extension = entry.path().extension().string();
                if (entry.is_regular_file() && (extension == ".xim" || extension == ".XIM")) files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& path : files) {
                try {
                    const auto texture = okage::decode_xim(read(path));
                    std::cout << path.string() << '\t' << texture.width << '\t' << texture.height << '\t'
                              << texture.pixel_format << '\t' << texture.palette_format << '\n';
                } catch (const std::exception& e) { throw std::runtime_error(path.string() + ": " + e.what()); }
            }
            return 0;
        }
        const auto bytes = read(argv[2]);
        if (command == "texture" && argc == 4) {
            const auto texture = okage::decode_xim(bytes);
            const auto header = "P7\nWIDTH " + std::to_string(texture.width) + "\nHEIGHT " + std::to_string(texture.height) +
                                "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
            std::vector<std::uint8_t> pam(header.begin(), header.end());
            pam.insert(pam.end(), texture.rgba.begin(), texture.rgba.end());
            write(argv[3], pam);
            std::cout << texture.width << '\t' << texture.height << '\t' << texture.pixel_format << '\t' << texture.palette_format << '\n';
        } else if (command == "decode" && argc == 4) {
            auto result = okage::decompress_xpf(bytes);
            write(argv[3], result.bytes);
            std::cout << result.bytes.size() << '\t' << result.consumed << '\n';
        } else if ((command == "list" && argc == 3) || (command == "extract" && argc == 4)) {
            const auto entries = okage::read_xpf(bytes);
            for (const auto& entry : entries) {
                auto result = okage::decompress_xpf(okage::Bytes(bytes).subspan(entry.offset, entry.packed_size));
                if (command == "extract") write(fs::path(argv[3]) / entry.name, result.bytes);
                std::cout << entry.name << '\t' << entry.offset << '\t' << entry.packed_size
                          << '\t' << result.bytes.size() << '\t' << result.consumed << '\n';
            }
        } else throw std::runtime_error("Invalid command or arguments");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "okage-assets: " << e.what() << '\n';
        return 1;
    }
}
