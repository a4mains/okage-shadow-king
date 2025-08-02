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
        // Title lifecycle: scheduled invalidation, logo image list, retirement, title model binding.
        scenario([&] {
            StartupTextureOwner o;
            GameRandom r{1};
            Hook h;
            auto in = inputs();
            in.record_observations = true;
            run(o, in, r, h);
            auto& m = o.memory();
            const auto logo = effect_xim({19, 256, 64, 32}, 50);
            bool threw = false;
            try { o.begin_logo_construction(); } catch (const std::logic_error&) { threw = true; }
            require(threw, "logos before the first scheduler traversal are rejected");
            // Font high lanes inside the depth words: known before and after invalidation.
            const auto depth = *m.allocation(2);
            std::size_t font_lanes = 0, depth_low_known = 0;
            for (std::uint32_t a = depth.base_block * 256u; a < (depth.base_block + 0x700u) * 256u; ++a) {
                if (a % 4 == 3 && m.observe_nibble(a, 0).known) ++font_lanes;
                if (a % 4 != 3 && m.observe_byte(a).value) ++depth_low_known;
            }
            require(font_lanes > 0 && depth_low_known == 0, "fixture: font lanes known, Z24 lanes never written");
            const auto before = m.snapshot();
            // Pretend an earlier writer made a colour byte known, then invalidate.
            m.write_bytes(m.allocation(o.layers()[0].color.id)->base_block * 256u, std::array<std::uint8_t, 4>{1, 2, 3, 4});
            const auto with_write = m.snapshot();
            o.begin_scheduled_frames();
            require(o.memory_coverage() == MemoryContentCoverage::DrawTargetsInvalidated, "memory coverage separated");
            require(!m.observe_byte(0).value && with_write.observe_byte(0).value == std::optional<std::uint8_t>(1),
                    "colour buffer unknown now; the older snapshot keeps its bytes");
            std::size_t font_after = 0;
            for (std::uint32_t a = depth.base_block * 256u; a < (depth.base_block + 0x700u) * 256u; a += 4)
                font_after += m.observe_nibble(a + 3, 0).known;
            require(font_after == font_lanes, "font lanes in the depth top byte stay known");
            (void)before;
            // Logos: append in order, retire in insertion order, frees reused by the title.
            o.begin_logo_construction();
            const int h1 = o.append_image(logo), h2 = o.append_image(effect_xim({19, 256, 64, 32}, 51));
            require(h1 == 36 && h2 == 37 && o.image_list() == std::vector<int>({36, 37}), "image list in order");
            require(o.append_image(std::nullopt) == -1 && o.image_list().size() == 2, "read failure appends nothing");
            const auto logo_alloc = o.handles().image(h1)->id;
            const auto available = m.available_count();
            o.retire_image_list();
            require(o.image_list().empty() && !m.allocation(logo_alloc) && m.available_count() == available + 4,
                    "both logo images and CLUTs freed at retirement");
            threw = false;
            try { o.retire_image_list(); } catch (const std::logic_error&) { threw = true; }
            require(threw, "double retirement rejected");
            o.begin_title_construction();
            const int t0 = o.append_image(effect_xim({19, 256, 64, 32}, 52));
            require(t0 == 36 && o.handles().image(t0)->serial > 79, "handle id reused, new allocation incarnation");
            // A hole in the live list (retired handle) and a failed load shift the successor binding.
            const int stray = o.handles().create_handle(std::nullopt);
            std::vector<std::string> names{"Tex_A", "tex_b", "TEX_A", "missing", "tex_c"};
            int reads = 0;
            std::vector<std::vector<std::uint8_t>> returned;
            const auto result = o.load_and_bind_model("MODEL/a.xmd", names, [&](const std::string& f) -> std::optional<std::vector<std::uint8_t>> {
                ++reads;
                auto bytes = f == "missing.xim" ? effect_xim({19, 256, 64, 32}, 99)  // loads fine
                                                : effect_xim({19, 256, 32, 32}, static_cast<unsigned>(f.size()) + reads);
                returned.push_back(bytes);
                return bytes;
            });
            require(reads == 5 && result.first >= 0, "cache off: every name read, duplicates load again");
            const auto& binding = o.model_binding();
            require(binding.size() == 5 && binding[0] == result.first, "binding starts at the first handle");
            for (std::size_t i = 1; i < binding.size(); ++i)
                require(binding[i] == o.handles().live_successor(binding[i - 1]), "successor walk, not first+i");
            require(o.handles().live_successor(binding.back()) == -7 && o.handles().live_successor(-7) == -1,
                    "past the tail: -7 then -1");
            const auto& slot_bytes = o.model_slot_bytes();
            require(slot_bytes.size() == binding.size(), "one owned-byte entry per slot");
            for (std::size_t i = 0; i < binding.size(); ++i) {
                std::size_t name = result.names.size();
                for (std::size_t j = 0; j < result.names.size(); ++j)
                    if (result.names[j].handle == binding[i]) name = j;
                require(name < result.names.size() && slot_bytes[i] && *slot_bytes[i] == returned[name],
                        "slot bytes are the bytes read for the handle bound there");
            }
            require(*slot_bytes[0] != *slot_bytes[2], "case-variant duplicate keeps its own read");
            (void)stray;
            o.finish_title_construction();
            require(o.stage() == ResourceStage::TitleConstructed, "title stage");
            threw = false;
            try { o.begin_title_construction(); } catch (const std::logic_error&) { threw = true; }
            require(threw, "no second title construction");
            o.mark_unmodeled("first title update");
            require(o.stage() == ResourceStage::Unmodeled && o.unmodeled_reason() == "first title update",
                    "history past the boundary is recorded as unmodeled, without throwing");
            o.mark_unmodeled("again");  // idempotent; keeps the first reason
            require(o.unmodeled_reason() == "first title update", "first reason kept");
            int phases = 0, materials = 0, frees = 0;
            for (const auto& line : o.lifecycle()) {
                phases += line.find("\"phase\"") != std::string::npos;
                materials += line.find("\"material\"") != std::string::npos;
                frees += line.find("\"kind\":\"free\"") != std::string::npos;
            }
            require(materials == 5 && frees == 4 && phases > 0, "material and free events recorded");
        });
        scenario([&] {
            StartupTextureOwner o;
            GameRandom r{1};
            Hook h;
            auto in = inputs();
            in.record_observations = true;
            run(o, in, r, h);
            o.begin_scheduled_frames();
            o.begin_logo_construction();
            o.append_image(effect_xim({19, 256, 64, 32}, 50));
            o.retire_image_list();
            o.begin_title_construction();
            for (unsigned i = 0; i < 3; ++i) o.append_image(effect_xim({19, 256, 64, 32}, 60 + i));
            const int hanten = o.image_list().back();
            std::vector<std::string> names{"Wat_A", "wat_b", "WAT_A", "wat_c"};  // case-variant duplicate
            o.load_and_bind_model("M/a.xmd", names, [](std::string& f) -> std::optional<std::vector<std::uint8_t>> {
                for (auto& c : f) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
                return effect_xim({19, 256, 32, 32}, static_cast<unsigned>(f.size()));
            });
            o.finish_title_construction();
            auto& m = o.memory();
            const auto survivors_alloc = [&] { std::vector<TextureAllocationId> v;
                for (TextureAllocationId id = 0; id < 79; ++id) if (m.allocation(id)) v.push_back(id); return v; }();
            require(survivors_alloc.size() == 79, "fixture: the 79 startup allocations");
            std::vector<std::uint32_t> startup_refs;
            for (auto id : survivors_alloc) startup_refs.push_back(m.allocation(id)->references);
            const auto binding = o.model_binding();
            const auto hanten_alloc = o.handles().image(hanten)->id;
            const auto hanten_refs = m.allocation(hanten_alloc)->references;
            bool threw = false;
            try { o.note_effect_create(0, 106); } catch (const std::logic_error&) { threw = true; }
            require(threw, "effect events before the first update are rejected");
            o.begin_title_updates();
            o.note_effect_create(0, 106);
            const auto identity = m.identity();
            o.note_effect_bind(0, 0, hanten);
            require(m.allocation(hanten_alloc)->references == hanten_refs && m.identity() == identity,
                    "descriptor-only rebind: no reference, allocation or memory change");
            threw = false;
            try { o.note_effect_bind(0, 0, 4000); } catch (const std::logic_error&) { threw = true; }
            require(threw, "rebinding a dead handle is rejected");
            o.note_checkpoint("stable-menu");
            threw = false;
            try { o.retire_title_resources(); } catch (const std::logic_error&) { threw = true; }
            require(threw, "retirement before the mode request is rejected");
            threw = false;
            try { o.request_title_successor(11); } catch (const std::logic_error&) { threw = true; }
            require(threw, "only Restore/New Game successors are modeled");
            o.note_effect_removal_request(0);
            o.note_effect_removal_request(0);
            require(o.effect106() && std::string(o.effect106()->state) == "removal-requested" &&
                    o.effect106()->element_handles == std::vector<int>({hanten}), "effect106 observation");
            o.request_title_successor(10);
            o.note_effect_reclaim_armed(0);
            require(std::string(o.effect106()->state) == "reclaim-pending", "reclaim armed, not reclaimed");
            const auto free_before = m.free_queue_ids().size();
            const auto lifecycle_before = o.lifecycle().size();
            o.retire_title_resources();
            require(o.model_binding().empty() && o.image_list().empty() && o.model_slot_bytes().empty(),
                    "bindings and image list released");
            for (int handle : binding) require(o.handles().record_of(handle) < 0, "every bound handle retired");
            require(!m.allocation(hanten_alloc), "hanten freed by the image list, not by the effect");
            for (std::size_t i = 0; i < survivors_alloc.size(); ++i)
                require(m.allocation(survivors_alloc[i]) && m.allocation(survivors_alloc[i])->references == startup_refs[i],
                        "startup allocations survive with unchanged references");
            require(o.handles().live_order().size() == 36, "startup handles 0..35 survive");
            for (const auto& [slot, value] : o.handles().hash_entries()) require(value == -1, "no live name-hash entry");
            require(m.free_queue_ids().size() > free_before, "frees reach the allocator queue");
            // Source order: model slots first (binding order), then image list (insertion order).
            std::vector<int> retired;
            for (std::size_t i = lifecycle_before; i < o.lifecycle().size(); ++i) {
                const auto& line = o.lifecycle()[i];
                const std::string key = "\"kind\":\"retire\",\"handle\":";
                const auto at = line.find(key);
                if (at != std::string::npos) retired.push_back(std::stoi(line.substr(at + key.size())));
            }
            std::vector<int> expected = binding;
            expected.insert(expected.end(), {36, 37, hanten});
            require(retired == expected, "retire order: binding order, then insertion order");
            o.destroy_title_mode();
            threw = false;
            try { o.enter_successor(10); } catch (const std::logic_error&) { threw = true; }
            require(threw, "successor entry before the wrappers are freed is rejected");
            o.free_title_wrappers();
            threw = false;
            try { o.enter_successor(9); } catch (const std::logic_error&) { threw = true; }
            require(threw, "successor must be the requested mode");
            o.enter_successor(10);
            require(o.stage() == ResourceStage::SuccessorEntry && r.state == o.rng_log().back().state,
                    "successor entry reached with no RNG draw after the prefix");
            // Restore: Restore owns its atlas through the wrapper, not the image list.
            threw = false;
            try { o.note_effect_reclaim(0); } catch (const std::logic_error&) { threw = true; }
            require(threw, "reclaim before Restore construction is rejected");
            const auto records_before = o.handles().free_record_count();
            const int atlas = o.construct_restore(effect_xim({19, 256, 64, 32}, 77));
            require(atlas >= 0 && o.restore_atlas() == atlas && o.image_list().empty(), "wrapper-owned atlas");
            const auto atlas_alloc = o.handles().image(atlas)->id;
            o.note_effect_reclaim(0);
            require(std::string(o.effect106()->state) == "reclaimed", "water job reclaimed by the Restore resource tick");
            r.next();
            o.note_session_draw(r.state, 1);  // a layout draw is logged after the prefix
            require(o.rng_log().back().phase == StartupRngPhase::Session && o.rng_log().back().state == r.state,
                    "session draw logged in order");
            threw = false;
            try { o.retire_restore_resources(); } catch (const std::logic_error&) { threw = true; }
            require(threw, "retirement before manager death is rejected");
            o.restore_manager_death();
            require(m.allocation(atlas_alloc).has_value(), "window death does not release the atlas");
            o.note_mode_request(8, 1, 0);
            o.retire_restore_resources();
            require(m.allocation(atlas_alloc).has_value(), "the resource child (101058) releases nothing");
            o.release_restore_atlas();
            require(!m.allocation(atlas_alloc) && o.restore_atlas() == -1 && o.handles().record_of(atlas) < 0,
                    "wrapper dtor releases the atlas (1cca30)");
            o.free_restore_resource_node();
            o.free_restore_wrappers();
            // The second title starts from the live state: no reset, records continue from the free list.
            const auto live_before = o.handles().live_order().size();
            o.begin_title_construction();
            require(o.second_title() && live_before == 36, "second title from the live state");
            const int img = o.append_image(effect_xim({19, 256, 64, 32}, 78));
            require(o.handles().record_of(img) >= 0 && o.handles().free_record_count() + 1 < records_before + 2,
                    "a fresh record from the free list");
            o.finish_title_construction();
            require(o.stage() == ResourceStage::TitleConstructed, "second title constructed");
            o.mark_unmodeled("second title updates");
            require(o.stage() == ResourceStage::Unmodeled, "past the stop: unmodeled");
        });
        // Successor walk with a retired handle between loads (hole) and a failed load (-1 skipped).
        scenario([&] {
            Ps2TextureMemory mem; mem.initialize_empty_allocator();
            TextureHandleOwner t(mem);
            const int a = t.create_handle(std::nullopt), b = t.create_handle(std::nullopt), c = t.create_handle(std::nullopt);
            t.retire(b);
            require(t.live_successor(a) == c && t.live_order() == std::vector<int>({a, c}), "retired handle leaves the list");
            const int d = t.create_handle(std::nullopt);
            require(d == b && t.live_successor(c) == d && t.live_successor(d) == -7,
                    "reused id is appended at the tail, not at its old position");
        });
        std::cout << "startup texture tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
