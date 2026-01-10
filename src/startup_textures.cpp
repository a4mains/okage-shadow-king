#include "okage/startup_textures.hpp"
#include "okage/sha256.hpp"
#include "okage/texture_transfer.hpp"

#include <algorithm>
#include <map>
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

StartupTextureOwner::StartupTextureOwner() : handles_(memory_), resources_(handles_), models_(resources_) {}

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
        case StartupRngPhase::Session: return "session";
        case StartupRngPhase::RestoreUiGlyph: return "restore-ui-glyph";
        case StartupRngPhase::RestoreUiIcon: return "restore-ui-icon";
        case StartupRngPhase::RestoreUiSelect: return "restore-ui-select";
        case StartupRngPhase::NameUiGlyph: return "name-ui-glyph";
        case StartupRngPhase::NameUiIcon: return "name-ui-icon";
        case StartupRngPhase::NameUiSelect: return "name-ui-select";
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
            case K::Upload:
                lifecycle_push("{\"kind\":\"upload\",\"alloc\":" + n(e.a) + "}");
                if (!e.text.empty())  // Restore: the exact transmitted bytes (original 0x1c9b48 qwc*16 hash)
                    lifecycle_push("{\"kind\":\"upload-bytes\",\"alloc\":" + n(e.a) + ",\"bytes\":" + n(e.b) +
                                   ",\"sha256\":\"" + e.text + "\"}");
                break;
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
                               ",\"class\":" + n(e.c) + "}");
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

        // 1004d0 -> 18e4c0: two real embedded one-model, zero-texture groups.
        models_.insert("embedded-1fece0", 1, ResourceTextures{}, false);
        models_.insert("embedded-1ffc90", 1, ResourceTextures{}, false);

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

const char* to_string(ResourceStage s) {
    switch (s) {
        case ResourceStage::PreScheduler: return "pre-scheduler";
        case ResourceStage::Scheduled: return "scheduled";
        case ResourceStage::LogosConstructed: return "logos-constructed";
        case ResourceStage::LogosRetired: return "logos-retired";
        case ResourceStage::TitleConstructed: return "title-constructed";
        case ResourceStage::TitleActive: return "title-active";
        case ResourceStage::TitleRemovalRequested: return "title-removal-requested";
        case ResourceStage::TitleResourcesRetired: return "title-resources-retired";
        case ResourceStage::TitleWrappersFreed: return "title-wrappers-freed";
        case ResourceStage::SuccessorEntry: return "successor-entered";
        case ResourceStage::RestoreActive: return "restore-active";
        case ResourceStage::RestoreManagerDead: return "restore-manager-dead";
        case ResourceStage::RestoreResourcesRetired: return "restore-resources-retired";
        case ResourceStage::RestoreWrappersFreed: return "restore-wrappers-freed";
        case ResourceStage::NameEntryActive: return "name-entry-active";
        case ResourceStage::NameEntryManagerDead: return "name-entry-manager-dead";
        case ResourceStage::NameEntryCommandPublished: return "name-entry-command-published";
        case ResourceStage::NameEntryResourcesRetired: return "name-entry-resources-retired";
        case ResourceStage::NameEntryWrappersFreed: return "name-entry-wrappers-freed";
        case ResourceStage::ChapterActive: return "chapter-active";
        case ResourceStage::ChapterModeRequested: return "chapter-mode-requested";
        case ResourceStage::ChapterResourcesRetired: return "chapter-resources-retired";
        case ResourceStage::ChapterWrappersFreed: return "chapter-wrappers-freed";
        case ResourceStage::FieldConstructorEntry: return "field-constructor-entry";
        case ResourceStage::FieldConstructed: return "field-constructed";
        case ResourceStage::FieldWorkerEntered: return "field-worker-entered";
        case ResourceStage::FieldAtlasLoaded: return "field-atlas-loaded";
        case ResourceStage::FieldTexenvLoaded: return "field-texenv-loaded";
        case ResourceStage::FieldRuwLoaded: return "field-ruw-loaded";
        case ResourceStage::FieldWorldLoading: return "field-world-loading";
        case ResourceStage::FieldWorldComplete: return "field-world-complete";
        case ResourceStage::FieldMapCommitted: return "field-map-committed";
        case ResourceStage::FieldActive: return "field-active";
        case ResourceStage::FieldResourcesRetired: return "field-resources-retired";
        case ResourceStage::FieldWrappersFreed: return "field-wrappers-freed";
        case ResourceStage::BattleLoading: return "battle-loading";
        case ResourceStage::BattleActive: return "battle-active";
        case ResourceStage::BattleRootRetired: return "battle-root-retired";
        case ResourceStage::BattleResourcesRetired: return "battle-resources-retired";
        case ResourceStage::BattleWrappersFreed: return "battle-wrappers-freed";
        case ResourceStage::FieldUiPublished: return "field-ui-published";
        case ResourceStage::Unmodeled: return "unmodeled";
    }
    return "unknown";
}

const char* to_string(MemoryContentCoverage c) {
    return c == MemoryContentCoverage::UploadsExact ? "uploads-exact" : "draw-targets-invalidated";
}

void StartupTextureOwner::require_stage(ResourceStage expected, const char* operation) const {
    if (coverage_ != StartupCoverage::PreSchedulerOnly)
        throw std::logic_error(std::string(operation) + ": the startup prefix did not complete");
    if (stage_ != expected)
        throw std::logic_error(std::string(operation) + " out of source order (stage " + to_string(stage_) + ")");
}

void StartupTextureOwner::advance(ResourceStage next) {
    stage_ = next;
    sync();
    lifecycle_push(std::string("{\"kind\":\"phase\",\"name\":\"") + to_string(next) + "\"}");
}

void StartupTextureOwner::begin_scheduled_frames() {
    require_stage(ResourceStage::PreScheduler, "begin_scheduled_frames");
    for (std::size_t i = 0; i < 2; ++i) memory_.invalidate_owned_nibbles(layers_[i].color.id);
    memory_.invalidate_owned_nibbles(layers_[0].depth->id);  // Z24 lanes only; font top-byte lanes stay
    memory_coverage_ = MemoryContentCoverage::DrawTargetsInvalidated;
    advance(ResourceStage::Scheduled);
}

void StartupTextureOwner::submit_main_loop() {
    require_modeled("1cb648 main-loop submission");
    if(stage_==ResourceStage::PreScheduler)throw std::logic_error("GS submission before100618");
    gs_buffer_selector_^=1; ++gs_submissions_;
}
void StartupTextureOwner::capture_previous_frame(std::uint64_t expected_submissions) {
    require_stage(ResourceStage::FieldActive,"114d50 field capture");
    if(gs_submissions_!=expected_submissions)throw std::logic_error("Capture GS submission history is incomplete");
    const auto& source_binding=layers_[gs_buffer_selector_^1].color;
    const auto& destination_binding=layers_[2].color;
    const auto source=memory_.allocation(source_binding.id),destination=memory_.allocation(destination_binding.id);
    if(!source||!destination||source->serial!=source_binding.serial||destination->serial!=destination_binding.serial)
        throw std::logic_error("Capture layer allocation incarnation changed");
    const GsLocalToLocal transfer{source->descriptor,destination->descriptor,0,0,0,0,640,224,0};
    transfer_local_to_local(memory_,transfer);
    lifecycle_push("{\"kind\":\"capture\",\"op\":\"local-to-local\",\"source_selector\":"+
        std::to_string(gs_buffer_selector_^1)+",\"submissions\":"+std::to_string(gs_submissions_)+
        ",\"width\":640,\"height\":224}");
}

