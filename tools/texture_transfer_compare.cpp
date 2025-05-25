#include "okage/texture_transfer.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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

struct Case2 {
    unsigned n{}, kind{}, psm{}, w{}, h{}, x{}, y{}, tbp{}, tbw{}, pattern{}, seed{}, pre_lo{}, pre_len{};
    std::uint32_t lo{}, hi{};
    std::map<std::uint32_t, std::pair<unsigned, unsigned>> written;  // address -> (value, nibble mask)
};

bool staged(const Case2& c) { return c.kind == 3 || c.kind == 4; }

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

bool supported_v2_psm(unsigned psm) {
    return psm == 0 || psm == 1 || psm == 19 || psm == 20 || psm == 36 || psm == 44;
}
bool supported_pre_psm(unsigned psm) { return psm != 1 && supported_v2_psm(psm); }
unsigned bits_v2(unsigned psm) { return psm == 0 ? 32 : psm == 1 ? 24 : psm == 19 ? 8 : 4; }

Case2 read_case2(Reader& r) {
    Case2 c;
    c.n = static_cast<unsigned>(r.number("case2 number", 1000000));
    c.kind = static_cast<unsigned>(r.number("kind", 4));
    c.psm = static_cast<unsigned>(r.number("psm", 255));
    c.w = static_cast<unsigned>(r.number("w", 512));
    c.h = static_cast<unsigned>(r.number("h", 512));
    c.x = static_cast<unsigned>(r.number("x", 2047));
    c.y = static_cast<unsigned>(r.number("y", 2047));
    c.tbp = static_cast<unsigned>(r.number("tbp", 0x3fff));
    c.tbw = static_cast<unsigned>(r.number("tbw", 0x3f));
    c.pattern = static_cast<unsigned>(r.number("pattern", 3));
    c.seed = static_cast<unsigned>(r.number("seed", 0xffffffffull));
    c.pre_lo = static_cast<unsigned>(r.number("pre_lo", Ps2TextureMemory::vram_bytes));
    c.pre_len = static_cast<unsigned>(r.number("pre_len", 0xffffffffull));
    c.lo = static_cast<std::uint32_t>(r.number("descriptor lo", 0xffffffffull));
    c.hi = static_cast<std::uint32_t>(r.number("descriptor hi", 0xffffffffull));
    const auto count = r.number("written count", Ps2TextureMemory::vram_bytes);
    const std::string id = "case2 " + std::to_string(c.n) + ": ";
    if (c.kind > 4 || c.kind == 1) fail(id + "kind must be 0, 2, 3 or 4");
    if (!supported_v2_psm(c.psm)) fail(id + "unsupported psm " + std::to_string(c.psm));
    if (c.w == 0 || c.h == 0) fail(id + "zero dimension");
    if (c.kind == 3 || c.kind == 4) {
        if (!supported_pre_psm(c.pre_lo)) fail(id + "unsupported staged pre-upload psm " + std::to_string(c.pre_lo));
        const unsigned pw = c.pre_len & 0xffffu, ph = c.pre_len >> 16;
        if (pw < 1 || pw > 512 || ph < 1 || ph > 512) fail(id + "staged pre-upload dimension outside 1..512");
        if (std::uint64_t(pw) * ph * bits_v2(c.pre_lo) < 128) fail(id + "staged pre-upload sends no quadword");
    } else if (std::uint64_t(c.pre_lo) + c.pre_len > Ps2TextureMemory::vram_bytes) {
        fail(id + "preload window outside VRAM");
    }
    if ((c.kind == 0 || c.kind == 4) && (c.x || c.y)) fail(id + "allocated row has an origin");
    if ((c.lo & 0x3fffu) != c.tbp || ((c.lo >> 14) & 0x3fu) != c.tbw || (c.hi & 0x3fu) != c.psm)
        fail(id + "tbp/tbw/psm columns disagree with the descriptor words");
    if (std::uint64_t(c.w) * c.h * bits_v2(c.psm) < 128) fail(id + "rectangle sends no quadword");
    for (unsigned long long i = 0; i < count; ++i) {
        const auto a = static_cast<std::uint32_t>(r.number("address", Ps2TextureMemory::vram_bytes - 1));
        const auto v = static_cast<unsigned>(r.number("value", 255));
        const auto m = static_cast<unsigned>(r.number("mask", 3));
        if (m == 0) fail(id + "written entry with empty mask at " + std::to_string(a));
        if (v & ~((m & 1 ? 0x0fu : 0u) | (m & 2 ? 0xf0u : 0u)))
            fail(id + "value has bits in an unwritten nibble at " + std::to_string(a));
        if (!c.written.emplace(a, std::make_pair(v, m)).second) fail(id + "duplicate written address " + std::to_string(a));
    }
    return c;
}

