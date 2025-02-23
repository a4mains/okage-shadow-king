#include "okage/texture_transfer.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace okage;

namespace {

constexpr std::uintmax_t max_fixture_bytes = 64u << 20;
constexpr unsigned max_cases = 4096;
constexpr unsigned max_dimension = 256;  // observer row limit: a,b in 1..256

struct Case {
    unsigned n{}, psm{}, w{}, h{}, rows{}, tbp{}, tbw{};
    std::uint32_t lo{}, hi{};
    bool clut{};
    std::map<std::uint32_t, unsigned> original;  // touched address -> value
};

struct FixtureError {
    std::string message;
};

[[noreturn]] void fail(const std::string& m) { throw FixtureError{m}; }

class Reader {
public:
    explicit Reader(std::istream& in) : in_(in) {}
    std::optional<std::string> tag() {
        std::string t;
        if (!(in_ >> t)) return std::nullopt;
        return t;
    }
    std::string word(const char* what) {
        std::string t;
        if (!(in_ >> t)) fail(std::string("truncated fixture: missing ") + what);
        return t;
    }
    // Strict unsigned decimal: digits only, no sign, no overflow, <= max.
    unsigned long long number(const char* what, unsigned long long max) {
        const std::string t = word(what);
        if (t.size() > 20) fail(std::string("non-numeric or oversize ") + what + ": '" + t.substr(0, 24) + "'");
        unsigned long long v = 0;
        for (char c : t) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                fail(std::string("non-numeric ") + what + ": '" + t + "'");
            const unsigned d = static_cast<unsigned>(c - '0');
            if (v > (~0ull - d) / 10) fail(std::string("overflow in ") + what + ": '" + t + "'");
            v = v * 10 + d;
        }
        if (v > max) fail(std::string(what) + " out of range: " + t);
        return v;
    }
private:
    std::istream& in_;
};

Case read_case(Reader& r) {
    Case c;
    c.n = static_cast<unsigned>(r.number("case number", 1000000));
    const std::string kind = r.word("kind");
    if (kind == "image") c.clut = false;
    else if (kind == "clut") c.clut = true;
    else fail("unknown kind '" + kind.substr(0, 24) + "' (expected image|clut)");
    c.psm = static_cast<unsigned>(r.number("psm", 255));
    c.w = static_cast<unsigned>(r.number("w_or_n", max_dimension));
    c.h = static_cast<unsigned>(r.number("h", max_dimension));
    c.rows = static_cast<unsigned>(r.number("rows", 1024));
    c.lo = static_cast<std::uint32_t>(r.number("descriptor lo", 0xffffffffull));
    c.hi = static_cast<std::uint32_t>(r.number("descriptor hi", 0xffffffffull));
    c.tbp = static_cast<unsigned>(r.number("tbp", 0x3fff));
    c.tbw = static_cast<unsigned>(r.number("tbw", 0x3f));
    const auto count = r.number("touched count", Ps2TextureMemory::vram_bytes);

    const std::string id = "case " + std::to_string(c.n) + ": ";
    if (c.w == 0) fail(id + "zero width/count");
    if (c.clut) {
        if (c.psm != 0) fail(id + "clut psm must be 0");
        if (c.h != 0) fail(id + "clut h must be 0");
        if (c.rows != ((c.w + 31u) >> 5) << 1) fail(id + "clut rows inconsistent with n");
    } else {
        if (c.psm != 0 && c.psm != 19) fail(id + "unsupported psm " + std::to_string(c.psm));
        if (c.h == 0) fail(id + "zero height");
        if (c.rows != 0) fail(id + "image rows must be 0");
    }
    if ((c.lo & 0x3fffu) != c.tbp || ((c.lo >> 14) & 0x3fu) != c.tbw)
        fail(id + "tbp/tbw columns disagree with descriptor lo word");
    if (c.hi >> 30) fail(id + "descriptor hi word has bits above 29");

    for (unsigned long long i = 0; i < count; ++i) {
        const auto a = static_cast<std::uint32_t>(r.number("address", Ps2TextureMemory::vram_bytes - 1));
        const auto v = static_cast<unsigned>(r.number("value", 255));
        if (!c.original.emplace(a, v).second) fail(id + "duplicate touched address " + std::to_string(a));
    }
    return c;
}

