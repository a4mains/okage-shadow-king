#include "okage/startup_textures.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace okage;

namespace {
void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
template <class F> void scenario(F&& f) { f(); }

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int k = 0; k < 4; ++k) v[at + k] = static_cast<std::uint8_t>(x >> (8 * k));
}

struct EffectShape { unsigned psm, colors, w, h; };
// Order of the authored startup file order.
constexpr EffectShape shapes[31] = {
    {20, 16, 32, 32}, {19, 256, 64, 64}, {20, 16, 64, 64}, {19, 256, 92, 64}, {19, 256, 96, 64},
    {20, 16, 32, 24}, {19, 256, 64, 64}, {20, 16, 128, 84}, {20, 16, 512, 32}, {19, 256, 128, 64},
    {20, 16, 64, 32}, {19, 256, 96, 64}, {19, 256, 240, 16}, {20, 16, 32, 32}, {20, 16, 32, 32},
    {20, 256, 256, 16}, {20, 256, 256, 16}, {20, 16, 32, 32}, {19, 256, 96, 96}, {19, 256, 64, 64},
    {20, 16, 64, 64}, {20, 16, 256, 96}, {19, 256, 96, 64}, {20, 16, 32, 32}, {20, 16, 192, 96},
    {20, 16, 128, 32}, {19, 256, 64, 64}, {20, 16, 480, 24}, {20, 16, 64, 80}, {20, 16, 192, 96},
    {20, 16, 64, 32}};

std::vector<std::uint8_t> effect_xim(const EffectShape& s, unsigned seed) {
    const std::size_t pal = 16 + std::size_t(s.colors) * 4;
    const std::size_t bytes = s.psm == 20 ? std::size_t(s.w) * s.h / 2 : std::size_t(s.w) * s.h;
    std::vector<std::uint8_t> v(16 + pal + 16 + bytes, 0);
    put32(v, 0, s.psm << 20);
    put32(v, 0x10, static_cast<std::uint32_t>(pal));
    put32(v, 0x18, 1);
    put32(v, 0x1c, s.colors);
    for (std::size_t i = 0; i < std::size_t(s.colors) * 4; ++i) v[0x20 + i] = static_cast<std::uint8_t>(seed + i * 5 + 1);
    const auto ih = 16 + pal;
    put32(v, ih + 8, s.h);
    put32(v, ih + 12, s.w);
    for (std::size_t i = 0; i < bytes; ++i) v[ih + 16 + i] = static_cast<std::uint8_t>(seed * 3 + i * 11 + 1);
    return v;
}

std::vector<std::uint8_t> font_file(unsigned first, unsigned last, unsigned w, unsigned h, unsigned seed) {
    const unsigned count = last - first + 1;
    const std::size_t header = 8 + 0x14u * count;
    std::vector<std::uint8_t> f(header + 4 + std::size_t(w) * h / 2, 0);
    f[4] = static_cast<std::uint8_t>(first);
    f[5] = static_cast<std::uint8_t>(last);
    f[header] = h & 0xff; f[header + 1] = h >> 8;
    f[header + 2] = w & 0xff; f[header + 3] = w >> 8;
    for (std::size_t i = header + 4; i < f.size(); ++i) f[i] = static_cast<std::uint8_t>(seed + i * 13 + 1);
    return f;
}

StartupTextureInputs inputs() {
    StartupTextureInputs in;
    in.font_palette.resize(64);
    for (std::size_t i = 0; i < 64; ++i) in.font_palette[i] = static_cast<std::uint8_t>(i * 3 + 1);
    in.fonts = {font_file(0, 138, 256, 192, 1), font_file(0, 254, 256, 193, 2), font_file(0, 254, 256, 130, 3)};
    in.read_effect = [](const std::string& name) -> std::optional<std::vector<std::uint8_t>> {
        const auto& names = startup_effect_names();
        for (unsigned i = 0; i < names.size(); ++i)
            if (name == names[i]) return effect_xim(shapes[i], i);
        return std::nullopt;
    };
    return in;
}

