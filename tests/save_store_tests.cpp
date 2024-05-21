#include "okage/save_store.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace okage;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F&& f, const char* message) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
template<class F> void rejects_format(F&& f, SaveFormatError::Reason expected, const char* message) {
    bool caught = false;
    try { f(); }
    catch (const SaveFormatError& error) { caught = error.reason() == expected; }
    check(caught, message);
}
struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("okage-save-store-" + std::to_string(::getpid()));
    TempDir() { std::filesystem::remove_all(path); std::filesystem::create_directory(path); }
    ~TempDir() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("write test bytes");
}
void checksum_and_envelope() {
    const std::array<std::uint8_t, 0> empty{};
    const std::array<std::uint8_t, 1> zero{0};
    const std::array<std::uint8_t, 1> one{1};
    const std::array<std::uint8_t, 8> sequence{0,1,2,3,4,5,6,7};
    check(source_save_checksum(empty) == 0xffff0000u, "source checksum empty known answer");
    check(source_save_checksum(zero) == 0xff0000ffu, "source checksum zero-byte known answer");
    check(source_save_checksum(one) == 0xff000e2eu, "source checksum one-byte known answer");
    check(source_save_checksum(sequence) == 0x27727c3du, "source checksum multi-byte known answer");

    SavePayload payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xff);
    const auto file = encode_source_save(payload);
    check(file.size() == source_save_file_size && file.size() == 0xd84, "one source save file has exact framed size");
    check(file[0] == '!' && file[1] == 'c' && file[2] == 'r' && file[3] == 'c', "source magic bytes");
    check(file[4] == 0x78 && file[5] == 0x0d && file[6] == 0 && file[7] == 0, "source block size is 0xd78 little endian");
    check(decode_source_save(file) == payload, "framed source save round trips");

    auto bad = file;
    bad[0] = 'X';
    rejects_format([&] { decode_source_save(bad); }, SaveFormatError::Reason::bad_magic, "bad magic reason preserved");
    bad = file; bad[4] ^= 1;
    rejects_format([&] { decode_source_save(bad); }, SaveFormatError::Reason::wrong_block_size, "wrong block size reason preserved");
    bad = file; bad[11] ^= 0x80;
    rejects_format([&] { decode_source_save(bad); }, SaveFormatError::Reason::checksum_mismatch, "checksum reason preserved");
    bad = file; bad.back() ^= 1;
    rejects_format([&] { decode_source_save(bad); }, SaveFormatError::Reason::checksum_mismatch, "payload corruption is a checksum failure");
    rejects_format([&] { decode_source_save(std::span<const std::uint8_t>(file).first(file.size()-1)); },
                   SaveFormatError::Reason::wrong_file_size, "truncated file reason preserved");
    bad = file; bad.push_back(0);
    rejects_format([&] { decode_source_save(bad); }, SaveFormatError::Reason::wrong_file_size, "trailing data reason preserved");
}
void native_slots_and_failures() {
    TempDir temp;
    const auto root = temp.path / "Saves";
    NativeSaveStore store(root);
    check(!store.read(0), "missing root/slot is distinguished as empty");
    rejects([&] { store.read(3); }, "unsupported slot rejected");

    SavePayload first{};
    first[0] = 0x12; first[0x510] = 0x34; first.back() = 0x56;
    store.write(0, first);
    check(std::filesystem::exists(root / "bkmo0.dat"), "slot zero uses source filename");
    check(store.read(0) == first, "native slot reads exact payload");
    check(!store.read(1), "other slots stay absent");

    SavePayload replacement{}; replacement[10] = 0xa5;
    store.write(0, replacement);
    check(store.read(0) == replacement, "atomic replacement publishes new complete payload");
    for (const auto& entry : std::filesystem::directory_iterator(root))
        check(entry.path().filename() == "bkmo0.dat", "temporary file is removed after successful rename");

    auto corrupt = read_bytes(root / "bkmo0.dat");
    corrupt[30] ^= 0x80;
    write_bytes(root / "bkmo0.dat", corrupt);
    rejects([&] { (void)store.read(0); }, "corrupt slot throws rather than appearing empty");

    std::filesystem::remove(root / "bkmo0.dat");
    std::filesystem::create_symlink(temp.path / "outside", root / "bkmo0.dat");
    rejects([&] { (void)store.read(0); }, "symlink slot is rejected");
    rejects([&] { store.write(0, first); }, "symlink destination is rejected");

    const auto bad_root = temp.path / "not-a-directory";
    { std::ofstream f(bad_root); f << "keep"; }
    NativeSaveStore bad_store(bad_root);
    rejects([&] { bad_store.write(1, first); }, "non-directory storage root is rejected");
    std::ifstream preserved(bad_root);
    std::string contents; preserved >> contents;
    check(contents == "keep", "failed storage write preserves unrelated existing data");

    const auto real_directory = temp.path / "real-save-root";
    std::filesystem::create_directory(real_directory);
    const auto linked_directory = temp.path / "linked-save-root";
    std::filesystem::create_directory_symlink(real_directory, linked_directory);
    NativeSaveStore linked_store(linked_directory);
    rejects([&] { linked_store.write(2, first); }, "symlink storage root is rejected");
}
}

int main() {
    try {
        checksum_and_envelope();
        native_slots_and_failures();
        std::cout << "Source save framing, checksum, three-slot mapping, and atomic store checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
