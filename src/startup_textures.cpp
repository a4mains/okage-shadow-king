#include "okage/startup_textures.hpp"

#include <cstdio>
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

// 0x1f14b0: next = next * 0x41c64e6d + 0x3039 (u32); result = next & 0x7fffffff.
struct LibcRand {
    std::uint32_t state;
    std::uint32_t operator()() {
        state = state * 0x41c64e6du + 0x3039u;
        return state & 0x7fffffffu;
    }
};

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

void StartupTextureOwner::checkpoint(const char* step) {
    std::uint32_t live = 0;
    for (TextureAllocationId id = 0; id < 4096; ++id) live += memory_.allocation(id).has_value();
    checkpoints_.push_back({step, live, static_cast<std::uint32_t>(handles_.live_handle_count()),
                            memory_.available_count(), memory_.identity()});
}

TextureDescriptor StartupTextureOwner::allocate_layer_buffer(std::uint8_t cls, GsPixelStorage psm) {
    // 0x1cf008(class, psm, 640, 224); a failure here is a hard error: the
    // retail startup assumes success and no failure path is modeled (U11).
    const auto a = memory_.allocate(cls, psm, 640, 224);
    if (!a) throw std::runtime_error("startup: colour/depth allocation failed");
    return {a->id, a->serial};
}

void StartupTextureOwner::run_startup(const StartupTextureInputs& in) {
    if (started_) throw std::logic_error("startup texture lifetime already initialized (re-initialization rejected)");
    started_ = true;
    if (in.font_palette.size() != 64) throw std::invalid_argument("startup: font palette must be 64 bytes");
    if (!in.read_effect) throw std::invalid_argument("startup: effect reader missing");

    auto require = [](bool ok, const char* what) { if (!ok) throw std::runtime_error(what); };
    bool uploaded = false;
    auto retain = [&](const TextureDescriptor& d) { require(memory_.retain(d.id), "startup: retain failed"); };

    // Step 0 (1003d8 -> 1cb088): 1ce940 allocator init, 1cbdf8(0x200,0x80) handle tables (owner ctor), palette mode 0.
    memory_.initialize_empty_allocator();
    checkpoint("allocator-init");

    // Step 1 (100428 -> 1cb1e8): two 640x224 colour layers (class 2, PSM0), then shared PSM49 depth (class 3).
    // Each layer holds one reference to its colour and one to the shared depth.
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
    glyph_cache_handle_ = handles_.create_image(GsPixelStorage::Psmt4, 1024, 256, {});
    require(glyph_cache_handle_ >= 0, "startup: glyph cache creation failed");
    {
        const auto pal = handles_.create_palette(16, in.font_palette, uploaded);
        require(pal && uploaded, "startup: glyph palette allocation failed");
        handles_.bind_palette(glyph_cache_handle_, 0, pal);
    }
    checkpoint("glyph-cache");

    {
        LibcRand rnd{in.rand_state};
        std::array<std::uint32_t, 256> words{};
        for (auto& w : words) {  // 192f10-192f48
            std::uint32_t x = 0x80000000u;
            x |= (rnd() << 3) & 0xff00u;
            x |= (rnd() >> 5) & 0xffu;
            w = x;
        }
        const auto clut_a = words_le(words);
        for (auto& w : words) {  // 192f68-192f94
            const std::uint32_t s = (rnd() >> 5) & 0xffu;
            w |= (s << 16) | (s << 8) | s;
        }
        const auto clut_b = words_le(words);
        std::vector<std::uint8_t> pixels(0x40 * 0x80);
        for (auto& p : pixels) p = static_cast<std::uint8_t>((rnd() >> 5) & 0xffu);  // 192fc0-193008
        rand_state_after_noise_ = rnd.state;
        // Source order: CLUT A allocated+uploaded (192f54), CLUT B (192fa0), then the image (193018).
        bool up_a = false, up_b = false;
        const auto pal_a = handles_.create_palette(256, clut_a, up_a);
        const auto pal_b = handles_.create_palette(256, clut_b, up_b);
        require(pal_a && pal_b && up_a && up_b, "startup: noise palette allocation failed");
        noise_handle_ = handles_.create_image(GsPixelStorage::Psmt8, 0x40, 0x80, pixels);
        require(noise_handle_ >= 0, "startup: noise texture creation failed");
        handles_.bind_palette(noise_handle_, 0, pal_a);
        handles_.bind_palette(noise_handle_, 1, pal_b);
    }
    //  18f8c0: capture target (class 2 PSM0 640x224) sharing the depth buffer.
    layers_[2].color = allocate_layer_buffer(2, GsPixelStorage::Psmct32);
    retain(layers_[2].color);
    retain(*layers_[0].depth);
    layers_[2].depth = layers_[0].depth;
    //  190f38 and 191030: 256-entry CLUTs with NULL source: allocated, never uploaded, never retained.
    for (auto& clut : filter_cluts_) {
        bool up = false;
        const auto pal = handles_.create_palette(256, {}, up);
        require(pal && !up, "startup: filter CLUT allocation failed");
        clut = *pal;
    }
    checkpoint("capture-filter");

    // Step 3 (100514 -> 1011d0 -> 103718): 31 shared effects through 18f7a8, authored order.
    for (const char* name : effect_names) {
        const auto bytes = in.read_effect(name);
        require(bytes.has_value(), "startup: effect file missing");
        const auto report = handles_.load_xim(OwnedRawXim(*bytes));
        require(report.handle >= 0, "startup: effect load failed");
        effect_handles_.push_back(report.handle);
    }
    checkpoint("effects");

    // Step 4 (100524 -> 1001a8 -> 1947a0): three fonts; PSM selected by flags (0x1000 -> 44, 0x2000 -> 36, 0 -> 20).
    static constexpr std::array<GsPixelStorage, 3> font_psm{GsPixelStorage::Psmt4Hh, GsPixelStorage::Psmt4Hl,
                                                            GsPixelStorage::Psmt4};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& f = in.fonts[i];
        require(f.size() > 8, "startup: font file too short");
        const unsigned count = unsigned(f[5]) - unsigned(f[4]) + 1u;
        const std::size_t header = 8 + 0x14u * count;
        const std::uint16_t height = le16(f, header), width = le16(f, header + 2);
        const std::size_t bytes = std::size_t(width) * height / 2;
        require(header + 4 + bytes <= f.size(), "startup: font atlas outside file");
        const std::span<const std::uint8_t> payload(f.data() + header + 4, bytes);  // verbatim file tail
        font_handles_[i] = handles_.create_image(font_psm[i], width, height, payload);
        require(font_handles_[i] >= 0, "startup: font image creation failed");
        const auto pal = handles_.create_palette(16, in.font_palette, uploaded);
        require(pal && uploaded, "startup: font palette allocation failed");
        handles_.bind_palette(font_handles_[i], 0, pal);
    }
    checkpoint("fonts");

    // The frozen contract totals; any deviation means the inputs are not the verified retail ones.
    std::uint32_t live = 0;
    for (TextureAllocationId id = 0; id < 4096; ++id) live += memory_.allocation(id).has_value();
    require(live == expected_allocations && std::uint32_t(handles_.live_handle_count()) == expected_handles,
            "startup: allocation/handle totals differ from the frozen contract");
    coverage_ = StartupCoverage::PreSchedulerOnly;
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