void StartupTextureOwner::begin_logo_construction() {
    require_stage(ResourceStage::Scheduled, "logo construction");
    if (!image_list_.empty()) throw std::logic_error("logo construction with a live image list");
    advance(ResourceStage::LogosConstructed);
}

int StartupTextureOwner::append_image(const std::optional<std::vector<std::uint8_t>>& file) {
    if (stage_ != ResourceStage::LogosConstructed && stage_ != ResourceStage::LogosRetired &&
        stage_ != ResourceStage::ChapterActive)
        throw std::logic_error("image-list append outside a logo/title/chapter constructor");
    if (!file) return -1;  // 1b2348: read failure, no append
    if (image_list_.size() >= 0x40) throw std::runtime_error("image list overflow (source has no bound check)");
    const auto report = handles_.load_xim(OwnedRawXim(*file));
    image_list_.push_back(report.handle);  // an 18f7a8 failure appends -1
    sync();
    return report.handle;
}

void StartupTextureOwner::retire_image_list() {
    require_stage(ResourceStage::LogosConstructed, "logo resource retirement");
    for (int handle : image_list_) handles_.retire(handle);  // 1cca30(-1) is a no-op
    image_list_.clear();
    models_.reset_instances_and_registrations(); // resource destructor1010f4
    advance(ResourceStage::LogosRetired);
}

void StartupTextureOwner::begin_title_construction() {
    if (stage_ == ResourceStage::RestoreWrappersFreed) {
        second_title_ = true;
        advance(ResourceStage::LogosRetired);
    }
    require_stage(ResourceStage::LogosRetired, "title construction");
    sync();
    lifecycle_push("{\"kind\":\"phase\",\"name\":\"title-construction\"}");
}

XmdLoadResult StartupTextureOwner::load_and_bind_model(const std::string& model, std::span<const std::string> names,
                                                       const TextureHandleOwner::FileReader& reader) {
    require_stage(ResourceStage::LogosRetired, "model load");
    std::vector<OwnedBytes> read_bytes;  // one entry per file read, in read order
    const TextureHandleOwner::FileReader capture = [&](std::string& name) {
        auto bytes = reader(name);
        read_bytes.push_back(bytes ? std::make_shared<const std::vector<std::uint8_t>>(*bytes) : nullptr);
        return bytes;
    };
    auto result = handles_.load_xmd_names(names, capture);
    sync();
    model_binding_.clear();
    model_slot_bytes_.clear();
    if (result.first < 0) return result;  // 12c350 returns early; no binding
    title_model_ = models_.insert(model, 1, ResourceTextures{}, false).range;
    std::map<int, OwnedBytes> created;
    std::size_t read_index = 0;
    for (const auto& outcome : result.names) {
        if (outcome.kind == XmdNameOutcome::Kind::CacheHit) continue;  // no read
        const auto& bytes = read_bytes.at(read_index++);
        if (outcome.kind == XmdNameOutcome::Kind::Loaded && outcome.handle >= 0) created[outcome.handle] = bytes;
    }
    // 0x182e28: count = name count; handles[i] = h; h = 0x1cc778(h).
    int h = result.first;
    for (std::size_t i = 0; i < names.size(); ++i) {
        model_binding_.push_back(h);
        const auto found = created.find(h);
        model_slot_bytes_.push_back(found == created.end() ? nullptr : found->second);
        lifecycle_push("{\"kind\":\"material\",\"model\":" + json_string(model) + ",\"slot\":" +
                       std::to_string(i) + ",\"handle\":" + std::to_string(h) + "}");
        h = handles_.live_successor(h);
    }
    models_.allocate_instance(*title_model_, {}, 0); // 12c350 -> 1b8a80
    return result;
}

void StartupTextureOwner::mark_unmodeled(const char* reason) {
    if (stage_ == ResourceStage::Unmodeled) return;
    unmodeled_reason_ = reason;
    advance(ResourceStage::Unmodeled);
}

void StartupTextureOwner::finish_title_construction() {
    require_stage(ResourceStage::LogosRetired, "title construction return");
    advance(ResourceStage::TitleConstructed);
}

void StartupTextureOwner::begin_title_updates() {
    require_stage(ResourceStage::TitleConstructed, "first title update");
    advance(ResourceStage::TitleActive);
}

void StartupTextureOwner::note_checkpoint(const char* name) {
    const bool second_return = second_title_ && stage_ == ResourceStage::TitleConstructed;
    if (!second_return && (stage_ < ResourceStage::TitleActive || stage_ == ResourceStage::Unmodeled))
        throw std::logic_error("checkpoint outside the modeled title/Restore stages");
    sync();
    lifecycle_push(std::string("{\"kind\":\"phase\",\"name\":\"") + name + "\"}");
}

void StartupTextureOwner::title_event(ResourceStage from, ResourceStage to, std::string json) {
    require_stage(from, "title lifecycle event");
    sync();
    lifecycle_push(std::move(json));
    if (to != from) advance(to);
}

void StartupTextureOwner::require_modeled(const char* operation) const {
    if (coverage_ != StartupCoverage::PreSchedulerOnly)
        throw std::logic_error(std::string(operation) + ": the startup prefix did not complete");
    if (stage_ == ResourceStage::Unmodeled) throw std::logic_error(std::string(operation) + " after unmodeled history");
}

void StartupTextureOwner::note_mode_request(unsigned mode, unsigned selector, unsigned arg) {
    require_modeled("mode request");
    sync();
    lifecycle_push("{\"kind\":\"mode\",\"op\":\"request\",\"mode\":" + std::to_string(mode) + ",\"selector\":" +
                   std::to_string(selector) + ",\"arg\":" + std::to_string(arg) + "}");
}

void StartupTextureOwner::note_mode_resource_retire(unsigned selector, unsigned mode) {
    require_modeled("resource destructor");
    sync();
    lifecycle_push("{\"kind\":\"mode\",\"op\":\"resource-retire\",\"mode\":" + std::to_string(mode) +
                   ",\"selector\":" + std::to_string(selector) + "}");
    lifecycle_push("{\"kind\":\"effect\",\"op\":\"remove-request\",\"slot\":-2}");  // 101240 -> 103640(-2)
}

void StartupTextureOwner::note_mode_destroy(unsigned selector, unsigned mode) {
    require_modeled("mode destructor");
    sync();
    lifecycle_push("{\"kind\":\"mode\",\"op\":\"destroy\",\"mode\":" + std::to_string(mode) +
                   ",\"selector\":" + std::to_string(selector) + "}");
}

void StartupTextureOwner::note_mode_wrapper_free(unsigned selector, unsigned mode, bool mode_node) {
    require_modeled("wrapper free");
    sync();
    const char* node = !mode_node ? "resource" : mode == 8 ? "mode8" : mode == 9 ? "mode9" : mode == 2 ? "mode2" : mode == 3 ? "mode3" : "mode10";
    lifecycle_push(std::string("{\"kind\":\"mode\",\"op\":\"wrapper-free\",\"node\":\"") + node +
                   "\",\"mode\":" + std::to_string(mode) + ",\"selector\":" + std::to_string(selector) + "}");
}