std::size_t live_allocations(const Ps2TextureMemory& m) {
    std::size_t n = 0;
    for (TextureAllocationId id = 0; id < 4096; ++id) n += m.allocation(id).has_value();
    return n;
}
// Stand-in for the 10bc78 preparation: 384 grid draws, then 160 template draws.
struct Hook {
    int calls = 0;
    std::size_t allocations_at_call = 0;
    StartupTextureOwner* owner = nullptr;
    void operator()(StartupRandomStream& s) {
        ++calls;
        allocations_at_call = live_allocations(owner->memory());
        for (int i = 0; i < 384; ++i) s.draw();
        s.enter(StartupRngPhase::EffectTemplate);
        for (int i = 0; i < 160; ++i) s.draw();
    }
};
void run(StartupTextureOwner& o, const StartupTextureInputs& in, GameRandom& r, Hook& h) {
    h.owner = &o;
    o.run_startup(in, r, [&h](StartupRandomStream& s) { h(s); });
}
} // namespace

int main() {
    try {
        scenario([&] {
            StartupTextureOwner o;
            require(o.coverage() == StartupCoverage::NotRun && !o.started(), "fresh owner has no history");
            GameRandom r{1};
            Hook h;
            auto in = inputs();
            in.record_observations = true;
            run(o, in, r, h);
            require(h.calls == 1 && h.allocations_at_call == 73, "effect preparation runs once between uploads and fonts");
            require(o.rng_draws() == 8960 + 544 && o.rng_log().size() == 9504, "all draws through the Session stream");
            require(o.rand_state_after_noise() == 0xceeb6501u, "observed state after the 8960 noise draws (assertion only)");
            require(r.state == 0x412c25e1u, "observed state after 9504 draws (assertion only)");
            require(o.rng_log()[8959].phase == StartupRngPhase::Noise && o.rng_log()[8960].phase == StartupRngPhase::EffectGrid &&
                    o.rng_log()[9343].phase == StartupRngPhase::EffectGrid && o.rng_log()[9344].phase == StartupRngPhase::EffectTemplate,
                    "phase labels at the source boundaries");
            require(o.rng_log()[8960 + 383].state == 0x728e2781u, "observed state after the grid (assertion only)");
            // Noise draws are interleaved with the CLUT/image allocations exactly as in 192ea0.
            std::vector<std::pair<std::uint32_t, std::uint64_t>> allocs;
            for (const auto& e : o.timeline())
                if (e.kind == StartupTimelineEvent::Kind::Allocation) allocs.push_back({e.allocation, e.draws});
            require(allocs.size() == 79, "79 allocation events");
            require(allocs[5].second == 512 && allocs[6].second == 768 && allocs[7].second == 8960 && allocs[4].second == 0,
                    "CLUT A after 512 draws, CLUT B after 768, image after 8960");
            require(allocs[72].second == 8960 && allocs[73].second == 9504, "effects before preparation, fonts after");
            require(o.coverage() == StartupCoverage::PreSchedulerOnly, "coverage is pre-scheduler only");
            const auto& m = o.memory();
            require(live_allocations(m) == StartupTextureOwner::expected_allocations, "79 allocations");
            require(o.handles().live_handle_count() == int(StartupTextureOwner::expected_handles), "36 handles");
            require(o.handles().live_record_count() == 36, "36 records");
            // Source-order descriptors and reference counts.
            const auto ref = [&](TextureAllocationId id) { return m.allocation(id)->references; };
            require(ref(0) == 1 && ref(1) == 1, "colour buffers hold one reference");
            require(ref(2) == 3, "shared depth: two colour layers + capture");
            const auto a0 = *m.allocation(0), a1 = *m.allocation(1), a2 = *m.allocation(2);
            require(a0.allocation_class == 2 && a0.psm == GsPixelStorage::Psmct32 && a0.width == 640 && a0.height == 224, "A00");
            require(a1.allocation_class == 2 && a1.base_block != a0.base_block, "A01");
            require(a2.allocation_class == 3 && a2.psm == GsPixelStorage::Psmz24, "A02 depth PSM49");
            const auto a3 = *m.allocation(3);
            require(a3.allocation_class == 1 && a3.psm == GsPixelStorage::Psmt4 && a3.width == 1024 && a3.height == 256,
                    "A03 glyph cache");
            require(o.glyph_cache_handle() == 0 && o.noise_handle() == 1, "handles 0 and 1");
            const auto a7 = *m.allocation(7), a8 = *m.allocation(8);
            require(a7.psm == GsPixelStorage::Psmt8 && a7.width == 64 && a7.height == 128, "A07 noise");
            require(a8.allocation_class == 2 && a8.width == 640, "A08 capture");
            require(o.layers()[2].color.id == 8 && ref(8) == 1, "capture layer owns A08");
            // Null-source filter CLUTs: live, unreferenced, bytes unknown.
            for (const auto& c : o.filter_cluts()) {
                require(ref(c.id) == 0, "null-source CLUT never retained");
                const auto al = *m.allocation(c.id);
                require(al.width == 16 && al.height == 16, "256-entry CLUT is 16x16");
                std::size_t known = 0;
                for (std::uint32_t b = al.base_block * 256u; b < (al.base_block + 4u) * 256u; ++b)
                    known += m.observe_byte(b).value.has_value();
                require(known == 0, "null-source CLUT bytes are not known zero");
            }
            // Colour/depth/capture bytes never written.
            for (TextureAllocationId id : {0u, 1u, 2u, 3u, 8u}) {
                const auto al = *m.allocation(id);
                require(!m.observe_byte(al.base_block * 256u).value.has_value(), "unwritten buffer start stays unknown");
            }
            require(o.effect_handles().size() == 31 && o.effect_handles().front() == 2 && o.effect_handles().back() == 32,
                    "effects take handles 2..32 in authored order");
            require(o.font_handles()[0] == 33 && o.font_handles()[2] == 35, "fonts take handles 33..35");
            require(m.allocation(o.handles().image(o.font_handles()[0])->id)->psm == GsPixelStorage::Psmt4Hh, "IQ24 is PSM44");
            require(m.allocation(o.handles().image(o.font_handles()[1])->id)->psm == GsPixelStorage::Psmt4Hl, "BMUI is PSM36");
            require(m.allocation(o.handles().image(o.font_handles()[2])->id)->psm == GsPixelStorage::Psmt4, "BM is PSM20");
            require(o.rand_state_after_noise() != 1, "noise consumed the generator");
            require(o.checkpoints().size() == 6 && std::string(o.checkpoints().back().step) == "fonts", "checkpoints");
            // Events are disabled by default: bounded in ordinary use.
            require(o.handles().events().empty(), "no accumulating event log");
        });
        // Re-initialization is rejected and does not disturb state.
        scenario([&] {
            StartupTextureOwner o;
            GameRandom r{1};
            Hook h;
            run(o, inputs(), r, h);
            const auto id = o.memory().identity();
            const auto state = r.state;
            bool threw = false;
            try { run(o, inputs(), r, h); } catch (const std::logic_error&) { threw = true; }
            require(r.state == state && h.calls == 1, "rejected run draws nothing and calls no hook");
            require(threw && o.memory().identity() == id && o.handles().live_handle_count() == 36, "second run rejected");
        });
        // A failing input leaves the owner unusable, not half-announced.
        scenario([&] {
            StartupTextureOwner o;
            auto in = inputs();
            in.read_effect = [](const std::string&) { return std::optional<std::vector<std::uint8_t>>{}; };
            GameRandom r{1};
            Hook h;
            bool threw = false;
            try { run(o, in, r, h); } catch (const std::runtime_error&) { threw = true; }
            require(threw && o.coverage() == StartupCoverage::NotRun && o.started(), "failed startup never claims coverage");
            require(std::string(o.failed_phase()) == "effect-uploads" && o.rng_draws() == 8960 && h.calls == 0,
                    "failure phase and the draws already made are retained");
            threw = false;
            try { run(o, inputs(), r, h); } catch (const std::logic_error&) { threw = true; }
            require(threw, "no retry on a partially built owner");
        });
        // The preparation hook failing or re-entering is a failed prefix, never coverage.
        scenario([&] {
            StartupTextureOwner o;
            GameRandom r{1};
            bool reentered = false;
            bool threw = false;
            try {
                o.run_startup(inputs(), r, [&](StartupRandomStream& s) {
                    s.draw();
                    try { o.run_startup(inputs(), r, [](StartupRandomStream&) {}); } catch (const std::logic_error&) { reentered = true; }
                    throw std::runtime_error("template factory failed");
                });
            } catch (const std::runtime_error&) { threw = true; }
            require(threw && reentered && o.coverage() == StartupCoverage::NotRun, "hook failure leaves no coverage");
            require(std::string(o.failed_phase()) == "effect-preparation" && o.rng_draws() == 8961, "failure in preparation retained");
            require(o.handles().live_handle_count() == 33, "fonts never created after a failed preparation");
            StartupTextureOwner n;
            threw = false;
            try { n.run_startup(inputs(), r, {}); } catch (const std::invalid_argument&) { threw = true; }
            require(threw && n.coverage() == StartupCoverage::NotRun, "missing hook rejected");
        });
        // Invalid font ranges and extents are rejected before any allocation or draw.
        scenario([&] {
            auto broken = [&](auto mutate) {
                StartupTextureOwner o;
                auto in = inputs();
                mutate(in);
                GameRandom r{1};
                Hook h;
                bool threw = false;
                try { run(o, in, r, h); } catch (const std::runtime_error&) { threw = true; }
                require(threw && r.state == 1 && h.calls == 0 && !o.memory().allocator_initialized(),
                        "font rejected before any state or draw");
            };
            broken([](StartupTextureInputs& in) { std::swap(in.fonts[0][4], in.fonts[0][5]); in.fonts[0][4] = 9; in.fonts[0][5] = 3; });
            broken([](StartupTextureInputs& in) { in.fonts[1].resize(7); });
            broken([](StartupTextureInputs& in) { in.fonts[2].resize(in.fonts[2].size() - 1); });
            broken([](StartupTextureInputs& in) { in.fonts[2][5] = 255; in.fonts[2][4] = 0; in.fonts[2].resize(8 + 0x14 * 200); });
            broken([](StartupTextureInputs& in) {
                const std::size_t header = 8 + 0x14u * 139;
                in.fonts[0][header] = 0; in.fonts[0][header + 1] = 0;  // zero height
            });
        });
        // Snapshots taken at the boundary survive later writes, frees and reuse.
        scenario([&] {
            StartupTextureOwner o;
            GameRandom r{1};
            Hook h;
            run(o, inputs(), r, h);
            const auto snap = o.memory().snapshot();
            const auto id = o.layers()[2].color.id;
            const auto before_ref = snap.allocation(id)->references;
            const auto glyph = snap.allocation(3);
            const auto probe = [&](std::uint32_t a) { return snap.observe_byte(a); };
            const auto img = *o.handles().image(o.font_handles()[2]);
            const auto base = o.memory().allocation(img.id)->base_block * 256u;
            const auto old_known = probe(base).value.has_value();
            o.handles().retire(o.font_handles()[2]);  // frees the font allocation, bytes persist
            o.memory().write_bytes(base, std::array<std::uint8_t, 4>{9, 9, 9, 9});
            o.memory().write_bytes(o.memory().allocation(id)->base_block * 256u, std::array<std::uint8_t, 1>{7});
            require(snap.allocation(id)->references == before_ref && snap.allocation(3)->serial == glyph->serial && snap.allocation(3)->references == glyph->references, "snapshot allocation table frozen");
            require(snap.allocation(img.id).has_value(), "freed allocation still present in the old snapshot");
            require(probe(base).value.has_value() == old_known, "old snapshot bytes unchanged by later write");
            require(!snap.observe_byte(o.memory().allocation(id)->base_block * 256u).value.has_value(),
                    "snapshot did not learn the later write");
        });
        // Teardown releases every reference; memory is reusable only through a new aggregate.
        scenario([&] {
            auto* o = new StartupTextureOwner;
            GameRandom r{1};
            Hook h;
            run(*o, inputs(), r, h);
            const auto snap = o->memory().snapshot();
            delete o;
            require(snap.valid() && snap.allocation(2)->references == 3, "snapshot outlives the aggregate");
        });
        std::cout << "startup texture tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
