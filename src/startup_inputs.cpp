#include "okage/startup_inputs.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "okage/sha256.hpp"
#include "okage/startup_textures.hpp"

namespace okage {
namespace {

constexpr const char* kElfSha256 = "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
constexpr const char* kPayloadSha256 = "2ea415986ee6ffc70ebf023d6324670a18967408615dd04455a23cf49cd9a31c";
constexpr const char* kEfdSha256 = "2c99993d5ac51760c426b4b157fb65c1348c85d09ef2552fc47e5241b3e43c6c";
constexpr const char* kVirtualAddress = "0x2003f8";
constexpr std::uint64_t kVirtualAddressValue = 0x2003f8;
constexpr std::size_t kPayloadSize = 64;
constexpr const char* kManifestPath = "NATIVE_STARTUP/startup-inputs.json";
constexpr const char* kPayloadPath = "NATIVE_STARTUP/FONTCLUT.BIN";
constexpr const char* kEfdPath = "CMNDATA/2D/TES.EFD";
constexpr std::uintmax_t kMaxManifestBytes = 1u << 20;
constexpr std::uintmax_t kMaxInputBytes = 64u << 20;

struct Pin {
    const char* name;
    const char* sha256;
};

// the startup file manifest (file sha256), source order IQ24, BMUI, BM.
constexpr std::array<Pin, 3> kFontPins{{
    {"IQ24.FNT", "f38eb6266c02315dda5b6a4f97d1dab347ef67c7b262f23deb4a3575d4cadc7a"},
    {"BMUI.FNT", "ebf4eb1f157638baf3ad9002e9990acb24b3807f54653a8c34a8d4e2ef5df356"},
    {"BM.FNT", "7bc478129d2e46991cab24b563ff02af0e0c5b32294f70bb475cf5a31e5241fb"},
}};

// the authored startup file order (file sha256), authored order of 0x221bf8.
constexpr std::array<Pin, 31> kEffectPins{{
    {"niji.xim", "b86509274c38f73e3c811402fd43ce1a434a5a9687ff8bc0b00f5a8511eb6b03"},
    {"splash_a.xim", "b133efc1b4a928478d573ea79b3f78db6c0c9298e0d323337aaa01846997d6af"},
    {"wat_4.xim", "90acd2d41837a60b50773a694fd60c3dfc828b4241dc636d3f8f348d52ad1ae6"},
    {"exp9.xim", "0f75f7b7bd0b3c218cdaaeb482bdaafe6ff7e999c6b7d986e1fbdbbc2da3df46"},
    {"f2.xim", "bea1730e29afffe4f97f9f229fc679f85148e57b91e74a96870c404f93aa7a3e"},
    {"h29_4.xim", "a683d6bfbe5fef31a398cb762443520271367ef14509dcae0dbdf2d706ae9717"},
    {"hit!.xim", "4ca368c7882ba6c67fb31b77fd2abc8ef48c6ca991c93f658a618b9dadabcb26"},
    {"spark.xim", "b8d181a1157a6aaa1ddf7653a959eb82ebfab3dad1fbb6c905ba1b3d22240e24"},
    {"smoke_4.xim", "52f59a2114feab46c535f56d729e33194b748fbaeb08ce3b8a3c1c29d3629a63"},
    {"thunder.xim", "82283a13f1cfaf107d740540f84d8c48b7e3bfb9de7c4a182ae51b8f67d39e80"},
    {"oldskilt4.xim", "473ab019281a39cba4b43e1043010d6cf6c02f21af658974902943da392fd6ad"},
    {"water.xim", "a550cb4932f2bc8a5406361c42ec90467b2689d143126e927479338785d0bd3b"},
    {"suuji.xim", "83e0896ecca63c95ea69858d7e93ce9ada5a5198880ae8e0c29ae2148e9d4fef"},
    {"lgtballm4.xim", "81d1ee7bddd13c218cf3a85672e76b5e673c895acf44b1251a52ba64accc7b5b"},
    {"fuki32b.xim", "f4362dc85edeb174ba4d012d2cad6b666bfef3f420e05366352a8b08306195a4"},
    {"all.xim", "e35e6d02436cde092dc26cffd3340cdfa5119173ffe02f080797b288d9fd1947"},
    {"all2.xim", "af4d452b82ceec1e21a01aa2885aedab6453b281e3005fd44b92c299e5ae0e6a"},
    {"kemuri.xim", "c04b3c2219b95ce79b524afa5c2d7cf9f363b4d51d6459bfadb00a118c4ccb4a"},
    {"thunderball.xim", "ec934adf9c3e0fcfa3365e1e1af1f404471d2a6266df582f1d9352ad41b39086"},
    {"6eff01.xim", "57fc8e2e47a985d35ba5531058bd5b490a5d268498122393525f56caa94ac61d"},
    {"6eff02.xim", "f67a1bdd275a54284539246e89cfb2f138bcd9f59c12fb3343867aec5c648d33"},
    {"5sen.xim", "a6f4aabe8909a195ff37735efaf72001287c37958f5c68fd4d6cd498b42a775b"},
    {"exp_c.xim", "bf48faf54ce36ea1af13844e715c4b0123e713675e2cb5e56610924a3c3cd724"},
    {"heart.xim", "821a7e98c4774c2bc00f8a0786072032bab3967100c3136279797d3506de15a0"},
    {"onnpu.xim", "b9294c102f58b85d552145aedada0ffffdad90abffdfb0a14d344ccbb2e58a1c"},
    {"wind.xim", "16354ff48e492e46dec87ba99d85295c210efe7ea7daac63ec6a0f7423d0eab5"},
    {"treasure.xim", "e9ccaa79db2df0a8c6b0858ee41415c8b0e81be45c3d034622f5947d51710af1"},
    {"page.xim", "869aae415f2ec8ed6a18b7b874dbf3e5054b57cb1fba7630403da35da5bc641b"},
    {"odoro.xim", "1b19fb7af3dab17670a9873ea0d70902a40e9e2bb374e0d5ebc53f02667b5503"},
    {"aura.xim", "9b789518dc328b3462e3bc406e66ce61c18d639ae6c4464450e0d79d2daece4a"},
    {"kyusyu16.xim", "fc34044f98269d70c193a38bdbaad4ceef3e221ce3f351b69d6ff78699cc883a"},
}};

[[noreturn]] void fail(const std::string& file, const std::string& problem) {
    throw StartupInputError(file + ": " + problem +
                            ". Regenerate the startup inputs: run tools/import_startup_inputs.py with the owned "
                            "SCUS_971.29, fonts, ENV_TEX.XPF members and TES.EFD, then tools/stage_game_data.py "
                            "into a fresh root (see the startup input RNG contract)");
}

struct Value {
    enum class Kind { Object, String, Number } kind{Kind::Object};
    std::string text;
    std::uint64_t number{0};
    std::vector<std::pair<std::string, Value>> members;
};

class Parser {
public:
    Parser(std::string_view text, std::string file) : text_(text), file_(std::move(file)) {}

