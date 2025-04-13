#include "okage/sha256.hpp"
#include "okage/startup_inputs.hpp"
#include "okage/startup_textures.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace okage;
namespace fs = std::filesystem;

namespace {

int skipped = 0;

void require(bool v, const std::string& m) {
    if (!v) throw std::runtime_error(m);
}

std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_text(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

std::string replace_once(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    require(at != std::string::npos, "test fixture: pattern not found: " + from);
    text.replace(at, from.size(), to);
    return text;
}

struct TempRoot {
    fs::path path;
    TempRoot() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("okage-startup-inputs-" + std::to_string(std::rand()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

const char* real_root() { return std::getenv("OKAGE_STARTUP_ROOT"); }

// Copies the 37 relevant files (symlinks resolved) into a fresh temp root.
void copy_real(const TempRoot& tmp) {
    const fs::path src = real_root();
    std::vector<std::string> files{"NATIVE_STARTUP/startup-inputs.json", "NATIVE_STARTUP/FONTCLUT.BIN",
                                   "CMNDATA/2D/TES.EFD"};
    for (const char* f : {"IQ24.FNT", "BMUI.FNT", "BM.FNT"}) files.push_back(std::string("CMNDATA/FONT/") + f);
    for (const char* n : startup_effect_names()) files.push_back(std::string("CMNDATA/2D/ENV_TEX.XPF/") + n);
    for (const auto& f : files) {
        fs::create_directories((tmp.path / f).parent_path());
        fs::copy_file(src / f, tmp.path / f);  // follows symlinks
    }
}

// Runs load and returns the error message ("" if it succeeded).
std::string load_error(const fs::path& root) {
    try {
        load_startup_inputs(root);
    } catch (const StartupInputError& e) {
        return e.what();
    }
    return "";
}

void expect_fail(const fs::path& root, const std::string& needle, const char* what) {
    const auto msg = load_error(root);
    require(!msg.empty(), std::string(what) + ": load unexpectedly succeeded");
    require(msg.find(needle) != std::string::npos, std::string(what) + ": message lacks '" + needle + "': " + msg);
    require(msg.find("tools/import_startup_inputs.py") != std::string::npos &&
                msg.find("tools/stage_game_data.py") != std::string::npos,
            std::string(what) + ": message does not say how to regenerate: " + msg);
}

void corrupt_one_byte(const fs::path& file) {
    auto text = read_text(file);
    require(!text.empty(), "fixture file is empty");
    text[text.size() / 2] = static_cast<char>(text[text.size() / 2] ^ 0x01);
    write_text(file, text);
}

#define NEED_REAL()                                                          \
    do {                                                                     \
        if (!real_root()) {                                                  \
            std::cout << "SKIP (OKAGE_STARTUP_ROOT unset): " << __func__ << '\n'; \
            ++skipped;                                                       \
            return;                                                          \
        }                                                                    \
    } while (0)

// ---- SHA-256 ---------------------------------------------------------------

void sha256_vectors() {
    require(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    const auto abc = bytes_of("abc");
    require(sha256_hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    const auto two_block = bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    require(sha256_hex(two_block) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");
    const std::vector<std::uint8_t> million(1000000, 'a');
    require(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "1M a");
    // Incremental updates with awkward chunk sizes must agree with one-shot hashing.
    Sha256 s;
    for (std::size_t at = 0, step = 1; at < million.size(); at += step, step = step % 97 + 1)
        s.update({million.data() + at, std::min(step, million.size() - at)});
    const auto d = s.finish();
    require(sha256_hex(million) ==
                [&] {
                    static const char* hex = "0123456789abcdef";
                    std::string o;
                    for (auto b : d) { o += hex[b >> 4]; o += hex[b & 15]; }
                    return o;
                }(),
            "incremental");
    // Padding boundaries 55/56/63/64/65 must match known digests.
    require(sha256_hex(std::vector<std::uint8_t>(55, 'a')) ==
                "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "55");
    require(sha256_hex(std::vector<std::uint8_t>(56, 'a')) ==
                "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "56");
    require(sha256_hex(std::vector<std::uint8_t>(64, 'a')) ==
                "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "64");
}

// ---- manifest-only failures (no valid data needed) --------------------------

void missing_manifest() {
    TempRoot tmp;
    expect_fail(tmp.path, "startup-inputs.json", "missing manifest");
}

void unparseable_manifest() {
    TempRoot tmp;
    write_text(tmp.path / "NATIVE_STARTUP/startup-inputs.json", "{not json");
    expect_fail(tmp.path, "invalid manifest JSON", "garbage manifest");
}

void empty_object_manifest() {
    TempRoot tmp;
    write_text(tmp.path / "NATIVE_STARTUP/startup-inputs.json", "{}");
    expect_fail(tmp.path, "missing key", "empty object");
}

void names_match_effect_table() {
    NEED_REAL();
    TempRoot tmp;
    copy_real(tmp);
    require(load_error(tmp.path).empty(), "pristine copy of the real root must load");
}

// ---- tests on a copy of the real root ----------------------------------------

void positive_real_root() {
    NEED_REAL();
    const auto in = load_startup_inputs(real_root());
    require(in.font_palette.size() == 64, "palette size");
    require(sha256_hex(in.font_palette) == "2ea415986ee6ffc70ebf023d6324670a18967408615dd04455a23cf49cd9a31c",
            "palette hash");
    require(!in.fonts[0].empty() && !in.fonts[1].empty() && !in.fonts[2].empty(), "fonts");
    const auto& names = startup_effect_names();
    for (std::size_t i = 0; i < in.effects.size(); ++i) {
        require(!in.effects[i].empty(), "effect empty");
        const auto on_disk = read_text(fs::path(real_root()) / "CMNDATA/2D/ENV_TEX.XPF" / names[i]);
        require(on_disk.size() == in.effects[i].size() &&
                    std::equal(on_disk.begin(), on_disk.end(), in.effects[i].begin(),
                               [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }),
                std::string("effect order/content mismatch at ") + names[i]);
    }
    require(sha256_hex(in.efd) == "2c99993d5ac51760c426b4b157fb65c1348c85d09ef2552fc47e5241b3e43c6c", "efd");
}

void missing_payload() {
    NEED_REAL();
    TempRoot tmp;
    copy_real(tmp);
    fs::remove(tmp.path / "NATIVE_STARTUP/FONTCLUT.BIN");
    expect_fail(tmp.path, "NATIVE_STARTUP/FONTCLUT.BIN", "missing payload");
}

void missing_input_file() {
    NEED_REAL();
    TempRoot tmp;
    copy_real(tmp);
    fs::remove(tmp.path / "CMNDATA/2D/ENV_TEX.XPF/aura.xim");
    expect_fail(tmp.path, "aura.xim", "missing xim");
}

void corrupt_payload() {
    NEED_REAL();
    TempRoot tmp;
    copy_real(tmp);
    corrupt_one_byte(tmp.path / "NATIVE_STARTUP/FONTCLUT.BIN");
    expect_fail(tmp.path, "NATIVE_STARTUP/FONTCLUT.BIN", "corrupt payload");
}

void corrupt_font() {
    NEED_REAL();
    for (const char* f : {"IQ24.FNT", "BMUI.FNT", "BM.FNT"}) {
        TempRoot tmp;
        copy_real(tmp);
        corrupt_one_byte(tmp.path / "CMNDATA/FONT" / f);
        expect_fail(tmp.path, f, "corrupt font");
    }
}

void corrupt_xim() {
    NEED_REAL();
    for (const char* n : {"niji.xim", "hit!.xim", "kyusyu16.xim"}) {
        TempRoot tmp;
        copy_real(tmp);
        corrupt_one_byte(tmp.path / "CMNDATA/2D/ENV_TEX.XPF" / n);
        expect_fail(tmp.path, n, "corrupt xim");
    }
}

void corrupt_efd() {
    NEED_REAL();
    TempRoot tmp;
    copy_real(tmp);
    corrupt_one_byte(tmp.path / "CMNDATA/2D/TES.EFD");
    expect_fail(tmp.path, "TES.EFD", "corrupt efd");
}

// A self-consistent manifest (hash edited to match the corrupted file) must still fail via the pins.
void self_consistent_modified_manifest() {
    NEED_REAL();
    {
        TempRoot tmp;
        copy_real(tmp);
        const auto file = tmp.path / "CMNDATA/FONT/BM.FNT";
        const auto before = sha256_hex(bytes_of(read_text(file)));
        corrupt_one_byte(file);
        const auto after = sha256_hex(bytes_of(read_text(file)));
        const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
        write_text(manifest, replace_once(read_text(manifest), before, after));
        expect_fail(tmp.path, "pinned", "font with matching edited manifest");
    }
    {
        TempRoot tmp;
        copy_real(tmp);
        const auto file = tmp.path / "CMNDATA/2D/TES.EFD";
        const auto before = sha256_hex(bytes_of(read_text(file)));
        corrupt_one_byte(file);
        const auto after = sha256_hex(bytes_of(read_text(file)));
        const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
        write_text(manifest, replace_once(read_text(manifest), before, after));
        expect_fail(tmp.path, "pinned", "efd with matching edited manifest");
    }
    {
        TempRoot tmp;
        copy_real(tmp);
        const auto file = tmp.path / "NATIVE_STARTUP/FONTCLUT.BIN";
        const auto before = sha256_hex(bytes_of(read_text(file)));
        corrupt_one_byte(file);
        const auto after = sha256_hex(bytes_of(read_text(file)));
        const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
        write_text(manifest, replace_once(read_text(manifest), before, after));
        expect_fail(tmp.path, "pinned", "payload with matching edited manifest");
    }
}

// Intact file but the manifest records a different hash.
void manifest_hash_differs_from_pin() {
    NEED_REAL();
    TempRoot tmp;
    copy_real(tmp);
    const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
    write_text(manifest, replace_once(read_text(manifest),
                                      "7bc478129d2e46991cab24b563ff02af0e0c5b32294f70bb475cf5a31e5241fb",
                                      "7bc478129d2e46991cab24b563ff02af0e0c5b32294f70bb475cf5a31e5241fc"));
    expect_fail(tmp.path, "BM.FNT", "manifest hash differs from file");
}

void manifest_edit(const char* label, const std::string& from, const std::string& to, const std::string& needle) {
    TempRoot tmp;
    copy_real(tmp);
    const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
    write_text(manifest, replace_once(read_text(manifest), from, to));
    expect_fail(tmp.path, needle, label);
}

void manifest_structure_failures() {
    NEED_REAL();
    manifest_edit("unknown top-level key", "\"schema\"", "\"extra\": 1,\n  \"schema\"", "unknown key");
    manifest_edit("wrong version", "\"version\": 1", "\"version\": 2", "version");
    manifest_edit("wrong schema", "okage.startup-inputs", "okage.other", "schema");
    manifest_edit("wrong elf hash",
                  "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5",
                  "2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa6", "ELF identity");
    manifest_edit("extra input entry", "\"inputs\": {",
                  "\"inputs\": {\n    \"CMNDATA/2D/ENV_TEX.XPF/extra.xim\": "
                  "\"0000000000000000000000000000000000000000000000000000000000000000\",",
                  "inputs must list exactly");
    manifest_edit("extra source key", "\"size\": 64,", "\"size\": 64,\n    \"note\": \"x\",", "unknown key");
    manifest_edit("wrong source size", "\"size\": 64", "\"size\": 65", "size");
    manifest_edit("wrong virtual address", "0x2003f8", "0x2003f9", "virtual_address");
    manifest_edit("inconsistent segment", "\"file_offset\": 1053688", "\"file_offset\": 1053689", "segment");
    manifest_edit("wrong importer", "tools/import_startup_inputs.py", "tools/other.py", "importer");
    manifest_edit("trailing data", "\n}\n", "\n} x", "trailing");
    // Duplicate key.
    manifest_edit("duplicate key", "\"version\": 1,", "\"version\": 1,\n  \"version\": 1,", "duplicate key");
}

void manifest_missing_and_renamed_input() {
    NEED_REAL();
    {
        TempRoot tmp;
        copy_real(tmp);
        const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
        auto text = read_text(manifest);
        const auto key = text.find("    \"CMNDATA/2D/ENV_TEX.XPF/wind.xim\"");
        require(key != std::string::npos, "fixture: wind.xim entry");
        const auto end = text.find('\n', key);
        text.erase(key, end - key + 1);
        write_text(manifest, text);
        expect_fail(tmp.path, "inputs must list exactly", "missing input entry");
    }
    {
        // Same count but an entry renamed: unknown input.
        TempRoot tmp;
        copy_real(tmp);
        const auto manifest = tmp.path / "NATIVE_STARTUP/startup-inputs.json";
        write_text(manifest, replace_once(read_text(manifest), "ENV_TEX.XPF/wind.xim", "ENV_TEX.XPF/gale.xim"));
        expect_fail(tmp.path, "unknown input", "renamed input entry");
    }
}

struct Case {
    const char* name;
    void (*fn)();
};

}  // namespace

int main() {
    const Case cases[] = {
        {"sha256_vectors", sha256_vectors},
        {"missing_manifest", missing_manifest},
        {"unparseable_manifest", unparseable_manifest},
        {"empty_object_manifest", empty_object_manifest},
        {"names_match_effect_table", names_match_effect_table},
        {"positive_real_root", positive_real_root},
        {"missing_payload", missing_payload},
        {"missing_input_file", missing_input_file},
        {"corrupt_payload", corrupt_payload},
        {"corrupt_font", corrupt_font},
        {"corrupt_xim", corrupt_xim},
        {"corrupt_efd", corrupt_efd},
        {"self_consistent_modified_manifest", self_consistent_modified_manifest},
        {"manifest_hash_differs_from_pin", manifest_hash_differs_from_pin},
        {"manifest_structure_failures", manifest_structure_failures},
        {"manifest_missing_and_renamed_input", manifest_missing_and_renamed_input},
    };
    int failed = 0;
    for (const auto& c : cases) {
        try {
            const int before = skipped;
            c.fn();
            if (skipped == before) std::cout << "ok   " << c.name << '\n';
        } catch (const std::exception& e) {
            std::cout << "FAIL " << c.name << ": " << e.what() << '\n';
            ++failed;
        }
    }
    std::cout << (failed ? "FAILED" : "PASSED") << " (" << skipped << " skipped)\n";
    return failed ? 1 : 0;
}
