#include "okage/texture_transfer.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

using namespace okage;

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: texture-transfer-compare fixture.txt\n"; return 2; }
    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "cannot open fixture\n"; return 2; }
    std::string tag;
    int failures = 0, cases = 0;
    while (in >> tag) {
        if (tag != "case") { std::cerr << "bad fixture\n"; return 2; }
        unsigned n, psm, w, h, rows, tbp, tbw, count;
        std::string kind;
        unsigned long long lo, hi;
        in >> n >> kind >> psm >> w >> h >> rows >> lo >> hi >> tbp >> tbw >> count;
        std::map<std::uint32_t, unsigned> original;
        for (unsigned i = 0; i < count; ++i) { std::uint32_t a; unsigned v; in >> a >> v; original[a] = v; }
        const std::size_t bytes = kind == "image"
            ? (psm == 0 ? std::size_t(w) * h * 4 : std::size_t(w) * h)
            : std::size_t(w) * 4;
        std::vector<std::uint8_t> payload(bytes);
        for (std::uint32_t k = 0; k < bytes; ++k) {
            std::uint32_t v = (k * 167 + n * 29) % 250 + 1;
            if (v >= 165) ++v;
            payload[k] = static_cast<std::uint8_t>(v);
        }
        auto memory = std::make_unique<Ps2TextureMemory>();
        memory->initialize_empty_allocator();
        std::optional<GsTextureAllocation> alloc;
        if (kind == "image") {
            alloc = memory->allocate(1, static_cast<GsPixelStorage>(psm), w, h);
            if (alloc) upload_allocation_image(*memory, alloc->id, payload);
        } else {
            alloc = memory->allocate(0, GsPixelStorage::Psmct32, 16, rows);
            if (alloc) upload_clut(*memory, alloc->id, w, payload);
        }
        bool ok = alloc.has_value();
        std::string why;
        if (!ok) why = "allocation failed";
        else {
            const std::uint64_t desc = lo | (hi << 32);
            // The observer reports descriptor as two 64-bit halves {low word group, high}; compare packed fields.
            if (alloc->base_block != tbp || alloc->buffer_width_64 != tbw) { ok = false; why = "placement differs"; }
            (void)desc;
            std::size_t native_defined = 0, mismatches = 0;
            for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a) {
                const auto o = memory->observe_byte(a);
                const auto it = original.find(a);
                if (o.value.has_value()) {
                    ++native_defined;
                    if (it == original.end() || it->second != *o.value) ++mismatches;
                } else if (it != original.end()) {
                    ++mismatches;
                }
            }
            if (mismatches || native_defined != original.size()) {
                ok = false;
                why = "native defined " + std::to_string(native_defined) + " vs original " +
                      std::to_string(original.size()) + ", mismatching bytes " + std::to_string(mismatches);
            }
        }
        std::printf("case %u %s psm%u: %s%s%s\n", n, kind.c_str(), psm, ok ? "MATCH" : "DIFF", ok ? "" : " - ", why.c_str());
        failures += !ok;
        ++cases;
    }
    std::printf("%d cases, %d differences\n", cases, failures);
    return failures || !cases;
}