std::vector<std::uint8_t> payload_v2(const Case2& c) {
    const std::size_t bytes = (std::size_t(c.w) * c.h * bits_v2(c.psm) + 7) / 8;
    std::vector<std::uint8_t> p(bytes);
    for (std::uint32_t k = 0; k < bytes; ++k) {
        switch (c.pattern) {
            case 0: p[k] = static_cast<std::uint8_t>(k * 167u + c.seed * 29u + 5u); break;
            case 1: p[k] = 0x00; break;
            case 2: p[k] = 0xff; break;
            default: p[k] = (k & 1) ? 0x0f : 0xf0; break;
        }
    }
    return p;
}

std::uint8_t preload_byte(const Case2& c, std::uint32_t a) {
    return static_cast<std::uint8_t>(a * 131u + c.seed * 17u + 11u);
}

// Runs the native transfer into `memory` (optionally preloaded). Throws on a native rejection.
void run_native_v2(const Case2& c, Ps2TextureMemory& memory, bool preload, std::string& descriptor_diff) {
    memory.initialize_empty_allocator();
    if (preload && !staged(c)) {
        std::vector<std::uint8_t> window(c.pre_len);
        for (std::uint32_t i = 0; i < c.pre_len; ++i) window[i] = preload_byte(c, c.pre_lo + i);
        if (!window.empty()) memory.write_bytes(c.pre_lo, window);
    }
    const auto payload = payload_v2(c);
    if (staged(c)) {
        const unsigned pre_psm = c.pre_lo, pw = c.pre_len & 0xffffu, ph = c.pre_len >> 16;
        std::vector<std::uint8_t> pre((std::size_t(pw) * ph * bits_v2(pre_psm) + 7) / 8);
        for (std::uint32_t k = 0; k < pre.size(); ++k)
            pre[k] = static_cast<std::uint8_t>(k * 167u + (c.seed + 1000u) * 29u + 5u);
        if (c.kind == 3) {
            upload_retail_rect(memory, GsHostToLocal{pre_psm, c.tbp, c.tbw, 0, 0, pw, ph}, pre);
            upload_retail_rect(memory, GsHostToLocal{c.psm, c.tbp, c.tbw, c.x, c.y, c.w, c.h}, payload);
        } else {
            const auto first = memory.allocate(1, static_cast<GsPixelStorage>(pre_psm), static_cast<std::uint16_t>(pw),
                                               static_cast<std::uint16_t>(ph));
            if (!first) throw std::runtime_error("native pre allocation failed");
            const auto alloc = memory.allocate(1, static_cast<GsPixelStorage>(c.psm), static_cast<std::uint16_t>(c.w),
                                               static_cast<std::uint16_t>(c.h));
            if (!alloc) throw std::runtime_error("native allocation failed");
            auto field = [&](const char* name, unsigned orig, unsigned nat) {
                if (orig != nat)
                    descriptor_diff += std::string(descriptor_diff.empty() ? "" : ", ") + name + " original " +
                                       std::to_string(orig) + " native " + std::to_string(nat);
            };
            field("psm", c.hi & 0x3f, static_cast<unsigned>(alloc->psm));
            field("width", (c.hi >> 6) & 0xfff, alloc->width);
            field("height", (c.hi >> 18) & 0xfff, alloc->height);
            field("tbp", c.tbp, alloc->base_block);
            field("tbw", c.tbw, alloc->buffer_width_64);
            field("id", c.lo >> 20, alloc->id);
            upload_allocation_image(memory, first->id, pre);
            upload_allocation_image(memory, alloc->id, payload);
        }
    } else if (c.kind == 0) {
        const auto alloc = memory.allocate(1, static_cast<GsPixelStorage>(c.psm), static_cast<std::uint16_t>(c.w),
                                           static_cast<std::uint16_t>(c.h));
        if (!alloc) throw std::runtime_error("native allocation failed");
        auto field = [&](const char* name, unsigned orig, unsigned nat) {
            if (orig != nat)
                descriptor_diff += std::string(descriptor_diff.empty() ? "" : ", ") + name + " original " +
                                   std::to_string(orig) + " native " + std::to_string(nat);
        };
        field("psm", c.hi & 0x3f, static_cast<unsigned>(alloc->psm));
        field("width", (c.hi >> 6) & 0xfff, alloc->width);
        field("height", (c.hi >> 18) & 0xfff, alloc->height);
        field("tbp", c.tbp, alloc->base_block);
        field("tbw", c.tbw, alloc->buffer_width_64);
        field("id", c.lo >> 20, alloc->id);
        upload_allocation_image(memory, alloc->id, payload);
    } else {
        GsHostToLocal rect{c.psm, c.tbp, c.tbw, c.x, c.y, c.w, c.h};
        upload_retail_rect(memory, rect, payload);
    }
}

