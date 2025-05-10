#include "okage/startup_textures.hpp"

#include <cstdio>
#include <bit>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace okage {
namespace {

// 0x221bf8: 31 names then 0, copied to the stack by 0x103718.
const std::array<const char*, 31> effect_names{
    "niji.xim", "splash_a.xim", "wat_4.xim", "exp9.xim", "f2.xim", "h29_4.xim", "hit!.xim", "spark.xim",
    "smoke_4.xim", "thunder.xim", "oldskilt4.xim", "water.xim", "suuji.xim", "lgtballm4.xim",
    "fuki32b.xim", "all.xim", "all2.xim", "kemuri.xim", "thunderball.xim", "6eff01.xim", "6eff02.xim",
    "5sen.xim", "exp_c.xim", "heart.xim", "onnpu.xim", "wind.xim", "treasure.xim", "page.xim",
    "odoro.xim", "aura.xim", "kyusyu16.xim"};

std::uint16_t le16(std::span<const std::uint8_t> d, std::size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("FNT header outside file");
    return static_cast<std::uint16_t>(d[at] | d[at + 1] << 8);
}

std::vector<std::uint8_t> words_le(const std::array<std::uint32_t, 256>& w) {
    std::vector<std::uint8_t> out;
    out.reserve(1024);
    for (auto v : w)
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    return out;
}

} // namespace

const std::array<const char*, 31>& startup_effect_names() { return effect_names; }