void StartupTextureOwner::note_session_draw(std::uint32_t state, std::uint32_t frame, unsigned site) {
    if (!record_ || stage_ == ResourceStage::Unmodeled || coverage_ != StartupCoverage::PreSchedulerOnly) return;
    if (rng_log_.size() >= StartupRandomStream::log_limit) {
        mark_unmodeled("session RNG log limit reached (draws past it are not logged)");
        return;
    }
    const bool restore = stage_ >= ResourceStage::RestoreActive && stage_ <= ResourceStage::RestoreWrappersFreed;
    const bool name = stage_ >= ResourceStage::NameEntryActive && stage_ <= ResourceStage::NameEntryWrappersFreed;
    const auto phase = (!restore && !name) || site == 0 ? StartupRngPhase::Session
                       : restore ? (site == 1 ? StartupRngPhase::RestoreUiGlyph
                                    : site == 2 ? StartupRngPhase::RestoreUiIcon : StartupRngPhase::RestoreUiSelect)
                       : site == 1 ? StartupRngPhase::NameUiGlyph
                       : site == 2 ? StartupRngPhase::NameUiIcon : StartupRngPhase::NameUiSelect;
    rng_log_.push_back({phase, state, state & 0x7fffffffu});
    session_draw_frames_.push_back(frame);
    ++rng_draws_;
}

int StartupTextureOwner::construct_restore(const std::vector<std::uint8_t>& atlas) {
    require_stage(ResourceStage::SuccessorEntry, "Restore construction");
    if (successor_mode_ != 10) throw std::logic_error("Restore construction for a non-Restore successor");
    sync();
    const auto report = handles_.load_xim(OwnedRawXim(atlas));  // 18f7a8: authored PSM19/CPSM0, unnamed
    wrapper_atlas_ = report.handle;
    field_.atlas_global_24f730 = report.handle;
    sync();
    advance(ResourceStage::RestoreActive);
    return wrapper_atlas_;
}

void StartupTextureOwner::note_effect_reclaim(int slot) {
    if (stage_ != ResourceStage::RestoreActive && stage_ != ResourceStage::NameEntryActive)
        throw std::logic_error("effect reclaim outside the Restore/name-entry resource tick");
    sync();
    lifecycle_push("{\"kind\":\"effect\",\"op\":\"reclaim\",\"slot\":" + std::to_string(slot) + "}");
    if (effect106_ && effect106_->slot == slot) {
        effect106_->state = "reclaimed";
        effect106_->shape_known = false;
        effect106_->element_handles.clear();
    }
}

void StartupTextureOwner::restore_manager_death() {
    require_stage(ResourceStage::RestoreActive, "Restore manager death");
    advance(ResourceStage::RestoreManagerDead);
}

void StartupTextureOwner::retire_restore_resources() {
    require_stage(ResourceStage::RestoreManagerDead, "Restore resource retirement");
    note_mode_resource_retire(0, 10);
}

void StartupTextureOwner::release_restore_atlas() {
    require_stage(ResourceStage::RestoreManagerDead, "Restore atlas release");
    note_mode_destroy(0, 10);
    sync();
    handles_.retire(wrapper_atlas_);   // 124448 tail 1cca30(wrapper+0x30): image then CLUT freed, never hashed
    wrapper_atlas_ = -1;
    advance(ResourceStage::RestoreResourcesRetired);
}

void StartupTextureOwner::note_ui(const std::string& op, int value) {
    if (stage_ != ResourceStage::RestoreActive && stage_ != ResourceStage::RestoreManagerDead &&
        stage_ != ResourceStage::NameEntryActive && stage_ != ResourceStage::NameEntryManagerDead)
        throw std::logic_error("UI row outside the Restore/name-entry stages");
    sync();
    const char* key = op == "state" ? "state" : op == "window-create" || op == "window-death" ? "window" : "mode";
    std::string row = "{\"kind\":\"ui\",\"op\":\"" + op + "\",\"" + key + "\":" + std::to_string(value);
    if (op == "manager-create") row += ",\"windows\":1";
    lifecycle_push(row + "}");
}

void StartupTextureOwner::note_manager_free() {
    const bool name = stage_ == ResourceStage::NameEntryManagerDead;
    if (!name) require_stage(ResourceStage::RestoreManagerDead, "Restore manager free");
    sync();
    // Observed: native's own manager unlink traversal (D+1).
    lifecycle_push(std::string("{\"kind\":\"mode\",\"op\":\"wrapper-free\",\"node\":\"manager\",\"mode\":") +
                   (name ? "9" : "10") + ",\"selector\":0}");
}

void StartupTextureOwner::free_restore_resource_node() {
    require_stage(ResourceStage::RestoreResourcesRetired, "Restore resource node free");
    sync();
    lifecycle_push(std::string("{\"kind\":\"mode\",\"op\":\"wrapper-free\",\"node\":\"resource\",\"mode\":10,\"selector\":0") + "}");
}

void StartupTextureOwner::free_restore_wrappers() {
    require_stage(ResourceStage::RestoreResourcesRetired, "Restore wrapper free");
    sync();
    lifecycle_push(std::string("{\"kind\":\"mode\",\"op\":\"wrapper-free\",\"node\":\"mode10\",\"mode\":10,\"selector\":0") + "}");
    advance(ResourceStage::RestoreWrappersFreed);
}

void StartupTextureOwner::note_effect_create(int slot, unsigned shape) {
    title_event(ResourceStage::TitleActive, ResourceStage::TitleActive,
                "{\"kind\":\"effect\",\"op\":\"create\",\"shape\":" + std::to_string(shape) +
                    ",\"slot\":" + std::to_string(slot) + "}");
    if (shape == 106) effect106_ = EffectObservation{slot, "active", {}};
}

void StartupTextureOwner::note_effect_bind(int slot, unsigned element, int handle) {
    // 0x10c188 reads the descriptor through 0x1cc2b0 only: the handle must be live, and
    // nothing (reference, allocation, record) changes.
    if (handles_.record_of(handle) < 0) throw std::logic_error("effect bind: handle is not live");
    title_event(ResourceStage::TitleActive, ResourceStage::TitleActive,
                "{\"kind\":\"effect\",\"op\":\"bind\",\"slot\":" + std::to_string(slot) + ",\"element\":" +
                    std::to_string(element) + ",\"handle\":" + std::to_string(handle) + "}");
    if (effect106_ && effect106_->slot == slot) {
        if (effect106_->element_handles.size() <= element) effect106_->element_handles.resize(element + 1, -1);
        effect106_->element_handles[element] = handle;
    }
}

void StartupTextureOwner::note_effect_removal_request(int slot) {
    title_event(ResourceStage::TitleActive, ResourceStage::TitleActive,
                "{\"kind\":\"effect\",\"op\":\"remove-request\",\"slot\":" + std::to_string(slot) + "}");
    if (effect106_ && effect106_->slot == slot && std::string(effect106_->state) == "active")
        effect106_->state = "removal-requested";
}

void StartupTextureOwner::note_effect_reclaim_armed(int slot) {
    require_modeled("effect reclaim arm");
    if (effect106_ && effect106_->slot == slot) effect106_->state = "reclaim-pending";
}

void StartupTextureOwner::request_title_successor(unsigned mode) {
    if (mode != 9 && mode != 10) throw std::logic_error("title successor other than New Game/Restore is not modeled");
    successor_mode_ = mode;
    title_event(ResourceStage::TitleActive, ResourceStage::TitleRemovalRequested,
                "{\"kind\":\"mode\",\"op\":\"request\",\"mode\":" + std::to_string(mode) + ",\"selector\":0,\"arg\":0}");
}

