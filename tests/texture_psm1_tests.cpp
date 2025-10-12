#include "okage/texture_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "okage/gs_texture_address.hpp"

using namespace okage;

namespace {
void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
template <class F> void rejects(const char* label, F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("expected rejection: ") + label);
}
template <class F> void scenario(F&& f) { f(); }

std::vector<std::uint8_t> ramp(std::size_t bytes, unsigned seed = 3) {
    std::vector<std::uint8_t> v(bytes);
    for (std::size_t k = 0; k < bytes; ++k) v[k] = static_cast<std::uint8_t>(k * 167u + seed * 29u + 5u);
    return v;
}
// PSMCT32 byte address of (x, 0), TBP 0, TBW 1, x < 32.
std::uint32_t ct32_row0(unsigned x) {
    return (x & 1 ? 4u : 0u) + (x & 2 ? 0x10u : 0u) + (x & 4 ? 0x20u : 0u) + (x & 8 ? 0x100u : 0u) + (x & 16 ? 0x400u : 0u);
}
std::size_t known_bytes(const Ps2TextureMemory& m) { return m.known_byte_addresses().size(); }
bool value_is(const Ps2TextureMemory& m, std::uint32_t a, unsigned v) {
    const auto o = m.observe_byte(a);
    return o.value && *o.value == v;
}
bool unknown(const Ps2TextureMemory& m, std::uint32_t a) { return m.observe_byte_masked(a).known_mask == 0; }
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int k = 0; k < 4; ++k) v[at + k] = static_cast<std::uint8_t>(x >> (8 * k));
}
std::vector<std::uint8_t> build_xim(std::uint32_t psm, std::uint32_t cpsm, std::uint32_t w, std::uint32_t h) {
    const bool indexed = psm >= 16;
    const std::size_t pal = indexed ? 16 + 16 * 4 : 0;  // one 16-colour CPSM0 level
    const std::size_t bytes = psm == 1 ? std::size_t(w) * h * 3 : psm == 0 ? std::size_t(w) * h * 4 : std::size_t(w) * h;
    std::vector<std::uint8_t> v(16 + pal + 16 + bytes, 0);
    put32(v, 0, psm << 20);
    put32(v, 4, cpsm << 19);
    if (indexed) {
        put32(v, 0x10, static_cast<std::uint32_t>(pal));
        put32(v, 0x18, 1);
        put32(v, 0x1c, 16);
    }
    put32(v, 16 + pal + 8, h);
    put32(v, 16 + pal + 12, w);
    for (std::size_t i = 0; i < bytes; ++i) v[16 + pal + 16 + i] = static_cast<std::uint8_t>(7 + i * 13);
    return v;
}
}  // namespace