struct Verdict {
    std::size_t mask_diffs{}, value_diffs{}, native_known_bytes{}, expected_known_bytes{};
    std::string first_mask, first_value;
};

Verdict compare_masks(const Ps2TextureMemory& memory,
                      const std::map<std::uint32_t, std::pair<unsigned, unsigned>>& expected) {
    Verdict v;
    const auto known = memory.known_byte_addresses();
    v.native_known_bytes = known.size();
    v.expected_known_bytes = expected.size();
    auto check = [&](std::uint32_t a) {
        const auto o = memory.observe_byte_masked(a);
        const auto it = expected.find(a);
        const unsigned em = it == expected.end() ? 0u : it->second.second;
        const unsigned ev = it == expected.end() ? 0u : it->second.first;
        if (o.known_mask != em) {
            ++v.mask_diffs;
            if (v.first_mask.empty())
                v.first_mask = "addr " + std::to_string(a) + " native mask " + std::to_string(o.known_mask) +
                               " original " + std::to_string(em);
        }
        const unsigned both = o.known_mask & em;
        const unsigned bits = (both & 1 ? 0x0fu : 0u) | (both & 2 ? 0xf0u : 0u);
        if ((o.value & bits) != (ev & bits)) {
            ++v.value_diffs;
            if (v.first_value.empty())
                v.first_value = "addr " + std::to_string(a) + " native value " + std::to_string(o.value & bits) +
                                " original " + std::to_string(ev & bits);
        }
    };
    std::size_t ki = 0;
    for (const auto& entry : expected) {
        const auto a = entry.first;
        while (ki < known.size() && known[ki] < a) check(known[ki++]);
        if (ki < known.size() && known[ki] == a) ++ki;
        check(a);
    }
    while (ki < known.size()) check(known[ki++]);
    return v;
}