void StartupTextureOwner::retire_title_resources() {
    require_stage(ResourceStage::TitleRemovalRequested, "title resource destructor");
    note_mode_resource_retire(1);
    // 1b6f50 -> 1b6400 -> 1797b8 -> 179650: 1cca30(handles[i]) for every binding slot in order.
    for (int handle : model_binding_) handles_.retire(handle);  // 1cca30 of -7/-1 is a no-op
    model_binding_.clear();
    model_slot_bytes_.clear();
    if (title_model_) { models_.retire(*title_model_); title_model_.reset(); }
    // 1b5fd0 -> 1b2240: the image list in insertion order, then count = 0.
    for (int handle : image_list_) handles_.retire(handle);
    image_list_.clear();
    models_.reset_instances_and_registrations(); // 1010f4, after material and image lists
    advance(ResourceStage::TitleResourcesRetired);
}

void StartupTextureOwner::destroy_title_mode() {
    require_stage(ResourceStage::TitleResourcesRetired, "title mode-8 destructor");
    note_mode_destroy(1);
}

void StartupTextureOwner::free_title_resource_node() {
    require_stage(ResourceStage::TitleResourcesRetired, "title resource node free");
    note_mode_wrapper_free(1, 8, false);
}

void StartupTextureOwner::free_title_wrappers() {
    require_stage(ResourceStage::TitleResourcesRetired, "title wrapper free");
    note_mode_wrapper_free(1, 8, true);
    advance(ResourceStage::TitleWrappersFreed);
}

void StartupTextureOwner::enter_successor(unsigned mode) {
    require_stage(ResourceStage::TitleWrappersFreed, "successor entry");
    if (mode != successor_mode_) throw std::logic_error("successor differs from the requested mode");
    advance(ResourceStage::SuccessorEntry);
}

// ---- Newgame: New Game name entry and Chapter 1 (HANDOFF.md) ----
int StartupTextureOwner::construct_name_entry(const std::vector<std::uint8_t>& atlas) {
    require_stage(ResourceStage::SuccessorEntry, "name-entry construction");
    if (successor_mode_ != 9) throw std::logic_error("name-entry construction for a non-New Game successor");
    sync();
    const auto report = handles_.load_xim(OwnedRawXim(atlas));  // 18f7a8: authored PSM19/CPSM0, unnamed
    wrapper_atlas_ = report.handle;
    field_.atlas_global_24f730 = report.handle;
    sync();
    advance(ResourceStage::NameEntryActive);
    return wrapper_atlas_;
}

void StartupTextureOwner::name_manager_death() {
    require_stage(ResourceStage::NameEntryActive, "name manager death");
    advance(ResourceStage::NameEntryManagerDead);
}

void StartupTextureOwner::note_command(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const bool return_clear=stage_==ResourceStage::FieldConstructorEntry && field_.constructor_command==13 && !a&&!b&&!c;
    if (stage_ != ResourceStage::NameEntryManagerDead && stage_ != ResourceStage::ChapterActive &&
        stage_ != ResourceStage::RestoreActive && stage_ != ResourceStage::RestoreWrappersFreed &&
        stage_ != ResourceStage::FieldActive && stage_ != ResourceStage::BattleActive && !return_clear)
        throw std::logic_error("published command outside a modeled game manager");
    sync();
    lifecycle_push("{\"kind\":\"mode\",\"op\":\"command\",\"command\":[" + std::to_string(a) + "," + std::to_string(b) +
                   "," + std::to_string(c) + "]}");
}

void StartupTextureOwner::name_entry_requested(unsigned mode, unsigned selector, unsigned arg) {
    require_stage(ResourceStage::NameEntryManagerDead, "name-entry mode request");
    note_mode_request(mode, selector, arg);
    advance(ResourceStage::NameEntryCommandPublished);
}

void StartupTextureOwner::retire_name_entry_resources() {
    require_stage(ResourceStage::NameEntryCommandPublished, "name-entry resource retirement");
    note_mode_resource_retire(0, 9);
}

void StartupTextureOwner::release_name_atlas() {
    require_stage(ResourceStage::NameEntryCommandPublished, "name-entry atlas release");
    note_mode_destroy(0, 9);
    sync();
    handles_.retire(wrapper_atlas_);   // 124448 tail 1cca30(wrapper+0x30): image then CLUT freed
    wrapper_atlas_ = -1;
    advance(ResourceStage::NameEntryResourcesRetired);
}

void StartupTextureOwner::free_name_entry_resource_node() {
    require_stage(ResourceStage::NameEntryResourcesRetired, "name-entry resource node free");
    note_mode_wrapper_free(0, 9, false);
}

void StartupTextureOwner::free_name_entry_wrappers() {
    require_stage(ResourceStage::NameEntryResourcesRetired, "name-entry wrapper free");
    note_mode_wrapper_free(0, 9, true);
    advance(ResourceStage::NameEntryWrappersFreed);
}

int StartupTextureOwner::construct_chapter(unsigned chapter, const std::vector<std::uint8_t>& image) {
    if (stage_ != ResourceStage::NameEntryWrappersFreed && stage_ != ResourceStage::FieldWrappersFreed)
        throw std::logic_error("chapter construction outside a retired mode");
    if (!image_list_.empty()) throw std::logic_error("chapter construction with a live image list");
    sync();
    lifecycle_push("{\"kind\":\"phase\",\"name\":\"mode2-0x133830-entry-command14-chapter" + std::to_string(chapter) + "\"}");
    advance(ResourceStage::ChapterActive);
    return append_image(image);
}

void StartupTextureOwner::chapter_requested(unsigned mode, unsigned scene, unsigned entrance) {
    require_stage(ResourceStage::ChapterActive, "chapter mode request");
    note_mode_request(mode, scene, entrance);
    chapter_scene_ = scene;
    advance(ResourceStage::ChapterModeRequested);
}

void StartupTextureOwner::retire_chapter_resources() {
    require_stage(ResourceStage::ChapterModeRequested, "chapter resource retirement");
    note_mode_resource_retire(chapter_scene_, 2);
    for (int handle : image_list_) handles_.retire(handle);
    image_list_.clear();
    sync();
    advance(ResourceStage::ChapterResourcesRetired);
}

void StartupTextureOwner::destroy_chapter_mode() {
    require_stage(ResourceStage::ChapterResourcesRetired, "chapter 133a38");
    note_mode_destroy(chapter_scene_, 2);
}

void StartupTextureOwner::free_chapter_resource_node() {
    require_stage(ResourceStage::ChapterResourcesRetired, "chapter resource node free");
    note_mode_wrapper_free(chapter_scene_, 2, false);
}

void StartupTextureOwner::free_chapter_wrappers() {
    require_stage(ResourceStage::ChapterResourcesRetired, "chapter node free");
    note_mode_wrapper_free(chapter_scene_, 2, true);
    advance(ResourceStage::ChapterWrappersFreed);
}