    Value parse() {
        Value v = value(0);
        skip();
        if (pos_ != text_.size()) error("trailing data after the JSON document");
        return v;
    }

private:
    [[noreturn]] void error(const std::string& what) const {
        fail(file_, "invalid manifest JSON at byte " + std::to_string(pos_) + " (" + what + ")");
    }
    void skip() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t'))
            ++pos_;
    }
    bool take(char c) {
        skip();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    std::string string_literal() {
        // Opening quote already consumed.
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) error("unterminated string");
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20 || c >= 0x7f || c == '\\') error("unsupported character in string");
            out.push_back(static_cast<char>(c));
        }
    }
    Value value(int depth) {
        if (depth > 6) error("nesting too deep");
        skip();
        if (pos_ >= text_.size()) error("unexpected end of document");
        Value v;
        const char c = text_[pos_];
        if (c == '{') {
            ++pos_;
            v.kind = Value::Kind::Object;
            if (take('}')) return v;
            while (true) {
                if (!take('"')) error("expected a string key");
                std::string key = string_literal();
                for (const auto& m : v.members)
                    if (m.first == key) error("duplicate key \"" + key + "\"");
                if (!take(':')) error("expected ':'");
                v.members.emplace_back(std::move(key), value(depth + 1));
                if (take(',')) continue;
                if (take('}')) return v;
                error("expected ',' or '}'");
            }
        }
        if (c == '"') {
            ++pos_;
            v.kind = Value::Kind::String;
            v.text = string_literal();
            return v;
        }
        if (c >= '0' && c <= '9') {
            v.kind = Value::Kind::Number;
            if (c == '0' && pos_ + 1 < text_.size() && text_[pos_ + 1] >= '0' && text_[pos_ + 1] <= '9')
                error("leading zero");
            std::uint64_t n = 0;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                const auto digit = static_cast<std::uint64_t>(text_[pos_++] - '0');
                if (n > (UINT64_MAX - digit) / 10) error("number too large");
                n = n * 10 + digit;
            }
            if (pos_ < text_.size() && (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E'))
                error("only integers are allowed");
            v.number = n;
            return v;
        }
        error("unexpected token");
    }

    std::string_view text_;
    std::string file_;
    std::size_t pos_{0};
};

