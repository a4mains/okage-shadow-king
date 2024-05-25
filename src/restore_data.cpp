#include "okage/restore_data.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'O','K','R','S','T','1',0,0};
constexpr unsigned header_size = 88;
constexpr unsigned string_count = 44;
constexpr std::string_view elf_hash =
    "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
constexpr std::string_view source_hash =
    "35df7df98ca463c9e9a7e5e326bd56d4384b3cb48d770b1c08b849ad0c5a4f1f";

std::uint32_t read_word(Bytes data, std::size_t at) {
    if (at > data.size() || data.size() - at < 4)
        throw std::runtime_error("Restore data word bounds");
    return std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
           (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
}

bool digest_matches(Bytes data, std::size_t at, std::string_view expected) {
    if (at > data.size() || data.size() - at < expected.size() / 2) return false;
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto nibble = (data[at + i / 2] >> (i % 2 ? 0 : 4)) & 0xf;
        if (expected[i] != digits[nibble]) return false;
    }
    return true;
}
} // namespace

RestoreUiData::RestoreUiData(Bytes data) {
    if (data.size() < header_size || !std::equal(magic.begin(), magic.end(), data.begin()) ||
        read_word(data, 8) != 1 || read_word(data, 12) != 28 ||
        read_word(data, 16) != 1 || read_word(data, 20) != string_count)
        throw std::runtime_error("Unsupported Restore data format");
    if (!digest_matches(data, 24, elf_hash) || !digest_matches(data, 56, source_hash))
        throw std::runtime_error("Unsupported Restore data source revision");

    constexpr std::size_t row_at = header_size;
    for (unsigned i = 0; i < definition.words.size(); ++i)
        definition.words[i] = read_word(data, row_at + i * 4);
    const auto& row = definition.words;
    if (row[0] != 1 || row[21] != 1 || row[22] != 1 || row[23] != 1 ||
        row[24] != 8 || row[25] != 17 || row[26] != 16 || row[27] != 0)
        throw std::runtime_error("Unsupported Restore row binding or layout");

    std::size_t at = row_at + row.size() * 4;
    atlas.reserve(1);
    if (at > data.size() || data.size() - at < 16)
        throw std::runtime_error("Restore atlas extent");
    UiWindow::AtlasTuple terminator{};
    for (unsigned word = 0; word < terminator.size(); ++word)
        terminator[word] = read_word(data, at + word * 4);
    if (terminator != UiWindow::AtlasTuple{})
        throw std::runtime_error("Restore atlas terminator differs");
    atlas.push_back(terminator);
    at += 16;

    for (unsigned i = 0; i < strings_.size(); ++i) {
        if (at > data.size() || data.size() - at < 8)
            throw std::runtime_error("Restore string header extent");
        const auto id = read_word(data, at);
        const auto length = read_word(data, at + 4);
        at += 8;
        if (id != i + 1 || length == 0 || length > 256 ||
            length > data.size() - at)
            throw std::runtime_error("Restore string ID or extent");
        if (data[at + length - 1] != 0 ||
            std::find(data.begin() + at, data.begin() + at + length - 1, 0) != data.begin() + at + length - 1)
            throw std::runtime_error("Restore string terminator");
        strings_[i].assign(reinterpret_cast<const char*>(data.data() + at), length - 1);
        at += length;
    }
    if (at != data.size()) throw std::runtime_error("Restore data trailing bytes");
}

std::string_view RestoreUiData::string(RestoreStringId id) const {
    const auto index = static_cast<std::uint32_t>(id);
    if (index == 0 || index > strings_.size())
        throw std::runtime_error("Restore string ID bounds");
    return strings_[index - 1];
}

std::array<UiStreamCommand, 2> RestoreUiData::stream_commands() {
    return {{{14, UiStreamReference::MenuTransform1f9c00},
             {6, UiStreamReference::MenuTransform1f9c2c}}};
}

} // namespace okage