void StartupTextureOwner::enter_field_constructor(unsigned scene, unsigned entrance, unsigned command) {
    if(command!=0 && command!=3 && command!=13)throw std::logic_error("field constructor command is unrecovered");
    if(command==13) {
        require_stage(ResourceStage::BattleWrappersFreed,"133830 command13 entry");
    } else if (stage_ == ResourceStage::ChapterWrappersFreed) {
        if (scene != chapter_scene_ || entrance != 0) throw std::logic_error("field constructor destination differs from the chapter request");
    } else if (stage_ != ResourceStage::RestoreWrappersFreed && stage_ != ResourceStage::FieldWrappersFreed)
        throw std::logic_error("133830 entry outside a retired mode");
    sync();
    lifecycle_push("{\"kind\":\"phase\",\"name\":\"mode2-0x133830-entry-command"+std::to_string(command)+"\"}");
    field_.constructor_command=command;
    advance(ResourceStage::FieldConstructorEntry);
}

// ---- Field: the ordinary-field common prefix (HANDOFF.md) ----
void StartupTextureOwner::construct_field_mode(unsigned scene, unsigned entrance) {
    require_stage(ResourceStage::FieldConstructorEntry, "133830 command-0 body");
    sync();
    // 1151b8(node, 221980, 0x50): the resource child (100ef8 sets its +0x48 texenv handle to -1).
    const auto command=field_.constructor_command;
    field_ = FieldPrefixState{.constructor_command=command,
        .atlas_global_24f730 = field_.atlas_global_24f730, .clut_class_2138c4 = field_.clut_class_2138c4};
    field_.resource_created = true;
    lifecycle_push("{\"kind\":\"field\",\"op\":\"resource-create\"}");
    field_.switch_337e70 = 1;  // 1bafd0(1)
    lifecycle_push("{\"kind\":\"field\",\"op\":\"switch-337e70\",\"value\":1}");
    // 1b7eb0(10), 135aa0 and 1315d0 touch no texture, RNG or ownership state (see the Field contract).
    field_.job_queued = true;
    field_.scene = scene;
    field_.entrance = 0;
    lifecycle_push("{\"kind\":\"field\",\"op\":\"job-enqueue\",\"job\":\"0x132688\",\"scene\":" + std::to_string(scene) +
                   ",\"arg\":0}");
    advance(ResourceStage::FieldConstructed);
}

void StartupTextureOwner::enter_field_worker() {
    require_stage(ResourceStage::FieldConstructed, "132688 entry");
    sync();
    field_.w30 = field_.w34 = 0;  // +0x38 (the parent-incremented counter) is scheduling state, not reported here
    lifecycle_push("{\"kind\":\"field\",\"op\":\"worker-entry\"}");
    advance(ResourceStage::FieldWorkerEntered);
}

int StartupTextureOwner::load_field_atlas(const OwnedBytes& atlas) {
    require_stage(ResourceStage::FieldWorkerEntered, "131cd0 atlas");
    if (!atlas) throw std::invalid_argument("field atlas bytes missing");
    sync();
    field_.clut_class_2138c4 = 0;  // 1cd7a8(0)
    lifecycle_push("{\"kind\":\"field\",\"op\":\"clut-class\",\"value\":0}");
    handles_.set_lower_case_file_names(true);  // 186a40(1)
    lifecycle_push("{\"kind\":\"field\",\"op\":\"lower-case\",\"value\":1}");
    const OwnedRawXim xim(*atlas);
    if (xim.layout().pixel_format != 27) throw std::invalid_argument("field atlas bytes lack the 18f500 PSM 27 override");
    const auto report = handles_.load_xim(xim);  // 18f7a8 on the patched buffer
    sync();
    field_atlas_bytes_ = atlas;
    field_.atlas_global_24f730 = report.handle;
    lifecycle_push("{\"kind\":\"field\",\"op\":\"atlas\",\"handle\":" + std::to_string(report.handle) + "}");
    advance(ResourceStage::FieldAtlasLoaded);
    return report.handle;
}

void StartupTextureOwner::bind_field_atlas() {
    require_stage(ResourceStage::FieldAtlasLoaded, "131d14 atlas ownership store");
    field_.manager_60 = field_.atlas_global_24f730;
}

int StartupTextureOwner::load_field_texenv(const OwnedBytes& texenv) {
    require_stage(ResourceStage::FieldAtlasLoaded, "100c60 texenv");
    if (!texenv) throw std::invalid_argument("texenv bytes missing");
    sync();
    const OwnedRawXim xim(*texenv);
    const auto report = handles_.load_xim(xim);
    sync();
    field_texenv_bytes_ = texenv;
    field_.resource_48 = report.handle;  // 100ca0
    lifecycle_push("{\"kind\":\"field\",\"op\":\"texenv\",\"handle\":" + std::to_string(report.handle) + "}");
    // 183ff0(handle, 0, 0, 0, t0 = image header +0xc, t1 = +0x8): the region, then TEX0 from the live descriptors
    // (1cc2b0 image, 1cd500 level 0). No reference is taken.
    const auto& w = xim.layout().image_header_words;
    const auto region = [](std::int32_t v) { return std::bit_cast<std::uint32_t>((float(v) + 0.5f) * (1.f / 1024.f)); };
    field_.region_332450 = {region(0), region(0), region(std::int32_t(w[3])), region(std::int32_t(w[2]))};
    std::uint64_t image = 0, clut = 0;
    if (report.handle >= 0) {
        if (const auto d = handles_.image(report.handle))
            if (const auto a = memory_.allocation(d->id)) image = a->descriptor;
        if (const auto d = handles_.palette(report.handle, 0))
            if (const auto a = memory_.allocation(d->id)) clut = a->descriptor;
    }
    field_.tex0_332430 = (image & 0x3fff) | (((image >> 14) & 0x3f) << 14) | (((image >> 32) & 0x3f) << 20) |
                         (std::uint64_t(10) << 26) | (std::uint64_t(10) << 30) | (std::uint64_t(1) << 34) |
                         ((clut & 0x3fff) << 37) | (((clut >> 32) & 0x3f) << 51) | (std::uint64_t(1) << 61);
    char tex0[24];
    std::snprintf(tex0, sizeof tex0, "0x%016llx", static_cast<unsigned long long>(field_.tex0_332430));
    std::string r;
    for (auto v : field_.region_332450) r += (r.empty() ? "" : ",") + std::to_string(v);
    lifecycle_push("{\"kind\":\"field\",\"op\":\"env-bind\",\"handle\":" + std::to_string(report.handle) + ",\"tex0\":\"" +
                   tex0 + "\",\"region\":[" + r + "]}");
    advance(ResourceStage::FieldTexenvLoaded);
    return report.handle;
}

void StartupTextureOwner::load_field_ruw(const OwnedBytes& ruw) {
    require_stage(ResourceStage::FieldTexenvLoaded, "1a9190 fld.ruw");
    if (!ruw) throw std::invalid_argument("fld.ruw bytes missing");
    sync();
    field_ruw_bytes_ = ruw;
    field_.ruw_slot0_live = true;
    field_.ruw_bytes = ruw->size();
    field_.ruw_sha256 = sha256_hex(*ruw);
    lifecycle_push("{\"kind\":\"field\",\"op\":\"ruw\",\"slot\":0,\"bytes\":" + std::to_string(ruw->size()) +
                   ",\"sha256\":\"" + field_.ruw_sha256 + "\"}");
    advance(ResourceStage::FieldRuwLoaded);
}

void StartupTextureOwner::publish_field_ui() {
    require_stage(ResourceStage::FieldRuwLoaded, "131e7c publication");
    sync();
    field_.w34 = 1;
    lifecycle_push("{\"kind\":\"field\",\"op\":\"ui-available\"}");
    advance(ResourceStage::FieldUiPublished);
}