std::vector<std::uint8_t> read_elf_va(std::span<const std::uint8_t> elf, std::uint32_t va, std::size_t size) {
    auto u32 = [&](std::size_t at) {
        if (at + 4 > elf.size()) throw std::runtime_error("ELF truncated");
        return std::uint32_t(elf[at]) | std::uint32_t(elf[at + 1]) << 8 | std::uint32_t(elf[at + 2]) << 16 |
               std::uint32_t(elf[at + 3]) << 24;
    };
    if (elf.size() < 0x34 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
        throw std::runtime_error("not an ELF image");
    const auto phoff = u32(0x1c);
    const auto phentsize = static_cast<std::size_t>(elf[0x2a] | elf[0x2b] << 8);
    const auto phnum = static_cast<std::size_t>(elf[0x2c] | elf[0x2d] << 8);
    for (std::size_t i = 0; i < phnum; ++i) {
        const std::size_t ph = phoff + i * phentsize;
        if (u32(ph) != 1) continue;  // PT_LOAD
        const auto offset = u32(ph + 4), vaddr = u32(ph + 8), filesz = u32(ph + 16);
        if (va >= vaddr && std::uint64_t(va) + size <= std::uint64_t(vaddr) + filesz) {
            const std::size_t at = offset + (va - vaddr);
            if (at + size > elf.size()) throw std::runtime_error("ELF segment outside file");
            return {elf.begin() + at, elf.begin() + at + size};
        }
    }
    throw std::runtime_error("virtual address not backed by file contents");
}

StartupTextureOwner::StartupTextureOwner() : handles_(memory_) {}

StartupTextureOwner::~StartupTextureOwner() {
    try {
        handles_.retire_all();
        for (const auto& l : layers_) {
            if (l.color.serial) memory_.release(l.color.id);
            if (l.depth) memory_.release(l.depth->id);
        }
    } catch (const std::exception& e) {
        // Destructors cannot throw; surface the leak instead of hiding it.
        std::fprintf(stderr, "StartupTextureOwner teardown failed: %s (references may leak)\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "StartupTextureOwner teardown failed (references may leak)\n");
    }
}

const char* to_string(StartupRngPhase p) {
    switch (p) {
        case StartupRngPhase::Noise: return "noise";
        case StartupRngPhase::EffectGrid: return "effect-grid";
        case StartupRngPhase::EffectTemplate: return "effect-template";
    }
    return "unknown";
}

std::uint32_t StartupRandomStream::draw() {
    const auto value = random_->next();
    ++draws_;
    if (record_) {
        if (log_.size() < log_limit) log_.push_back({phase_, random_->state, value});
        else ++dropped_;
    }
    return value;
}

void StartupTextureOwner::checkpoint(const char* step) {
    std::uint32_t live = 0;
    for (TextureAllocationId id = 0; id < 4096; ++id) live += memory_.allocation(id).has_value();
    checkpoints_.push_back({step, live, static_cast<std::uint32_t>(handles_.live_handle_count()),
                            memory_.available_count(), memory_.identity()});
}

void StartupTextureOwner::lifecycle_push(std::string json) {
    if (!record_) return;
    if (lifecycle_.size() < lifecycle_limit) lifecycle_.push_back(std::move(json));
    else ++lifecycle_dropped_;
}

namespace {
std::string json_string(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
} // namespace

void StartupTextureOwner::lifecycle_alloc(const GsTextureAllocation& a) {
    lifecycle_push("{\"kind\":\"alloc\",\"alloc\":" + std::to_string(a.id) + ",\"class\":" +
                   std::to_string(a.allocation_class) + ",\"psm\":" + std::to_string(unsigned(a.psm)) +
                   ",\"w\":" + std::to_string(a.width) + ",\"h\":" + std::to_string(a.height) +
                   ",\"tbp\":" + std::to_string(a.base_block) + "}");
}

void StartupTextureOwner::lifecycle_ref(TextureAllocationId id, int delta) {
    lifecycle_push("{\"kind\":\"ref\",\"alloc\":" + std::to_string(id) + ",\"delta\":" + std::to_string(delta) +
                   ",\"refs\":" + std::to_string(memory_.allocation(id)->references) + "}");
}

void StartupTextureOwner::sync() {
    if (!record_) return;
    using K = HandleEvent::Kind;
    for (const auto& e : handles_.events()) {
        const auto n = [](std::int64_t v) { return std::to_string(v); };
        switch (e.kind) {
            case K::Allocate:
                lifecycle_push("{\"kind\":\"alloc\",\"alloc\":" + n(e.a) + ",\"class\":" + n(e.c >> 56 & 0xff) +
                               ",\"psm\":" + n(e.c >> 48 & 0xff) + ",\"w\":" + n(e.c >> 24 & 0xffffff) +
                               ",\"h\":" + n(e.c & 0xffffff) + ",\"tbp\":" + n(e.b) + "}");
                break;
            case K::Upload: lifecycle_push("{\"kind\":\"upload\",\"alloc\":" + n(e.a) + "}"); break;
            case K::DescriptorRetain:
                lifecycle_push("{\"kind\":\"ref\",\"alloc\":" + n(e.a) + ",\"delta\":1,\"refs\":" + n(e.b) + "}");
                break;
            case K::DescriptorRelease:
                lifecycle_push("{\"kind\":\"ref\",\"alloc\":" + n(e.a) + ",\"delta\":-1,\"refs\":" + n(e.b) + "}");
                break;
            case K::DescriptorFree: lifecycle_push("{\"kind\":\"free\",\"alloc\":" + n(e.a) + "}"); break;
            case K::HandleCreated:
            case K::AliasCreated:
                lifecycle_push("{\"kind\":\"handle\",\"handle\":" + n(e.a) + ",\"record\":" + n(e.b) +
                               ",\"alias\":" + (e.kind == K::AliasCreated ? "true" : "false") + "}");
                break;
            case K::RetireCalled: lifecycle_push("{\"kind\":\"retire\",\"handle\":" + n(e.a) + "}"); break;
            case K::PaletteBind:
                lifecycle_push("{\"kind\":\"bind\",\"handle\":" + n(e.a) + ",\"level\":" + n(e.b) +
                               ",\"alloc\":" + n(e.c) + "}");
                break;
            case K::Named:
                lifecycle_push("{\"kind\":\"name\",\"handle\":" + n(e.a) + ",\"name\":" +
                               (e.b ? json_string(e.text) : std::string("null")) + "}");
                break;
            case K::Reserved:
                lifecycle_push("{\"kind\":\"reserve\",\"count\":" + n(e.a) + ",\"first\":" + n(e.b) +
                               ",\"class\":0}");
                break;
            default: break;
        }
    }
    handles_.clear_events();
}

void StartupTextureOwner::note_allocation(TextureAllocationId id) {
    if (!record_) return;
    timeline_.push_back({StartupTimelineEvent::Kind::Allocation, id, memory_.allocation(id)->base_block, nullptr,
                         stream_->draws(), stream_->state()});
}

void StartupTextureOwner::note_upload(TextureAllocationId id) {
    if (!record_) return;
    timeline_.push_back({StartupTimelineEvent::Kind::Upload, id, memory_.allocation(id)->base_block, nullptr,
                         stream_->draws(), stream_->state()});
}

void StartupTextureOwner::note_phase(StartupTimelineEvent::Kind kind, const char* phase) {
    if (kind == StartupTimelineEvent::Kind::PhaseBegin) phase_ = phase;
    sync();
    if (kind == StartupTimelineEvent::Kind::PhaseBegin)
        lifecycle_push(std::string("{\"kind\":\"phase\",\"name\":\"") + phase + "\"}");
    if (!record_) return;
    timeline_.push_back({kind, 0, 0, phase, stream_->draws(), stream_->state()});
}

TextureDescriptor StartupTextureOwner::allocate_layer_buffer(std::uint8_t cls, GsPixelStorage psm) {
    // 0x1cf008(class, psm, 640, 224); a failure here is a hard error: the
    // retail startup assumes success and no failure path is modeled (U11).
    sync();
    const auto a = memory_.allocate(cls, psm, 640, 224);
    if (!a) throw std::runtime_error("startup: colour/depth allocation failed");
    lifecycle_alloc(*a);
    note_allocation(a->id);
    return {a->id, a->serial};
}

namespace {
struct FontAtlas {
    std::uint16_t width{}, height{};
    std::span<const std::uint8_t> payload;
};

// 0x1947a0 header mode 0: glyph code range [buf[4], buf[5]], 0x14-byte glyph
// records from offset 8, then LE16 height, LE16 width and W*H/2 payload bytes.
FontAtlas parse_font_atlas(const std::vector<std::uint8_t>& f) {
    if (f.size() < 8) throw std::runtime_error("startup: font file too short for its header");
    if (f[5] < f[4]) throw std::runtime_error("startup: font glyph range end precedes its start");
    const std::size_t count = std::size_t(f[5]) - f[4] + 1u;
    const std::size_t header = 8 + 0x14u * count;
    if (header > f.size() || f.size() - header < 4) throw std::runtime_error("startup: font atlas header outside file");
    FontAtlas atlas;
    atlas.height = le16(f, header);
    atlas.width = le16(f, header + 2);
    if (!atlas.width || !atlas.height || atlas.width > 2048 || atlas.height > 2048 || (std::size_t(atlas.width) * atlas.height) % 32)
        throw std::runtime_error("startup: font atlas dimensions outside the transfer domain");
    const std::size_t bytes = std::size_t(atlas.width) * atlas.height / 2;
    if (f.size() - header - 4 < bytes) throw std::runtime_error("startup: font atlas payload outside file");
    atlas.payload = std::span<const std::uint8_t>(f.data() + header + 4, bytes);  // verbatim file tail
    return atlas;
}
} // namespace

void StartupTextureOwner::run_startup(const StartupTextureInputs& in, GameRandom& random,
                                      const StartupEffectPreparation& prepare_effects) {
    if (started_) throw std::logic_error("startup texture lifetime already initialized (re-initialization rejected)");
    started_ = true;
    StartupRandomStream stream(random, in.record_observations);
    stream_ = &stream;
    record_ = in.record_observations;
    if (record_) handles_.set_event_log_limit(lifecycle_limit);
    try {
        if (in.font_palette.size() != 64) throw std::invalid_argument("startup: font palette must be 64 bytes");
        if (!in.read_effect) throw std::invalid_argument("startup: effect reader missing");
        if (!prepare_effects) throw std::invalid_argument("startup: effect preparation hook missing");
        std::array<FontAtlas, 3> fonts;
        for (std::size_t i = 0; i < 3; ++i) fonts[i] = parse_font_atlas(in.fonts[i]);  // validated before any state

        auto require = [](bool ok, const char* what) { if (!ok) throw std::runtime_error(what); };
        auto retain = [&](const TextureDescriptor& d) {
            sync();
            require(memory_.retain(d.id), "startup: retain failed");
            lifecycle_ref(d.id, 1);
        };
        auto image = [&](GsPixelStorage psm, std::uint16_t w, std::uint16_t h, std::span<const std::uint8_t> px) {
            const int handle = handles_.create_image(psm, w, h, px);
            require(handle >= 0, "startup: image creation failed");
            const auto id = handles_.image(handle)->id;
            note_allocation(id);
            if (!px.empty()) note_upload(id);
            return handle;
        };
        auto palette = [&](std::uint32_t count, std::span<const std::uint8_t> entries) {
            bool uploaded = false;
            const auto pal = handles_.create_palette(count, entries, uploaded);
            require(pal.has_value() && uploaded == !entries.empty(), "startup: palette allocation failed");
            note_allocation(pal->id);
            if (uploaded) note_upload(pal->id);
            return *pal;
        };

        // Step 0 (1003d8 -> 1cb088): 1ce940 allocator init, handle tables (owner ctor), palette mode 0.
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "allocator-init");
        memory_.initialize_empty_allocator();
        checkpoint("allocator-init");

        // Step 1 (100428 -> 1cb1e8): two 640x224 colour layers (class 2, PSM0), then shared PSM49 depth (class 3).
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "colour-depth");
        for (int i = 0; i < 2; ++i) {
            layers_[i].color = allocate_layer_buffer(2, GsPixelStorage::Psmct32);
            retain(layers_[i].color);
        }
        const auto depth = allocate_layer_buffer(3, GsPixelStorage::Psmz24);
        for (int i = 0; i < 2; ++i) {
            retain(depth);
            layers_[i].depth = depth;
        }
        checkpoint("colour-depth");

        // Step 1b (100490 -> 1974f8(0x100)): glyph-cache texture (no upload) + 16-entry CLUT.
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "glyph-cache");
        glyph_cache_handle_ = image(GsPixelStorage::Psmt4, 1024, 256, {});
        handles_.bind_palette(glyph_cache_handle_, 0, palette(16, in.font_palette));
        checkpoint("glyph-cache");

        // Step 2 (1004b8 -> 18fc68(0)); a0=0 takes every flag branch.
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "capture-filter");
        {
            stream.enter(StartupRngPhase::Noise);
            note_phase(StartupTimelineEvent::Kind::PhaseBegin, "noise");
            std::array<std::uint32_t, 256> words{};
            for (auto& w : words) {  // 192f10-192f48
                std::uint32_t x = 0x80000000u;
                x |= (stream.draw() << 3) & 0xff00u;
                x |= (stream.draw() >> 5) & 0xffu;
                w = x;
            }
            const auto pal_a = palette(256, words_le(words));  // 192f54
            for (auto& w : words) {  // 192f68-192f94
                const std::uint32_t v = (stream.draw() >> 5) & 0xffu;
                w |= (v << 16) | (v << 8) | v;
            }
            const auto pal_b = palette(256, words_le(words));  // 192fa0
            std::vector<std::uint8_t> pixels(0x40 * 0x80);
            for (auto& p : pixels) p = static_cast<std::uint8_t>((stream.draw() >> 5) & 0xffu);  // 192fc0-193008
            rand_state_after_noise_ = stream.state();
            note_phase(StartupTimelineEvent::Kind::PhaseEnd, "noise");
            noise_handle_ = image(GsPixelStorage::Psmt8, 0x40, 0x80, pixels);  // 193018
            handles_.bind_palette(noise_handle_, 0, pal_a);
            handles_.bind_palette(noise_handle_, 1, pal_b);
        }
        //  18f8c0: capture target (class 2 PSM0 640x224) sharing the depth buffer.
        layers_[2].color = allocate_layer_buffer(2, GsPixelStorage::Psmct32);
        retain(layers_[2].color);
        retain(*layers_[0].depth);
        layers_[2].depth = layers_[0].depth;
        //  190f38 and 191030: 256-entry CLUTs with NULL source: allocated, never uploaded, never retained.
        for (auto& clut : filter_cluts_) clut = palette(256, {});
        checkpoint("capture-filter");

        // Step 3 (100514 -> 1011d0 -> 103a88): 103718 loads the 31 shared effects through 18f7a8 ...
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "effect-uploads");
        for (const char* name : effect_names) {
            const auto bytes = in.read_effect(name);
            require(bytes.has_value(), "startup: effect file missing");
            const auto report = handles_.load_xim(OwnedRawXim(*bytes));
            require(report.handle >= 0, "startup: effect load failed");
            const auto id = handles_.image(report.handle)->id;
            note_allocation(id);
            note_upload(id);
            for (const auto& level : report.palettes) {
                require(level.palette.has_value() && level.uploaded, "startup: effect palette allocation failed");
                note_allocation(level.palette->id);
                note_upload(level.palette->id);
            }
            effect_handles_.push_back(report.handle);
        }
        checkpoint("effects");
        // ... then 10bc78 prepares the EFD bank on the same stream (grid 10e098, then templates).
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "effect-preparation");
        stream.enter(StartupRngPhase::EffectGrid);
        prepare_effects(stream);
        note_phase(StartupTimelineEvent::Kind::PhaseEnd, "effect-preparation");

        // Step 4 (100524 -> 1001a8 -> 1947a0): three fonts; PSM selected by flags (0x1000 -> 44, 0x2000 -> 36, 0 -> 20).
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "fonts");
        static constexpr std::array<GsPixelStorage, 3> font_psm{GsPixelStorage::Psmt4Hh, GsPixelStorage::Psmt4Hl,
                                                                GsPixelStorage::Psmt4};
        for (std::size_t i = 0; i < 3; ++i) {
            font_handles_[i] = image(font_psm[i], fonts[i].width, fonts[i].height, fonts[i].payload);
            handles_.bind_palette(font_handles_[i], 0, palette(16, in.font_palette));
        }
        checkpoint("fonts");
        note_phase(StartupTimelineEvent::Kind::PhaseBegin, "boundary");

        // The frozen contract totals; any deviation means the inputs are not the verified retail ones.
        std::uint32_t live = 0;
        for (TextureAllocationId id = 0; id < 4096; ++id) live += memory_.allocation(id).has_value();
        require(live == expected_allocations && std::uint32_t(handles_.live_handle_count()) == expected_handles,
                "startup: allocation/handle totals differ from the frozen contract");
        rng_draws_ = stream.draws();
        rng_log_ = stream.log();
        sync();
        stream_ = nullptr;
        coverage_ = StartupCoverage::PreSchedulerOnly;
    } catch (...) {
        failed_phase_ = phase_;
        sync();
        rng_draws_ = stream.draws();
        rng_log_ = stream.log();
        stream_ = nullptr;
        throw;
    }
}