const Value& object_at(const Value& parent, const std::string& key, const std::string& file) {
    for (const auto& m : parent.members)
        if (m.first == key) {
            if (m.second.kind != Value::Kind::Object) fail(file, "manifest key \"" + key + "\" must be an object");
            return m.second;
        }
    fail(file, "manifest is missing key \"" + key + "\"");
}

const Value& field(const Value& parent, const std::string& key, Value::Kind kind, const std::string& file) {
    for (const auto& m : parent.members)
        if (m.first == key) {
            if (m.second.kind != kind) fail(file, "manifest key \"" + key + "\" has the wrong type");
            return m.second;
        }
    fail(file, "manifest is missing key \"" + key + "\"");
}

// Exact key set: every expected key present, no others.
void expect_keys(const Value& object, std::initializer_list<const char*> keys, const std::string& where,
                 const std::string& file) {
    for (const auto& m : object.members) {
        bool known = false;
        for (const char* k : keys) known = known || m.first == k;
        if (!known) fail(file, "manifest has unknown key \"" + m.first + "\" in " + where);
    }
    for (const char* k : keys) {
        bool found = false;
        for (const auto& m : object.members) found = found || m.first == k;
        if (!found) fail(file, std::string("manifest is missing key \"") + k + "\" in " + where);
    }
}

bool is_hex_sha256(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& root, const std::string& relative,
                                    std::uintmax_t limit) {
    const auto path = root / relative;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        fail(relative, "required file is missing or not a regular file (looked for " + path.string() + ")");
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit) fail(relative, "file size is unreadable or exceeds the sanity limit");
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!stream || static_cast<std::uintmax_t>(stream.gcount()) != size)
        fail(relative, "file could not be read completely");
    return bytes;
}

// Reads one hash-bound file: actual bytes must match the manifest value and the compiled-in pin.
std::vector<std::uint8_t> read_bound(const std::filesystem::path& root, const std::string& relative,
                                     const std::string& manifest_hash, const char* pinned) {
    auto bytes = read_file(root, relative, kMaxInputBytes);
    const auto actual = sha256_hex(bytes);
    if (actual != pinned)
        fail(relative, "file SHA256 " + actual + " does not match the pinned value " + pinned +
                           " (corrupt or modified data)");
    if (manifest_hash != pinned)
        fail(relative, "manifest records SHA256 " + manifest_hash + " but the pinned value is " + pinned);
    return bytes;
}

} // namespace

