#include "okage/assets.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Buf = std::vector<std::uint8_t>;

static std::string sha256_hex(const Buf& data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), CC_LONG(data.size()), digest);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : digest) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}
static std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += char(c);
    }
    return out + '"';
}
static bool is_xim(const fs::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e == ".xim";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: okage-xim-inventory <file-or-directory>...\n"; return 2; }
    std::set<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const fs::path root = fs::path(argv[i]).lexically_normal();
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) { paths.insert(root.generic_string()); continue; }
        if (!fs::is_directory(root, ec)) { std::cerr << "not a file or directory: " << argv[i] << '\n'; return 2; }
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) { std::cerr << "directory error under " << argv[i] << ": " << ec.message() << '\n'; return 2; }
            if (it->is_regular_file() && is_xim(it->path())) paths.insert(it->path().generic_string());
        }
    }
    std::map<std::string, std::size_t> by_status, by_format, by_levels, by_colors, by_error;
    std::set<std::string> unique_hashes;
    std::size_t total = 0, parsed = 0, decoder_accepts = 0, decoder_disagrees = 0;
    std::ostringstream files;
    for (const auto& path : paths) {
        std::ifstream in(path, std::ios::binary);
        if (!in) { std::cerr << "cannot open " << path << '\n'; return 2; }
        const Buf data((std::istreambuf_iterator<char>(in)), {});
        const auto hash = sha256_hex(data);
        unique_hashes.insert(hash);
        files << (total++ ? ",\n" : "") << "    {\"path\":" << json_string(path)
              << ",\"sha256\":\"" << hash << "\",\"size\":" << data.size();
        bool decodes = false;
        try { (void)okage::decode_xim(data); decodes = true; } catch (const std::runtime_error&) {}
        decoder_accepts += decodes;
        try {
            const auto l = okage::parse_xim_layout(data);
            const auto u = okage::xim_upload_support(l);
            ++parsed;
            const std::string status = okage::to_string(u.status);
            files << ",\"parsed\":true,\"psm\":" << l.pixel_format << ",\"cpsm\":" << l.palette_format
                  << ",\"width\":" << l.width << ",\"height\":" << l.height
                  << ",\"palette_levels\":" << l.palette_level_count << ",\"palette_colors\":" << l.palette_color_count
                  << ",\"upload_supported\":" << (u.status == okage::XimUploadStatus::Supported ? "true" : "false")
                  << ",\"classification\":\"" << status << "\",\"reason\":" << json_string(u.reason);
            ++by_status[status];
            ++by_format["psm" + std::to_string(l.pixel_format) + "/cpsm" + std::to_string(l.palette_format)];
            ++by_levels[std::to_string(l.palette_level_count)];
            ++by_colors[std::to_string(l.palette_color_count)];
            if (!decodes) ++decoder_disagrees;
        } catch (const std::runtime_error& e) {
            files << ",\"parsed\":false,\"classification\":\"ParseError\",\"error\":" << json_string(e.what());
            ++by_status["ParseError"];
            ++by_error[e.what()];
            if (decodes) ++decoder_disagrees;
        }
        files << ",\"decode_xim_accepts\":" << (decodes ? "true" : "false") << "}";
    }
    auto dump = [](const std::map<std::string, std::size_t>& m) {
        std::string out = "{";
        bool first = true;
        for (const auto& [k, v] : m) { out += (first ? "" : ",") + json_string(k) + ":" + std::to_string(v); first = false; }
        return out + "}";
    };
    std::cout << "{\n  \"total\":" << total << ",\n  \"unique_sha256\":" << unique_hashes.size()
              << ",\n  \"parsed\":" << parsed << ",\n  \"decode_xim_accepts\":" << decoder_accepts
              << ",\n  \"parser_decoder_disagreements\":" << decoder_disagrees
              << ",\n  \"by_classification\":" << dump(by_status)
              << ",\n  \"by_format\":" << dump(by_format)
              << ",\n  \"by_palette_levels\":" << dump(by_levels)
              << ",\n  \"by_palette_colors\":" << dump(by_colors)
              << ",\n  \"by_parse_error\":" << dump(by_error)
              << ",\n  \"files\":[\n" << files.str() << "\n  ]\n}\n";
}