void write_effect_preparation_report(const EffectTemplatePreparation& prepared, const EffectPreparationTrace& trace,
                                     const std::filesystem::path& dir) {
    std::ostringstream o;
    auto hex32 = [](float f) {
        char b[11];
        std::snprintf(b, sizeof b, "0x%08x", std::bit_cast<std::uint32_t>(f));
        return std::string("\"") + b + "\"";
    };
    o << "{\"grid\":[";
    for (std::size_t i = 0; i < prepared.noise.size(); ++i) o << (i ? "," : "") << prepared.noise[i];
    o << "],\n\"templates\":[";
    for (std::size_t n = 0; n < trace.construction_order.size(); ++n) {
        const auto shape = trace.construction_order[n];
        std::array<std::uint32_t, 2> owner{};
        for (const auto& r : trace.requests)
            if (r.shape == shape && r.constructed) { owner = {r.definition, r.element}; break; }
        o << (n ? ",\n" : "\n") << "{\"shape\":" << shape << ",\"owner\":[" << owner[0] << "," << owner[1]
          << "],\"order\":" << n << ",\"draws\":" << trace.construction_draws[shape] << ",\"vertices\":[";
        const auto& g = prepared.geometry[shape];
        if (!g) throw std::runtime_error("effect report: constructed template missing");
        for (std::size_t v = 0; v < g->vertices.size(); ++v) {
            const auto& x = g->vertices[v];
            o << (v ? "," : "") << "[";
            bool first = true;
            for (const auto& channel : {x.position, x.color, x.uv})
                for (float f : channel) { o << (first ? "" : ",") << hex32(f); first = false; }
            o << "]";
        }
        o << "],\"position_tags\":[";
        for (std::size_t v = 0; v < g->position_tags.size(); ++v) o << (v ? "," : "") << unsigned(g->position_tags[v]);
        o << "],\"uv_tags\":[";
        for (std::size_t v = 0; v < g->uv_tags.size(); ++v) o << (v ? "," : "") << unsigned(g->uv_tags[v]);
        o << "]}";
    }
    o << "],\n\"cache_owners\":[";
    bool first = true;
    for (std::size_t shape = 0; shape < prepared.cache_owners.size(); ++shape) {
        const auto& c = prepared.cache_owners[shape];
        if (!c[0] && !c[1]) continue;
        o << (first ? "" : ",") << "[" << shape << "," << c[0] << "," << c[1] << "]";
        first = false;
    }
    o << "],\n\"requests\":[";
    for (std::size_t i = 0; i < trace.requests.size(); ++i) {
        const auto& r = trace.requests[i];
        o << (i ? "," : "") << "{\"def\":" << r.definition << ",\"elem\":" << r.element << ",\"shape\":" << r.shape
          << ",\"constructed\":" << (r.constructed ? "true" : "false") << "}";
    }
    o << "]}\n";
    std::ofstream(dir / "effects.json", std::ios::binary) << o.str();
}