int main() {
    try {
        require(retail_transfer_qwc(1, 16, 16) == 48 && retail_transfer_qwc(1, 33, 17) == 105 &&
                retail_transfer_qwc(1, 3, 3) == 1 && retail_transfer_qwc(1, 5, 7) == 6 &&
                retail_transfer_qwc(1, 1, 5) == 0 && retail_transfer_qwc(1, 128, 256) == 6144,
                "PSM1 floor(w*h*24/128)");

        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(96);
            const auto r = transfer_host_to_local(m, {1, 0, 1, 0, 0, 32, 1}, d);
            require(r.pixels_written == 32 && r.bytes_written == 96 && r.partial_pixel_bytes == 0, "result");
            for (unsigned x = 0; x < 32; ++x) {
                const auto a = ct32_row0(x);
                for (unsigned k = 0; k < 3; ++k) require(value_is(m, a + k, d[3 * x + k]), "pixel byte");
                require(unknown(m, a + 3), "high byte stays unknown");
            }
            require(known_bytes(m) == 96, "exactly 3 bytes per pixel known");
            // Identical to the PSM0 words except the dropped alpha byte.
            Ps2TextureMemory n;
            std::vector<std::uint8_t> d32(128);
            for (unsigned x = 0; x < 32; ++x) { for (unsigned k = 0; k < 3; ++k) d32[4 * x + k] = d[3 * x + k]; d32[4 * x + 3] = 0xAA; }
            transfer_host_to_local(n, {0, 0, 1, 0, 0, 32, 1}, d32);
            for (unsigned x = 0; x < 32; ++x)
                for (unsigned k = 0; k < 3; ++k)
                    require(n.observe_byte(ct32_row0(x) + k).value == m.observe_byte(ct32_row0(x) + k).value, "same as PSM0");
        });

        // High byte preservation: value, definedness (known, unknown, half-known) and allocation owner.
        scenario([&] {
            Ps2TextureMemory m;
            // Words 0..3 of row 0 are at 0, 4, 0x10, 0x14. Prepare the high bytes of pixels 0..3.
            m.write_bytes(3, std::array<std::uint8_t, 1>{0xA5});           // pixel 0: known high byte
            // pixel 1 (word 4): high byte unknown
            m.write_nibble(0x10 + 3, 0, 0x7);
            m.write_nibble(0x14 + 3, 1, 0xC);
            m.write_bytes(0, std::array<std::uint8_t, 3>{0x11, 0x22, 0x33});  // pre-existing low bytes replaced
            const auto before = m.observe_byte_masked(3);
            std::vector<std::uint8_t> d(96);
            for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(0xE0 + (i & 7));
            transfer_host_to_local(m, {1, 0, 1, 0, 0, 32, 1}, d);
            require(m.observe_byte_masked(3).known_mask == 3 && m.observe_byte_masked(3).value == 0xA5 &&
                    before.known_mask == 3, "known high byte unchanged");
            require(unknown(m, 4 + 3), "unknown high byte stays unknown");
            require(m.observe_byte_masked(0x10 + 3).known_mask == 1 && m.observe_byte_masked(0x10 + 3).value == 0x07,
                    "low-nibble lane keeps its nibble and its unknown partner");
            require(m.observe_byte_masked(0x14 + 3).known_mask == 2 && m.observe_byte_masked(0x14 + 3).value == 0xC0,
                    "high-nibble lane keeps its nibble and its unknown partner");
            require(value_is(m, 0, d[0]) && value_is(m, 1, d[1]) && value_is(m, 2, d[2]), "low 24 bits replaced");
        });

        for (const auto high_psm : {GsPixelStorage::Psmt4Hl, GsPixelStorage::Psmt4Hh}) {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto hi = m.allocate(1, high_psm, 128, 128);
            const auto ct = m.allocate(1, GsPixelStorage::Psmct24, 128, 128);
            require(hi && ct, "allocations");
            require(hi->base_block == 0 && ct->base_block == 0 && hi->buffer_width_64 == 2 && ct->buffer_width_64 == 2,
                    "retail placement shares the blocks (original case 178)");
            const auto hdata = ramp(128 * 128 / 2, 5);
            upload_allocation_image(m, hi->id, hdata);
            const auto snap_before = m.snapshot();
            const auto cdata = ramp(128 * 128 * 3, 9);
            upload_allocation_image(m, ct->id, cdata);
            const bool hh = high_psm == GsPixelStorage::Psmt4Hh;
            for (unsigned y : {0u, 1u, 17u, 127u})
                for (unsigned x : {0u, 1u, 9u, 64u, 127u}) {
                    const auto a = *gs_texture_byte_address(0, 0, 2, x, y);
                    const auto before = snap_before.observe_byte_masked(a + 3);
                    const auto after = m.observe_byte_masked(a + 3);
                    require(before.known_mask == (hh ? 2 : 1) && after.known_mask == before.known_mask &&
                            after.value == before.value, "4-bit lane value and definedness preserved");
                    require(m.owner_of_nibble(a + 3, hh ? 1 : 0) == hi->id, "4-bit lane keeps its owner");
                    require(m.owner_of_nibble(a, 0) == ct->id && m.owner_of_nibble(a + 2, 1) == ct->id, "low 24 bits owned by PSM1");
                    require(value_is(m, a, cdata[3 * (y * 128 + x)]) && value_is(m, a + 2, cdata[3 * (y * 128 + x) + 2]),
                            "PSM1 pixel");
                    require(m.owner_of_nibble(a + 3, hh ? 0 : 1) == std::nullopt, "other nibble unowned");
                }
            require(m.snapshot().identity().ownership_epoch == snap_before.identity().ownership_epoch,
                    "transfer does not change ownership");
        }

        // Floor-QWC dropped pixels and partial-pixel tails through the retail wrapper.
        scenario([&] {
            Ps2TextureMemory m;
            const auto src = ramp(33 * 17 * 3);
            const auto r = upload_retail_rect(m, {1, 0, 2, 0, 0, 33, 17}, src);
            require(r.pixels_written == 560 && r.bytes_written == 1680 && r.partial_pixel_bytes == 0, "33x17 result");
            require(known_bytes(m) == 560 * 3, "dropped tail pixel unwritten");
            const auto last_sent = *gs_texture_byte_address(0, 0, 2, 559 % 33, 559 / 33);
            const auto first_dropped = *gs_texture_byte_address(0, 0, 2, 560 % 33, 560 / 33);
            require(value_is(m, last_sent, src[559 * 3]) && unknown(m, first_dropped), "boundary");
        });
        scenario([&] {
            Ps2TextureMemory m;
            const auto src = ramp(27);
            const auto r = upload_retail_rect(m, {1, 0, 1, 0, 0, 3, 3}, src);
            require(r.pixels_written == 6 && r.bytes_written == 16 && r.partial_pixel_bytes == 1, "3x3 partial pixel");
            const auto a = *gs_texture_byte_address(0, 0, 1, 2, 1);  // pixel index 5
            require(value_is(m, a, src[15]) && value_is(m, a + 1, 0) && value_is(m, a + 2, 0) && unknown(m, a + 3),
                    "partial pixel zero-extended (Play GS behaviour), 3 bytes known");
            require(unknown(m, *gs_texture_byte_address(0, 0, 1, 0, 2)), "pixel 6 unwritten");
            require(known_bytes(m) == 18, "6 pixels");
        });
        scenario([&] {
            Ps2TextureMemory m;
            const auto r = upload_retail_rect(m, {1, 0, 1, 0, 0, 11, 1}, ramp(33));
            require(r.pixels_written == 11 && r.partial_pixel_bytes == 2, "2-byte partial pixel");
            require(known_bytes(m) == 33, "11 pixels known");
        });

        // TBW stride: a 33x9 rect at TBW 5 steps rows by 320 pixels, not 33.
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(33 * 9 * 3 / 16 * 16);
            transfer_host_to_local(m, {1, 0x80, 5, 0, 0, 33, 9}, d);
            for (unsigned y : {0u, 4u, 8u})
                for (unsigned x : {0u, 32u}) {
                    const std::size_t i = y * 33 + x;
                    if (i * 3 + 3 > d.size()) continue;
                    const auto a = *gs_texture_byte_address(0, 0x80, 5, x, y);
                    require(value_is(m, a, d[i * 3]) && value_is(m, a + 2, d[i * 3 + 2]) && unknown(m, a + 3), "stride");
                }
        });
        // Address wrap past 4 MiB.
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(64 * 40 * 3);
            transfer_host_to_local(m, {1, 0x3fff, 2, 0, 0, 64, 40}, d);
            require(known_bytes(m) == 64 * 40 * 3, "wrapped pixels all known");
            const auto a = *gs_texture_byte_address(0, 0x3fff, 2, 0, 39);
            require(value_is(m, a, d[39 * 64 * 3]), "wrapped row");
        });

        // Rejections are atomic: nothing written, identity unchanged.
        scenario([&] {
            Ps2TextureMemory m;
            const auto id0 = m.identity();
            const auto d = ramp(48);
            rejects("short source via retail rect", [&] { upload_retail_rect(m, {1, 0, 1, 0, 0, 16, 1}, ramp(32)); });
            rejects("qwc zero", [&] { upload_retail_rect(m, {1, 0, 1, 0, 0, 5, 1}, d); });
            rejects("payload exceeds rectangle", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 4, 1}, ramp(16)); });
            rejects("non-qword payload", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 16, 1}, ramp(45)); });
            rejects("zero width", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 0, 4}, d); });
            rejects("zero height", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 16, 0}, d); });
            rejects("width beyond limit", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 2049, 1}, d); });
            rejects("height beyond limit", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 1, 2049}, d); });
            rejects("origin beyond limit", [&] { transfer_host_to_local(m, {1, 0, 1, 2048, 0, 16, 1}, d); });
            rejects("rect leaves coordinate range on a later row",
                    [&] { transfer_host_to_local(m, {1, 0, 1, 0, 2047, 16, 2}, ramp(96)); });
            rejects("rect leaves coordinate range in a row",
                    [&] { transfer_host_to_local(m, {1, 0, 1, 2040, 0, 16, 1}, d); });
            rejects("tbp", [&] { transfer_host_to_local(m, {1, 0x4000, 1, 0, 0, 16, 1}, d); });
            rejects("tbw", [&] { transfer_host_to_local(m, {1, 0, 64, 0, 0, 16, 1}, d); });
            rejects("dimension above int", [&] { upload_retail_rect(m, {1, 0, 1, 0, 0, 0x80000000u, 1}, d); });
            require(known_bytes(m) == 0 && m.identity() == id0, "rejected transfers wrote nothing");
            for (const std::uint32_t psm : {2u, 10u, 27u, 0x30u, 0x31u, 0x32u, 0x3au, 18u, 21u, 45u})
                rejects("guarded sibling psm", [&] { transfer_host_to_local(m, {psm, 0, 1, 0, 0, 32, 1}, ramp(96)); });
            require(known_bytes(m) == 0 && m.identity() == id0, "guards wrote nothing");
        });

        // Raw XIM support: direct PSMCT24, no palette. Field also accepts PSM27/CPSM0.
        scenario([&] {
            const OwnedRawXim xim(build_xim(1, 0, 32, 16));
            const auto support = xim_upload_support(xim.layout());
            require(support.status == XimUploadStatus::Supported && support.reason == "PSMCT24 direct image", "support");
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto r = upload_xim(m, xim);
            require(r && r->palette_attempts.empty() && r->all_palettes_allocated(), "direct image, no palettes");
            const auto a = m.allocation(r->image);
            require(a && a->psm == GsPixelStorage::Psmct24 && a->width == 32 && a->height == 16 && a->allocation_class == 1,
                    "allocation");
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 32; ++x) {
                    const auto addr = *gs_texture_byte_address(0, a->base_block, a->buffer_width_64, x, y);
                    for (unsigned k = 0; k < 3; ++k) require(value_is(m, addr + k, xim.image()[(y * 32 + x) * 3 + k]), "xim pixel");
                    require(unknown(m, addr + 3), "xim high byte unknown");
                }
            require(known_bytes(m) == 32 * 16 * 3, "only the image bytes");
            rejects("psm1 smaller than a quadword", [&] { upload_xim(m, OwnedRawXim(build_xim(1, 0, 5, 1))); });
            const OwnedRawXim p27(build_xim(27, 0, 8, 8));
            require(xim_upload_support(p27.layout()).status == XimUploadStatus::Supported, "psm27 supported with palette");
            const OwnedRawXim p0(build_xim(0, 0, 8, 8));
            require(xim_upload_support(p0.layout()).status == XimUploadStatus::Supported, "psm0 unchanged");
        });

        // Every XIM of the owned NIGHT_V archive (optional: OKAGE_NIGHT_V_XIM_DIR).
        if (const char* dir = std::getenv("OKAGE_NIGHT_V_XIM_DIR")) {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            std::size_t images = 0, pixels = 0;
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(dir))
                if (e.path().extension() == ".xim") files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                std::ifstream in(f, std::ios::binary);
                std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const OwnedRawXim xim(bytes);
                require(xim.layout().pixel_format == 1 && xim.layout().palette_format == 0 && !xim.layout().indexed,
                        "NIGHT_V XIM is direct PSMCT24 / CPSM0");
                require(xim_upload_support(xim.layout()).status == XimUploadStatus::Supported, "supported");
                const auto r = upload_xim(m, xim);
                require(r.has_value() && r->palette_attempts.empty(), "NIGHT_V upload");
                const auto a = *m.allocation(r->image);
                const auto w = xim.layout().width, h = xim.layout().height;
                require((w * h * 3) % 16 == 0, "no dropped pixel tail in NIGHT_V");
                for (unsigned y = 0; y < h; ++y)
                    for (unsigned x = 0; x < w; ++x) {
                        const auto addr = *gs_texture_byte_address(0, a.base_block, a.buffer_width_64, x, y);
                        for (unsigned k = 0; k < 3; ++k)
                            require(value_is(m, addr + k, xim.image()[(y * w + x) * 3 + k]), "NIGHT_V pixel");
                        ++pixels;
                    }
                ++images;
            }
            require(images > 0 && known_bytes(m) == pixels * 3, "all NIGHT_V pixels, no other byte known");
            std::cout << "NIGHT_V XIMs verified: " << images << " images, " << pixels << " pixels\n";
        } else {
            std::cout << "NIGHT_V XIM directory not set (OKAGE_NIGHT_V_XIM_DIR); skipped\n";
        }
        std::cout << "texture-psm1-tests ok\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