std::vector<Case> read_fixture(const char* path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("cannot open fixture");
    const auto size = static_cast<std::uintmax_t>(in.tellg());
    if (size > max_fixture_bytes) fail("fixture exceeds size cap");
    in.seekg(0);
    Reader r(in);
    std::vector<Case> cases;
    while (auto tag = r.tag()) {
        if (*tag != "case") fail("bad header tag '" + tag->substr(0, 24) + "' (trailing junk or corrupt fixture)");
        if (cases.size() >= max_cases) fail("too many cases");
        cases.push_back(read_case(r));
    }
    if (cases.empty()) fail("fixture contains no cases");
    return cases;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: texture-transfer-compare fixture.txt\n"; return 2; }
    std::vector<Case> fixture;
    try { fixture = read_fixture(argv[1]); }
    catch (const FixtureError& e) { std::cerr << "fixture error: " << e.message << "\n"; return 2; }

    int failures = 0, cases = 0;
    for (const Case& c : fixture) {
        const bool image = !c.clut;
        const std::size_t bytes = image ? (c.psm == 0 ? std::size_t(c.w) * c.h * 4 : std::size_t(c.w) * c.h)
                                        : std::size_t(c.w) * 4;
        std::vector<std::uint8_t> payload(bytes);
        for (std::uint32_t k = 0; k < bytes; ++k) {
            std::uint32_t v = (k * 167 + c.n * 29) % 250 + 1;
            if (v >= 165) ++v;
            payload[k] = static_cast<std::uint8_t>(v);
        }
        auto memory = std::make_unique<Ps2TextureMemory>();
        memory->initialize_empty_allocator();
        std::optional<GsTextureAllocation> alloc;
        if (image) {
            alloc = memory->allocate(1, static_cast<GsPixelStorage>(c.psm), c.w, c.h);
            if (alloc) upload_allocation_image(*memory, alloc->id, payload);
        } else {
            alloc = memory->allocate(0, GsPixelStorage::Psmct32, 16, c.rows);
            if (alloc) upload_clut(*memory, alloc->id, c.w, payload);
        }
        bool ok = alloc.has_value();
        std::string why;
        if (!ok) why = "allocation failed";
        else {
            // Original descriptor words: lo = tbp|tbw<<14|id<<20, hi = psm|w<<6|h<<18.
            std::string d;
            auto field = [&](const char* name, unsigned orig, unsigned nat) {
                if (orig != nat)
                    d += std::string(d.empty() ? "" : ", ") + name + " original " + std::to_string(orig) +
                         " native " + std::to_string(nat);
            };
            field("psm", c.hi & 0x3f, static_cast<unsigned>(alloc->psm));
            field("width", (c.hi >> 6) & 0xfff, alloc->width);
            field("height", (c.hi >> 18) & 0xfff, alloc->height);
            field("tbp", c.tbp, alloc->base_block);
            field("tbw", c.tbw, alloc->buffer_width_64);
            field("id", c.lo >> 20, alloc->id);
            if (!d.empty()) { ok = false; why = "descriptor differs: " + d; }

            std::size_t native_defined = 0, mismatches = 0;
            for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a) {
                const auto o = memory->observe_byte(a);
                const auto it = c.original.find(a);
                if (o.value.has_value()) {
                    ++native_defined;
                    if (it == c.original.end() || it->second != *o.value) ++mismatches;
                } else if (it != c.original.end()) {
                    ++mismatches;
                }
            }
            if (mismatches || native_defined != c.original.size()) {
                ok = false;
                why += std::string(why.empty() ? "" : "; ") + "native defined " + std::to_string(native_defined) +
                       " vs original " + std::to_string(c.original.size()) + ", mismatching bytes " +
                       std::to_string(mismatches);
            }
        }
        std::printf("case %u %s psm%u: %s%s%s\n", c.n, image ? "image" : "clut", c.psm, ok ? "MATCH" : "DIFF",
                    ok ? "" : " - ", why.c_str());
        failures += !ok;
        ++cases;
    }
    std::printf("%d cases, %d differences\n", cases, failures);
    return failures != 0;
}