void StartupTextureOwner::begin_field_world() {
    require_stage(ResourceStage::FieldUiPublished, "1bbac8 field map load");
    sync();
    handles_.enable_name_cache();
    lifecycle_push("{\"kind\":\"field\",\"op\":\"cache-enable\",\"value\":1}");
    advance(ResourceStage::FieldWorldLoading);
}

const StartupTextureOwner::WorldModelTextures& StartupTextureOwner::load_field_model(
    const std::string& name, std::span<const std::string> names,
    const TextureHandleOwner::FileReader& reader, const std::function<void()>& buffer_return, unsigned model_count) {
    if (stage_ != ResourceStage::FieldWorldLoading && stage_ != ResourceStage::FieldActive && stage_ != ResourceStage::BattleLoading)
        throw std::logic_error("186c48 field model outside construction/active event");
    auto textures = resources_.load_xmd(names, reader);
    sync();
    if (buffer_return) buffer_return();
    if(textures.xmd.aborted || (!names.empty()&&textures.xmd.first<0))
        throw std::runtime_error("186c48 texture loading failed before183450 model registration");
    const auto& result = models_.insert(name, model_count, std::move(textures));
    last_world_model_ = result.range;
    for (std::size_t i = 0; i < result.textures.slots.size(); ++i)
        lifecycle_push("{\"kind\":\"material\",\"model\":" + json_string(name) + ",\"slot\":" +
                       std::to_string(i) + ",\"handle\":" + std::to_string(result.textures.slots[i].handle) + "}");
    return result;
}

void StartupTextureOwner::load_field_sidecar(const OwnedBytes& bytes,
    const TextureHandleOwner::FileReader& reader, const std::function<void()>& image_return) {
    if (stage_ != ResourceStage::FieldWorldLoading && stage_ != ResourceStage::FieldActive && stage_ != ResourceStage::BattleLoading)
        throw std::logic_error("1b6848 field sidecar outside construction/active event");
    auto* model = last_world_model_ ? models_.find(*last_world_model_) : nullptr;
    if (!model) throw std::logic_error("Field XTA without a live model");
    resources_.load_xta(model->textures, bytes, reader, [&] {
        sync();
        if (image_return) image_return();
    });
    sync();
}

void StartupTextureOwner::complete_field_world() {
    require_stage(ResourceStage::FieldWorldLoading, "132688 completion");
    sync(); field_.w30 = 1;
    lifecycle_push("{\"kind\":\"field\",\"op\":\"world-complete\"}");
    advance(ResourceStage::FieldWorldComplete);
}
void StartupTextureOwner::commit_field_map() {
    require_stage(ResourceStage::FieldWorldComplete, "100c08 field map commit");
    sync(); field_.job_queued = false;
    lifecycle_push("{\"kind\":\"field\",\"op\":\"map-commit\"}");
    advance(ResourceStage::FieldMapCommitted);
}
void StartupTextureOwner::activate_field() {
    require_stage(ResourceStage::FieldMapCommitted, "132720 field activation return");
    sync();
    lifecycle_push("{\"kind\":\"field\",\"op\":\"activated\"}");
    advance(ResourceStage::FieldActive);
}

void StartupTextureOwner::retire_field_resources() {
    require_stage(ResourceStage::FieldActive, "101058 field retirement");
    note_mode_resource_retire(field_.scene, 2);
    handles_.retire(field_.resource_48); // 1010bc, before map/model cleanup
    models_.retire_associated();
    models_.retire_main();
    for (int handle : image_list_) handles_.retire(handle);
    image_list_.clear();
    models_.reset_instances_and_registrations(); // 1010f4, after all resource frees
    last_world_model_.reset();
    sync();
    advance(ResourceStage::FieldResourcesRetired);
}
void StartupTextureOwner::retire_field_manager() {
    require_stage(ResourceStage::FieldResourcesRetired, "133a38 field manager destructor");
    note_mode_destroy(field_.scene, 2);
    field_ruw_bytes_.reset();
    field_.ruw_slot0_live = false;
    handles_.retire(field_.manager_60); // 133a64 -> 11fbc8 -> 1cca30
    field_atlas_bytes_.reset(); field_texenv_bytes_.reset();
    sync();
}
void StartupTextureOwner::free_field_wrappers() {
    require_stage(ResourceStage::FieldResourcesRetired, "field wrapper free");
    note_mode_wrapper_free(field_.scene, 2, true);
    advance(ResourceStage::FieldWrappersFreed);
}
void StartupTextureOwner::free_field_resource_node() {
    require_stage(ResourceStage::FieldResourcesRetired, "field resource node free");
    note_mode_wrapper_free(field_.scene, 2, false);
}