StartupInputs load_startup_inputs(const std::filesystem::path& root) {
    const std::string manifest_name = kManifestPath;
    {
        const auto& names = startup_effect_names();
        for (std::size_t i = 0; i < kEffectPins.size(); ++i)
            if (std::strcmp(names[i], kEffectPins[i].name) != 0)
                fail(manifest_name, "internal error: pinned effect order differs from startup_effect_names()");
    }
    const auto manifest_bytes = read_file(root, manifest_name, kMaxManifestBytes);
    const Value doc = Parser(std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()),
                                              manifest_bytes.size()),
                             manifest_name)
                          .parse();
    if (doc.kind != Value::Kind::Object) fail(manifest_name, "manifest must be a JSON object");
    expect_keys(doc, {"schema", "version", "source", "payload", "inputs", "importer"}, "the top level",
                manifest_name);
    if (field(doc, "schema", Value::Kind::String, manifest_name).text != "okage.startup-inputs")
        fail(manifest_name, "unexpected manifest schema");
    if (field(doc, "version", Value::Kind::Number, manifest_name).number != 1)
        fail(manifest_name, "unsupported manifest version (expected 1)");

    const Value& source = object_at(doc, "source", manifest_name);
    expect_keys(source, {"elf_sha256", "virtual_address", "file_offset", "size", "segment"}, "source",
                manifest_name);
    if (field(source, "elf_sha256", Value::Kind::String, manifest_name).text != kElfSha256)
        fail(manifest_name, std::string("source ELF identity is not the pinned SCUS_971.29 ") + kElfSha256);
    if (field(source, "virtual_address", Value::Kind::String, manifest_name).text != kVirtualAddress)
        fail(manifest_name, std::string("source virtual_address must be ") + kVirtualAddress);
    if (field(source, "size", Value::Kind::Number, manifest_name).number != kPayloadSize)
        fail(manifest_name, "source size must be 64");
    const auto file_offset = field(source, "file_offset", Value::Kind::Number, manifest_name).number;
    const Value& segment = object_at(source, "segment", manifest_name);
    expect_keys(segment, {"index", "p_offset", "p_vaddr", "p_filesz", "p_memsz"}, "source.segment", manifest_name);
    field(segment, "index", Value::Kind::Number, manifest_name);
    const auto p_offset = field(segment, "p_offset", Value::Kind::Number, manifest_name).number;
    const auto p_vaddr = field(segment, "p_vaddr", Value::Kind::Number, manifest_name).number;
    const auto p_filesz = field(segment, "p_filesz", Value::Kind::Number, manifest_name).number;
    const auto p_memsz = field(segment, "p_memsz", Value::Kind::Number, manifest_name).number;
    if (p_vaddr > kVirtualAddressValue || p_filesz > p_memsz || p_filesz > 0xffffffffull ||
        p_offset > 0xffffffffull || p_vaddr + p_filesz < kVirtualAddressValue + kPayloadSize ||
        file_offset != p_offset + (kVirtualAddressValue - p_vaddr))
        fail(manifest_name, "source segment does not contain the payload at the recorded file offset");

    const Value& payload = object_at(doc, "payload", manifest_name);
    expect_keys(payload, {kPayloadPath}, "payload", manifest_name);
    const std::string payload_hash = field(payload, kPayloadPath, Value::Kind::String, manifest_name).text;
    if (!is_hex_sha256(payload_hash)) fail(manifest_name, "payload hash is malformed");

    const Value& inputs = object_at(doc, "inputs", manifest_name);
    std::vector<std::string> expected_paths;
    for (const auto& p : kFontPins) expected_paths.push_back(std::string("CMNDATA/FONT/") + p.name);
    for (const auto& p : kEffectPins) expected_paths.push_back(std::string("CMNDATA/2D/ENV_TEX.XPF/") + p.name);
    expected_paths.push_back(kEfdPath);
    if (inputs.members.size() != expected_paths.size())
        fail(manifest_name, "manifest inputs must list exactly " + std::to_string(expected_paths.size()) +
                                " files, found " + std::to_string(inputs.members.size()));
    for (const auto& m : inputs.members)
        if (std::find(expected_paths.begin(), expected_paths.end(), m.first) == expected_paths.end())
            fail(manifest_name, "manifest has unknown input \"" + m.first + "\"");
    std::vector<std::string> hashes;
    for (const auto& path : expected_paths) {
        const auto& v = field(inputs, path, Value::Kind::String, manifest_name);
        if (!is_hex_sha256(v.text)) fail(manifest_name, "input \"" + path + "\" has a malformed SHA256");
        hashes.push_back(v.text);
    }

    const Value& importer = object_at(doc, "importer", manifest_name);
    expect_keys(importer, {"tool", "tool_sha256"}, "importer", manifest_name);
    if (field(importer, "tool", Value::Kind::String, manifest_name).text != "tools/import_startup_inputs.py")
        fail(manifest_name, "unexpected importer tool");
    if (!is_hex_sha256(field(importer, "tool_sha256", Value::Kind::String, manifest_name).text))
        fail(manifest_name, "importer tool_sha256 is malformed");

    // Actual bytes against both the manifest and the compiled-in pins.
    StartupInputs out;
    out.font_palette = read_bound(root, kPayloadPath, payload_hash, kPayloadSha256);
    if (out.font_palette.size() != kPayloadSize) fail(kPayloadPath, "payload must be exactly 64 bytes");
    std::size_t index = 0;
    for (std::size_t i = 0; i < kFontPins.size(); ++i, ++index)
        out.fonts[i] = read_bound(root, expected_paths[index], hashes[index], kFontPins[i].sha256);
    for (std::size_t i = 0; i < kEffectPins.size(); ++i, ++index)
        out.effects[i] = read_bound(root, expected_paths[index], hashes[index], kEffectPins[i].sha256);
    out.efd = read_bound(root, expected_paths[index], hashes[index], kEfdSha256);
    return out;
}

} // namespace okage