void write_startup_texture_report(const StartupTextureOwner& owner, const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    if (fs::exists(dir)) throw std::runtime_error("startup texture report directory already exists");
    fs::create_directories(dir);
    const auto& m = owner.memory();
    const auto& h = owner.handles();
    std::ostringstream o;
    auto desc = [](const OptDescriptor& d) {
        return d ? std::string("[") + std::to_string(d->id) + "," + std::to_string(d->serial) + "]" : std::string("null");
    };
    o << "{\n \"coverage\":\"" << (owner.coverage() == StartupCoverage::PreSchedulerOnly ? "pre-scheduler-only" : "not-run")
      << "\",\n \"later_history\":\"unmodeled: title, models, field, battle, XTA, frees, filter display-list kicks\",";
    o << "\n \"rand_state_after_noise\":" << owner.rand_state_after_noise() << ",";
    o << "\n \"rng_draws\":" << owner.rng_draws() << ",";
    o << "\n \"checkpoints\":[";
    bool first = true;
    for (const auto& c : owner.checkpoints()) {
        o << (first ? "" : ",") << "{\"step\":\"" << c.step << "\",\"allocations\":" << c.allocations
          << ",\"live_handles\":" << c.live_handles << ",\"available\":" << c.available
          << ",\"content_generation\":" << c.identity.content_generation
          << ",\"ownership_epoch\":" << c.identity.ownership_epoch << "}";
        first = false;
    }
    o << "],\n \"allocations\":[";
    first = true;
    for (TextureAllocationId id = 0; id < 4096; ++id) {
        const auto a = m.allocation(id);
        if (!a) continue;
        o << (first ? "\n  " : ",\n  ") << "{\"id\":" << a->id << ",\"serial\":" << a->serial
          << ",\"class\":" << unsigned(a->allocation_class) << ",\"psm\":" << unsigned(a->psm)
          << ",\"width\":" << a->width << ",\"height\":" << a->height << ",\"tbp\":" << a->base_block
          << ",\"tbw\":" << unsigned(a->buffer_width_64) << ",\"references\":" << a->references
          << ",\"descriptor\":" << a->descriptor << "}";
        first = false;
    }
    o << "],\n \"layers\":[";
    first = true;
    for (const auto& l : owner.layers()) {
        o << (first ? "" : ",") << "{\"color\":" << desc(l.color) << ",\"depth\":" << desc(l.depth) << "}";
        first = false;
    }
    o << "],\n \"filter_cluts\":[" << desc(owner.filter_cluts()[0]) << "," << desc(owner.filter_cluts()[1]) << "]";
    o << ",\n \"handles\":[";
    first = true;
    for (int hd = 0; hd < TextureHandleOwner::handle_capacity; ++hd) {
        const int r = h.record_of(hd);
        if (r < 0) continue;
        o << (first ? "\n  " : ",\n  ") << "{\"handle\":" << hd << ",\"record\":" << r
          << ",\"image\":" << desc(h.image(hd)) << ",\"palettes\":[";
        const int levels = 1 + h.extra_chain_length(hd);
        for (int l = 0; l < levels; ++l) o << (l ? "," : "") << desc(h.palette(hd, l));
        o << "]}";
        first = false;
    }
    o << "],\n \"records\":[";
    first = true;
    for (int r = 0; r < TextureHandleOwner::record_capacity; ++r) {
        if (h.record_handle_count(r) == 0) continue;
        o << (first ? "" : ",") << "{\"record\":" << r << ",\"name\":\"" << h.record_name(r) << "\",\"hash_slot\":"
          << h.record_hash_slot(r) << ",\"handles\":" << h.record_handle_count(r) << "}";
        first = false;
    }
    o << "],\n \"hash\":[";
    first = true;
    for (const auto& [slot, value] : h.hash_entries()) { o << (first ? "" : ",") << "[" << slot << "," << value << "]"; first = false; }
    o << "],\n \"reserved\":[";
    first = true;
    for (int id : h.reserved_handles()) { o << (first ? "" : ",") << id; first = false; }
    o << "],\n \"free_records\":" << h.free_record_count() << ",\"free_extras\":" << h.free_extra_count();
    o << ",\n \"available\":" << m.available_count() << ",\n \"cursors\":[";
    first = true;
    for (auto c : m.search_cursors()) { o << (first ? "" : ",") << c; first = false; }
    o << "],\n \"free_queue_ids\":[";
    first = true;
    for (auto id : m.free_queue_ids()) { o << (first ? "" : ",") << id; first = false; }
    o << "],\n \"occupancy_hex\":\"";
    static const char* hex = "0123456789abcdef";
    for (auto b : m.occupancy_bytes()) o << hex[b >> 4] << hex[b & 15];
    o << "\"\n}\n";
    std::ofstream(dir / "state.json", std::ios::binary) << o.str();
    {
        std::ofstream rng(dir / "rng.jsonl", std::ios::binary);
        std::uint64_t i = 0;
        for (const auto& d : owner.rng_log())
            rng << "{\"i\":" << i++ << ",\"phase\":\"" << to_string(d.phase) << "\",\"state\":" << d.state
                << ",\"value\":" << d.value << "}\n";
        std::ofstream lc(dir / "lifecycle.jsonl", std::ios::binary);
        for (const auto& line : owner.lifecycle()) lc << line << "\n";
        std::ofstream tl(dir / "timeline.jsonl", std::ios::binary);
        for (const auto& e : owner.timeline()) {
            using K = StartupTimelineEvent::Kind;
            tl << "{\"kind\":\"" << (e.kind == K::Allocation ? "alloc" : e.kind == K::Upload ? "upload"
                                      : e.kind == K::PhaseBegin ? "phase-begin" : "phase-end") << "\"";
            if (e.kind == K::Allocation || e.kind == K::Upload)
                tl << ",\"alloc\":" << e.allocation << ",\"tbp\":" << e.base_block;
            else
                tl << ",\"phase\":\"" << e.phase << "\"";
            tl << ",\"draws\":" << e.draws << ",\"rng_state\":" << e.rng_state << "}\n";
        }
    }
    std::vector<std::uint8_t> bytes(Ps2TextureMemory::vram_bytes), known(Ps2TextureMemory::vram_bytes);
    for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a) {
        const auto ob = m.observe_byte_masked(a);
        bytes[a] = ob.value;
        known[a] = ob.known_mask;
    }
    std::ofstream(dir / "vram.bytes", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::ofstream(dir / "vram.known", std::ios::binary).write(reinterpret_cast<const char*>(known.data()), known.size());
}

} // namespace okage