void StartupTextureOwner::begin_battle(unsigned formation, unsigned stage, const OwnedBytes& ruw,
                                      const OwnedBytes& atlas, const OwnedBytes& environment) {
    require_stage(ResourceStage::FieldWrappersFreed, "14a118 battle loader");
    if(!ruw||!atlas||!environment)throw std::invalid_argument("Battle loader input bytes missing");
    if(stage<1||stage>23)throw std::invalid_argument("Battle stage was not clamped by149e60");
    battle_formation_=formation; battle_stage_=stage;
    battle_ruw_bytes_=ruw;
    lifecycle_push("{\"kind\":\"battle\",\"op\":\"ruw\",\"slot\":0,\"sha256\":"+json_string(sha256_hex(*ruw))+"}");
    const OwnedRawXim atlas_xim(*atlas);
    if(atlas_xim.layout().pixel_format!=27)throw std::invalid_argument("Battle atlas lacks PSM27 override");
    battle_atlas_=handles_.load_xim(atlas_xim).handle;
    battle_atlas_bytes_=atlas;
    lifecycle_push("{\"kind\":\"battle\",\"op\":\"atlas\",\"handle\":"+std::to_string(battle_atlas_)+"}");
    battle_environment_=handles_.load_xim(OwnedRawXim(*environment)).handle;
    battle_environment_bytes_=environment;
    lifecycle_push("{\"kind\":\"battle\",\"op\":\"texenv\",\"handle\":"+std::to_string(battle_environment_)+"}");
    //152248 stage construction, after root atlas and resource texenv.
    field_.clut_class_2138c4=0;
    handles_.set_lower_case_file_names(true);
    handles_.enable_name_cache();
    last_world_model_.reset();
    sync();advance(ResourceStage::BattleLoading);
}
void StartupTextureOwner::complete_battle() {
    require_stage(ResourceStage::BattleLoading,"14a118 completion");
    sync();advance(ResourceStage::BattleActive);
}
void StartupTextureOwner::retire_battle_actor(unsigned instance) {
    require_stage(ResourceStage::BattleActive,"154118/15c1b0 actor destructor");
    models_.retire_instance(instance);sync();
}
void StartupTextureOwner::retire_battle_root() {
    require_stage(ResourceStage::BattleActive,"14aac8 root destructor");
    note_mode_destroy(battle_formation_,3);
    handles_.retire(battle_atlas_);battle_atlas_bytes_.reset();
    battle_ruw_bytes_.reset();
    models_.retire_associated();
    sync();advance(ResourceStage::BattleRootRetired);
}
void StartupTextureOwner::retire_battle_resources() {
    require_stage(ResourceStage::BattleRootRetired,"101058 battle resource destructor");
    note_mode_resource_retire(battle_formation_,3);
    handles_.retire(battle_environment_);
    battle_environment_bytes_.reset();
    models_.retire_associated();models_.retire_main();
    for(int handle:image_list_)handles_.retire(handle);
    image_list_.clear();models_.reset_instances_and_registrations();last_world_model_.reset();
    sync();advance(ResourceStage::BattleResourcesRetired);
}
void StartupTextureOwner::free_battle_resource_node() {
    require_stage(ResourceStage::BattleResourcesRetired,"battle resource node free");
    note_mode_wrapper_free(battle_formation_,3,false);
}
void StartupTextureOwner::free_battle_wrappers() {
    require_stage(ResourceStage::BattleResourcesRetired,"battle root wrapper free");
    note_mode_wrapper_free(battle_formation_,3,true);advance(ResourceStage::BattleWrappersFreed);
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

namespace {
void write_report(const StartupTextureOwner& owner, const std::filesystem::path& dir, bool session_schema,
                  const std::string& checkpoint = {}, std::uint32_t session_rng_state = 0,
                  CheckpointSchema schema = CheckpointSchema::Retire);
}

void write_startup_texture_report(const StartupTextureOwner& owner, const std::filesystem::path& dir) {
    write_report(owner, dir, false);
}

void write_session_resource_report(const StartupTextureOwner& owner, const std::filesystem::path& dir) {
    write_report(owner, dir, true);
}

void write_title_checkpoint_report(const StartupTextureOwner& owner, const std::filesystem::path& dir,
                                   const std::string& checkpoint, std::uint32_t session_rng_state, CheckpointSchema schema) {
    if (checkpoint.empty()) throw std::logic_error("title checkpoint report needs a checkpoint name");
    write_report(owner, dir, true, checkpoint, session_rng_state, schema);
}

namespace {
void write_report(const StartupTextureOwner& owner, const std::filesystem::path& dir, bool session_schema,
                  const std::string& checkpoint, std::uint32_t session_rng_state, CheckpointSchema schema) {
    namespace fs = std::filesystem;
    if (fs::exists(dir)) throw std::runtime_error("startup texture report directory already exists");
    fs::create_directories(dir);
    const auto& m = owner.memory();
    const auto& h = owner.handles();
    std::ostringstream o;
    auto desc = [](const OptDescriptor& d) {
        return d ? std::string("[") + std::to_string(d->id) + "," + std::to_string(d->serial) + "]" : std::string("null");
    };
    if (session_schema && !checkpoint.empty()) {
        // Retire: a title checkpoint between the first 1296f8 and entry of 124548.
        const char* label = schema == CheckpointSchema::FieldWorld ? "field-world" : schema == CheckpointSchema::Field ? "field-prefix" : schema == CheckpointSchema::Newgame ? "newgame"
                          : schema == CheckpointSchema::Restore ? "restore" : "retire-title";
        const bool second_return = owner.second_title() && owner.stage() == ResourceStage::TitleConstructed;
        if ((!second_return && (owner.stage() < ResourceStage::TitleActive || owner.stage() == ResourceStage::Unmodeled)) ||
            owner.memory_coverage() != MemoryContentCoverage::DrawTargetsInvalidated)
            throw std::logic_error("title checkpoint report outside the modeled title/Restore stages");
        if (owner.rng_log().empty() || owner.rng_log().back().state != session_rng_state)
            throw std::runtime_error("title checkpoint: Session RNG drew after 0x100618; draws are not logged");
        o << "{\n \"coverage\":{\"lifecycle\":\"" << label << "\",\"checkpoint\":\"" << checkpoint
          << "\",\"memory\":\"host-uploads-known-draw-writes-unknown\"},\n \"later_history\":\"stage "
          << to_string(owner.stage())
          << (schema == CheckpointSchema::FieldWorld
                  ? "; field world uploads accounted; later scene retirement, battle and shared-GS rendering remain unproven\","
                  : schema == CheckpointSchema::Field
                  ? "; unmodeled after the 131e7c store (manager+0x34 = 1): 131e80 onward (field definition, PME, music, map/XMD/XTA, actors), the 132688 +0x30 completion, battle\","
                  : schema == CheckpointSchema::Newgame
                  ? "; unmodeled after entry of the second 133830 (command 0): ordinary field loading, XTA, battle\","
                  : schema == CheckpointSchema::Restore
                  ? "; unmodeled after the second 12c350(1) return: its first 1296f8 update, saved-slot Restore, field, battle, XTA\","
                  : "; unmodeled after entry of 124548: the successor constructor body, Restore/name entry, field, battle, XTA\",");
    } else if (session_schema) {
        // the title original-reference contract fixed labels for the title-constructor boundary.
        if (owner.stage() != ResourceStage::TitleConstructed ||
            owner.memory_coverage() != MemoryContentCoverage::DrawTargetsInvalidated)
            throw std::logic_error("session resource report is defined only at the title-constructor return");
        o << "{\n \"coverage\":{\"lifecycle\":\"title-constructor-return\",\"memory\":\"host-uploads-known-draw-writes-unknown\"},\n \"later_history\":\"unmodeled after "
          << to_string(owner.stage()) << ": title updates/retirement, New Game/Restore, XTA, field, battle\",";
    } else {
        o << "{\n \"coverage\":\"" << (owner.coverage() == StartupCoverage::PreSchedulerOnly ? "pre-scheduler-only" : "not-run")
          << "\",\n \"later_history\":\"unmodeled: title, models, field, battle, XTA, frees, filter display-list kicks\",";
    }
    if (session_schema) {
        o << "\n \"image_list\":[";
        for (std::size_t i = 0; i < owner.image_list().size(); ++i) o << (i ? "," : "") << owner.image_list()[i];
        o << "],\n \"materials\":[";
        for (std::size_t i = 0; i < owner.model_binding().size(); ++i)
            o << (i ? "," : "") << "[" << i << "," << owner.model_binding()[i] << "]";
        o << "],";
    }
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
    for (int id : (schema == CheckpointSchema::FieldWorld ? h.reserved_handles(0) : h.reserved_handles())) { o << (first ? "" : ",") << id; first = false; }
    o << "]";
    if (schema == CheckpointSchema::FieldWorld) {
        o << ",\n \"reserved1\":[";
        first = true;
        for (int id : h.reserved_handles(1)) { o << (first ? "" : ",") << id; first = false; }
        o << "],\n \"reserve_class\":" << h.reservation_class() ;
    }
    o << ",\n \"free_records\":" << h.free_record_count() << ",\"free_extras\":" << h.free_extra_count();
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
        const bool restore_rows = session_schema && !checkpoint.empty() && schema != CheckpointSchema::Retire;
        const bool newgame_rows = restore_rows && (schema == CheckpointSchema::Newgame || schema == CheckpointSchema::Field || schema == CheckpointSchema::FieldWorld);
        const bool field_rows = restore_rows && (schema == CheckpointSchema::Field || schema == CheckpointSchema::FieldWorld);
        // Rows outside this report's vocabulary: Restore kinds unless Restore/Newgame, published commands unless Newgame.
        const auto excluded = [&](const std::string& line) {
            const bool restore_kind = line.rfind("{\"kind\":\"upload-bytes\"", 0) == 0 || line.rfind("{\"kind\":\"ui\"", 0) == 0 ||
                                  line.rfind("{\"kind\":\"glyph\"", 0) == 0;
            const bool newgame_kind = line.rfind("{\"kind\":\"mode\",\"op\":\"command\"", 0) == 0;
            const bool field_kind = line.rfind("{\"kind\":\"field\"", 0) == 0;
            return (restore_kind && !restore_rows) || (newgame_kind && !newgame_rows) || (field_kind && !field_rows);
        };
        const auto plain = [&](const std::string& line) {  // earlier schemas have no provenance key
            if (restore_rows) return line;
            const std::string key = ",\"asserted\":true}";
            return line.size() >= key.size() && line.compare(line.size() - key.size(), key.size(), key) == 0
                       ? line.substr(0, line.size() - key.size()) + "}" : line;
        };
        std::ofstream lc(dir / "lifecycle.jsonl", std::ios::binary);
        for (const auto& line : owner.lifecycle())
            if (!excluded(line)) lc << plain(line) << "\n";
        if (session_schema) {
            std::ofstream tl(dir / "timeline.jsonl", std::ios::binary);
            for (const auto& line : owner.lifecycle())
                if ((!checkpoint.empty() && !excluded(line)) ||
                    (checkpoint.empty() && line.rfind("{\"kind\":\"mode\"", 0) != 0 &&
                     line.rfind("{\"kind\":\"effect\"", 0) != 0 && !excluded(line)))
                    tl << plain(line) << "\n";
        }
        std::ofstream tl(dir / (session_schema ? "startup-timeline.jsonl" : "timeline.jsonl"), std::ios::binary);
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
    if (!checkpoint.empty()) {
        std::vector<TextureAllocationId> targets;
        for (std::size_t i = 0; i < 2; ++i) targets.push_back(owner.layers()[i].color.id);
        targets.push_back(owner.layers()[0].depth->id);
        std::vector<std::uint8_t> invalidated(Ps2TextureMemory::vram_bytes);
        for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a)
            for (std::uint8_t n = 0; n < 2; ++n) {
                if (known[a] & (1u << n)) continue;
                const auto o = m.owner_of_nibble(a, n);
                if (o && std::find(targets.begin(), targets.end(), *o) != targets.end()) invalidated[a] |= 1u << n;
            }
        std::ofstream(dir / "vram.invalidated", std::ios::binary)
            .write(reinterpret_cast<const char*>(invalidated.data()), invalidated.size());
        std::ostringstream e;
        e << "{\"effect106\":";
        if (const auto& w = owner.effect106()) {
            e << "{\"slot\":" << w->slot << ",\"shape\":" << (w->shape_known ? "106" : "null") << ",\"state\":\""
              << w->state << "\",\"element_handles\":[";
            for (std::size_t i = 0; i < w->element_handles.size(); ++i) e << (i ? "," : "") << w->element_handles[i];
            e << "]}";
        } else e << "null";
        e << "}\n";
        std::ofstream(dir / "effects.json", std::ios::binary) << e.str();
        if (schema == CheckpointSchema::FieldWorld) {
            std::ostringstream world;
            world << "{\"models\":[";
            bool first_model = true;
            std::vector<const ModelResourceGroup*> active;
            for (const auto& model : owner.world_models())
                if (model->reported && !model->retired) active.push_back(model.get());
            std::sort(active.begin(), active.end(), [](const auto* a, const auto* b) { return a->range.first < b->range.first; });
            for (const auto* model : active) {
                world << (first_model ? "" : ",") << "{\"name\":" << json_string(model->name) << ",\"handles\":[";
                for (std::size_t i = 0; i < model->textures.slots.size(); ++i)
                    world << (i ? "," : "") << model->textures.slots[i].handle;
                world << "]}"; first_model = false;
            }
            world << "],\"xta_scene\":[";
            const auto& images = owner.resources().scene_images();
            for (std::size_t i = 0; i < images.size(); ++i) world << (i ? "," : "") << images[i].handle;
            world << "]}\n";
            std::ofstream(dir / "world.json", std::ios::binary) << world.str();
            std::ostringstream groups;
            groups << "{\"available\":" << owner.models().ranges().available_count() << ",\"free_ranges\":[";
            bool comma = false;
            for (const auto& [first, count] : owner.models().ranges().free_ranges()) {
                groups << (comma ? "," : "") << "[" << first << "," << count << "]"; comma = true;
            }
            groups << "],\"groups\":["; comma = false;
            for (const auto& group : owner.models().groups()) if (!group->retired) {
                groups << (comma ? "," : "") << "{\"name\":" << json_string(group->name)
                       << ",\"first\":" << group->range.first << ",\"count\":" << group->range.count
                       << ",\"serial\":" << group->range.serial << "}"; comma = true;
            }
            groups << "],\"instances\":["; comma = false;
            const auto& instances = owner.models().instances();
            for (unsigned index = 0; index < instances.size(); ++index) if (instances[index]) {
                const auto& instance = *instances[index];
                groups << (comma ? "," : "") << "{\"instance\":" << index << ",\"main\":" << instance.main.first
                       << ",\"shadow\":" << (instance.shadow ? std::to_string(instance.shadow->first) : "null") << "}"; comma = true;
            }
            groups << "],\"actor_registrations\":["; comma = false;
            for (const auto& [name, registration] : owner.models().registrations()) {
                groups << (comma ? "," : "") << "{\"name\":" << json_string(name) << ",\"main\":" << registration.main.first
                       << ",\"shadow\":" << (registration.shadow ? std::to_string(registration.shadow->first) : "null") << "}"; comma = true;
            }
            groups << "]}\n";
            std::ofstream(dir / "model-groups.json", std::ios::binary) << groups.str();
        }
        if (schema == CheckpointSchema::Field || schema == CheckpointSchema::FieldWorld) {
            // Field field.json: the prefix's source-visible words (HANDOFF.md).
            const auto& f = owner.field_prefix();
            char tex0[24];
            std::snprintf(tex0, sizeof tex0, "0x%016llx", static_cast<unsigned long long>(f.tex0_332430));
            std::ostringstream fj;
            fj << "{\"resource_created\":" << (f.resource_created ? "true" : "false") << ",\"switch_337e70\":" << f.switch_337e70
               << ",\"job_queued\":" << (f.job_queued ? "true" : "false") << ",\"scene\":" << f.scene << ",\"entrance\":" << f.entrance
               << ",\"w30\":" << f.w30 << ",\"w34\":" << f.w34 << ",\"manager_60\":" << f.manager_60
               << ",\"resource_48\":" << f.resource_48 << ",\"atlas_global_24f730\":" << f.atlas_global_24f730
               << ",\"clut_class_2138c4\":" << f.clut_class_2138c4
               << ",\"reserve_class_3abb14\":" << h.reservation_class()
               << ",\"lower_1fe30c\":" << (h.lower_case_file_names() ? 1 : 0) << ",\"cache_1fe310\":" << (h.name_cache_enabled() ? 1 : 0)
               << ",\"tex0_332430\":\"" << tex0 << "\",\"region_332450\":[";
            for (std::size_t i = 0; i < 4; ++i) fj << (i ? "," : "") << f.region_332450[i];
            fj << "],\"ruw_slot0_live\":" << (f.ruw_slot0_live ? "true" : "false") << ",\"ruw_bytes\":" << f.ruw_bytes
               << ",\"ruw_sha256\":" << (f.ruw_sha256.empty() ? std::string("null") : "\"" + f.ruw_sha256 + "\"") << "}\n";
            std::ofstream(dir / "field.json", std::ios::binary) << fj.str();
        }
    }
}
} // namespace

} // namespace okage