bool run_case2(const Case2& c, std::string& why) {
    bool ok = true;
    auto add = [&](const std::string& m) { why += std::string(why.empty() ? "" : "; ") + m; ok = false; };
    std::string descriptor_diff;
    try {
        auto fresh = std::make_unique<Ps2TextureMemory>();
        run_native_v2(c, *fresh, false, descriptor_diff);
        if (!descriptor_diff.empty()) add("descriptor differs: " + descriptor_diff);
        const auto v = compare_masks(*fresh, c.written);
        if (v.mask_diffs) add("write-mask differs in " + std::to_string(v.mask_diffs) + " bytes (native known " +
                              std::to_string(v.native_known_bytes) + " vs original written " +
                              std::to_string(v.expected_known_bytes) + ", first " + v.first_mask + ")");
        if (v.value_diffs) add("written values differ in " + std::to_string(v.value_diffs) + " bytes (first " + v.first_value + ")");
    } catch (const std::exception& e) {
        add(std::string("native fresh run threw: ") + e.what());
        return ok;
    }
    if (staged(c)) return ok;
    try {
        auto pre = std::make_unique<Ps2TextureMemory>();
        std::string ignored;
        run_native_v2(c, *pre, true, ignored);
        // Expected: preload window known (original P), overwritten by the written nibbles.
        std::map<std::uint32_t, std::pair<unsigned, unsigned>> expected = c.written;
        for (std::uint32_t i = 0; i < c.pre_len; ++i) {
            const std::uint32_t a = c.pre_lo + i;
            const unsigned p = preload_byte(c, a);
            auto it = expected.find(a);
            if (it == expected.end()) { expected.emplace(a, std::make_pair(p, 3u)); continue; }
            const unsigned m = it->second.second;
            const unsigned keep = (m & 1 ? 0u : 0x0fu) | (m & 2 ? 0u : 0xf0u);
            it->second = {it->second.first | (p & keep), 3u};
        }
        const auto v = compare_masks(*pre, expected);
        if (v.mask_diffs || v.value_diffs)
            add("preloaded-neighbour run differs: " + std::to_string(v.value_diffs) + " value bytes, " +
                std::to_string(v.mask_diffs) + " known-mask bytes (first " +
                (v.first_value.empty() ? v.first_mask : v.first_value) + ")");
    } catch (const std::exception& e) {
        add(std::string("native preloaded run threw: ") + e.what());
    }
    return ok;
}

std::vector<Case> read_fixture(const char* path, std::vector<Case2>& cases2) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("cannot open fixture");
    const auto size = static_cast<std::uintmax_t>(in.tellg());
    if (size > max_fixture_bytes) fail("fixture exceeds size cap");
    in.seekg(0);
    Reader r(in);
    std::vector<Case> cases;
    while (auto tag = r.tag()) {
        if (*tag == "case2") {
            if (cases.size() + cases2.size() >= max_cases) fail("too many cases");
            cases2.push_back(read_case2(r));
            for (std::size_t i = 0; i + 1 < cases2.size(); ++i)
                if (cases2[i].n == cases2.back().n) fail("duplicate case2 number " + std::to_string(cases2.back().n));
            continue;
        }
        if (*tag != "case") fail("bad header tag '" + tag->substr(0, 24) + "' (trailing junk or corrupt fixture)");
        if (cases.size() + cases2.size() >= max_cases) fail("too many cases");
        cases.push_back(read_case(r));
        for (std::size_t i = 0; i + 1 < cases.size(); ++i)
            if (cases[i].n == cases.back().n) fail("duplicate case number " + std::to_string(cases.back().n));
    }
    if (cases.empty() && cases2.empty()) fail("fixture contains no cases");
    return cases;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: texture-transfer-compare fixture.txt\n"; return 2; }
    std::vector<Case> fixture;
    std::vector<Case2> fixture2;
    try { fixture = read_fixture(argv[1], fixture2); }
    catch (const FixtureError& e) { std::cerr << "fixture error: " << e.message << "\n"; return 2; }

    int failures = 0, cases = 0;
    for (const Case2& c : fixture2) {
        std::string why;
        const bool ok = run_case2(c, why);
        std::printf("case2 %u %s psm%u %ux%u: %s%s%s\n", c.n, c.kind == 0 ? "allocated" : c.kind == 2 ? "descriptor" : c.kind == 3 ? "staged" : "allocated-pair", c.psm, c.w, c.h,
                    ok ? "MATCH" : "DIFF", ok ? "" : " - ", why.c_str());
        failures += !ok;
        ++cases;
    }
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
