#include "okage/texture_transfer.hpp"

#include <array>
#include "okage/gs_texture_address.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace okage;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <class F> void rejects(const char* label, F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("expected rejection: ") + label);
}
std::vector<std::uint8_t> sequence(std::size_t n, std::uint8_t first = 1) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(first + i * 7);
    return v;
}
bool byte_is(const Ps2TextureMemory& m, std::uint32_t a, int expect) {  // expect<0: undefined
    const auto o = m.observe_byte(a);
    return expect < 0 ? !o.value : (o.value && *o.value == expect);
}
std::size_t defined_count(const Ps2TextureMemory& m) {
    std::size_t n = 0;
    for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a) n += m.observe_byte(a).value.has_value();
    return n;
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int k = 0; k < 4; ++k) v[at + k] = static_cast<std::uint8_t>(x >> (8 * k));
}
std::vector<std::uint8_t> build_xim(std::uint32_t psm, std::uint32_t cpsm, std::uint32_t colors,
                                    std::uint32_t w, std::uint32_t h, std::uint8_t seed) {
    const bool indexed = psm >= 16;
    const std::size_t pal = indexed ? 16 + colors * (cpsm == 2 ? 2 : 4) : 0;
    const std::size_t bytes = psm == 0 ? std::size_t(w) * h * 4 : psm == 1 ? std::size_t(w) * h * 3 : std::size_t(w) * h;
    std::vector<std::uint8_t> v(16 + pal + 16 + bytes, 0);
    put32(v, 0, psm << 20);
    put32(v, 4, cpsm << 19);
    if (indexed) {
        put32(v, 0x10, static_cast<std::uint32_t>(pal));
        put32(v, 0x18, 1);
        put32(v, 0x1c, colors);
        for (std::size_t i = 0; i < colors * (cpsm == 2 ? 2 : 4); ++i) v[0x20 + i] = static_cast<std::uint8_t>(seed + i * 5);
    }
    const auto ih = 16 + pal;
    put32(v, ih + 8, h);
    put32(v, ih + 12, w);
    for (std::size_t i = 0; i < bytes; ++i) v[ih + 16 + i] = static_cast<std::uint8_t>(seed * 3 + i * 11);
    return v;
}
}

template <class F> void scenario(F&& f) { f(); }

int main() {
    try {
        // Retail 0x1c9b48 floor semantics (shift of the signed product).
        require(retail_transfer_qwc(0, 64, 64) == 1024, "ct32 64x64 qwc");
        require(retail_transfer_qwc(0, 3, 3) == 2, "ct32 9 pixels floor to 2 qwc, not 3");
        require(retail_transfer_qwc(19, 64, 64) == 256, "t8 64x64 qwc");
        require(retail_transfer_qwc(19, 5, 5) == 1, "t8 25 bytes floor to 1 qwc");
        require(retail_transfer_qwc(19, 3, 5) == 0, "t8 15 bytes floor to 0 qwc");
        require(retail_transfer_qwc(1, 16, 16) == 48, "ct24 uses *3>>4");
        require(retail_transfer_qwc(0x14, 64, 64) == 128, "t4");
        require(retail_transfer_qwc(0x12, 64, 64) == 0, "unlisted psm yields zero like retail");
        rejects("negative dim", [] { retail_transfer_qwc(0, -1, 4); });

        // Generic PSMCT32 transfer to hand-derived addresses
        // (x1->+4, x2->+0x10, y1->+8, y2->+0x40, x8->+0x100, y8->+0x200).
        scenario([&] {
            Ps2TextureMemory m;
            const auto data = sequence(16 * 4);  // 4 quadwords = 16 pixels
            GsHostToLocal t{0, 0, 1, 0, 0, 16, 1};
            const auto r = transfer_host_to_local(m, t, data);
            require(r.bytes_written == 64 && r.pixels_written == 16, "ct32 result");
            for (int k = 0; k < 4; ++k) require(byte_is(m, k, data[k]), "pixel 0");
            for (int k = 0; k < 4; ++k) require(byte_is(m, 4 + k, data[4 + k]), "pixel 1 at +4");
            for (int k = 0; k < 4; ++k) require(byte_is(m, 0x10 + k, data[8 + k]), "pixel 2 at +0x10");
            for (int k = 0; k < 4; ++k) require(byte_is(m, 0x100 + k, data[32 + k]), "pixel 8 at +0x100");
            require(defined_count(m) == 64, "only the 64 transferred bytes are defined");
            require(byte_is(m, 8, -1), "row-1 position 0,1 untouched: y1 is +8");
        });
        // Row wrap uses rrw but stride comes from TBW: 2x2 at TBW1.
        scenario([&] {
            Ps2TextureMemory m;
            const auto data = sequence(16);
            transfer_host_to_local(m, {0, 0, 1, 0, 0, 2, 2}, data);
            require(byte_is(m, 0x04, data[4]) && byte_is(m, 0x08, data[8]) && byte_is(m, 0x0c, data[12]),
                    "(1,0)=+4 (0,1)=+8 (1,1)=+0xc");
            // Destination offset and wrap to dsax.
            Ps2TextureMemory n;
            transfer_host_to_local(n, {0, 0, 1, 8, 0, 2, 2}, data);
            require(byte_is(n, 0x100, data[0]) && byte_is(n, 0x104, data[4]) &&
                    byte_is(n, 0x108, data[8]), "dsax 8 -> block column +0x100");
        });
        // PSMT8 bytes at hand-derived addresses: x8->+2, x1->+4, y1->+8.
        scenario([&] {
            Ps2TextureMemory m;
            const auto data = sequence(16);
            transfer_host_to_local(m, {19, 0, 2, 0, 0, 16, 1}, data);
            require(byte_is(m, 0, data[0]) && byte_is(m, 4, data[1]) && byte_is(m, 2, data[8]),
                    "t8 x1=+4, x8=+2");
            require(defined_count(m) == 16, "16 bytes defined");
        });
        // Partial payload: only leading pixels written; later ones untouched.
        scenario([&] {
            Ps2TextureMemory m;
            const auto pre = sequence(4, 200);
            m.write_bytes(0x08, pre);  // pixel (0,1) pre-existing
            const auto data = sequence(16);  // one qword = 4 pixels of an 8x2 rect
            transfer_host_to_local(m, {0, 0, 1, 0, 0, 8, 2}, data);
            // 4 pixels cover (0..3,0); (0,1) stays at the old value.
            require(byte_is(m, 0x08, pre[0]) && byte_is(m, 0x0b, pre[3]), "untouched byte keeps value");
            require(byte_is(m, 0x10, data[8]), "pixel 2 written");
        });
        // Wrap at the 4 MiB top: PSMT8 tbp 0x3fff, x128 is page +0x2000 -> wraps.
        scenario([&] {
            Ps2TextureMemory m;
            const auto data = sequence(16);
            transfer_host_to_local(m, {19, 0x3fff, 2, 128, 0, 16, 1}, data);
            require(byte_is(m, 0x001f00, data[0]), "x128 page offset wraps past 4 MiB to 0x1f00");
            require(defined_count(m) == 16, "wrapped bytes still all defined");
        });
        // Source fidelity: literal transcription of 0x1cd858's per-entry
        // instruction sequence (slt/movn/sra/sll form), independent of the
        // closed form in clut_scratch_word_index. Checked for all i in 0..511.
        for (int t1 = 0; t1 < 512; ++t1) {
            int v1 = t1 + 0x1f; int a2 = t1 & 8; int a3 = t1 & 0x10;
            int a1 = a2 + 7; int a0 = a3 + 0xf;
            if (-1 < t1) v1 = t1;
            if (-1 < a2) a1 = a2;
            if (-1 < a3) a0 = a3;
            v1 >>= 5; a1 >>= 3; v1 <<= 1; a0 >>= 4;
            int v0 = t1 & 7; v1 += a1; a0 <<= 3; a0 += v0; v1 <<= 4; v1 += a0;
            require(static_cast<std::uint32_t>(v1) == clut_scratch_word_index(t1), "0x1cd858 transcription");
        }
        // Values pinned from local/research/upload-transfer-evidence-v1/derive_transfer.out.txt
        // (re-derived from 0x1c9b48/0x1cd928 assembly; floor vs ceil columns).
        struct Q { std::uint32_t psm, w, h, qwc; };
        for (const Q q : {Q{0, 64, 64, 1024}, Q{0, 16, 2, 8}, Q{0, 5, 5, 6}, Q{0, 3, 7, 5}, Q{0, 17, 17, 72},
                          Q{0, 1, 1, 0}, Q{0, 100, 3, 75}, Q{19, 64, 64, 256}, Q{19, 16, 2, 2},
                          Q{19, 5, 5, 1}, Q{19, 3, 7, 1}, Q{19, 17, 17, 18}, Q{19, 1, 1, 0}})
            require(retail_transfer_qwc(q.psm, q.w, q.h) == q.qwc, "pinned QWC");
        require(clut_rect_rows(16) == 2 && clut_rect_rows(32) == 2 && clut_rect_rows(256) == 16, "cd928 rectangle rows");
        // Rejections are atomic and explicit.
        scenario([&] {
            Ps2TextureMemory m;
            const auto data = sequence(32);
            rejects("psm 1", [&] { transfer_host_to_local(m, {1, 0, 1, 0, 0, 4, 4}, data); });
            rejects("psm 20", [&] { transfer_host_to_local(m, {20, 0, 1, 0, 0, 8, 8}, data); });
            rejects("tbp", [&] { transfer_host_to_local(m, {0, 0x4000, 1, 0, 0, 4, 4}, data); });
            rejects("tbw", [&] { transfer_host_to_local(m, {0, 0, 64, 0, 0, 4, 4}, data); });
            rejects("empty", [&] { transfer_host_to_local(m, {0, 0, 1, 0, 0, 0, 4}, data); });
            rejects("oversize payload", [&] { transfer_host_to_local(m, {0, 0, 1, 0, 0, 2, 2}, data); });
            rejects("non-qword payload", [&] { transfer_host_to_local(m, {0, 0, 1, 0, 0, 8, 8}, sequence(20)); });
            // Rect leaves coordinate range on a later row: nothing written.
            rejects("coordinate range", [&] { transfer_host_to_local(m, {0, 0, 1, 0, 2047, 4, 2}, data); });
            require(defined_count(m) == 0, "failed transfers wrote nothing");
        });
        // Retail floor: 3x3 PSMCT32 sends 2 qwc = 8 pixels; pixel 9 untouched.
        scenario([&] {
            Ps2TextureMemory m;
            const auto data = sequence(48);  // more source than sent
            const auto r = upload_retail_rect(m, {0, 0, 1, 0, 0, 3, 3}, data);
            require(r.bytes_written == 32 && r.pixels_written == 8, "floor sends 8 pixels");
            // Pixel 8 is (2,2): x2 -> +0x10, y2 -> +0x40 => 0x50.
            require(byte_is(m, 0x50, -1), "tail pixel not written");
            require(byte_is(m, 0x44, data[28]), "pixel 7 = (1,2) at +0x44");
            rejects("short source", [&] { upload_retail_rect(m, {0, 0, 1, 0, 0, 3, 3}, sequence(16)); });
            rejects("zero qwc", [&] { upload_retail_rect(m, {19, 0, 1, 0, 0, 3, 5}, sequence(16)); });
        });

        // Allocation image upload: 64x64 PSM0 environment-style, TBW 2 stride rounding.
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto a = m.allocate(1, GsPixelStorage::Psmct32, 64, 64);
            require(a.has_value(), "allocation");
            const auto image = sequence(64 * 64 * 4, 3);
            const auto r = upload_allocation_image(m, a->id, image);
            require(r.pixels_written == 4096, "64x64 pixels, not the rounded footprint");
            // Pixel (0,0) lands at block base; each pixel is 4 contiguous bytes at the helper address.
            const auto base = std::uint32_t(a->base_block) * 256;
            require(byte_is(m, base + 0, image[0]) && byte_is(m, base + 4, image[4]), "origin and x1");
            require(defined_count(m) == 64 * 64 * 4, "exactly the image bytes defined");
            rejects("short image", [&] { upload_allocation_image(m, a->id, sequence(100)); });
            rejects("dead allocation", [&] { upload_allocation_image(m, 4095, image); });
            const auto t8 = m.allocate(1, GsPixelStorage::Psmt8, 64, 64);
            require(t8.has_value(), "t8 allocation");
            upload_allocation_image(m, t8->id, sequence(64 * 64));
            const auto z = m.allocate(1, GsPixelStorage::Psmct24, 8, 8);
            require(z.has_value(), "ct24 allocation exists");
            rejects("psm1 upload", [&] { upload_allocation_image(m, z->id, sequence(8 * 8 * 3)); });
            const auto t4 = m.allocate(1, GsPixelStorage::Psmt4, 64, 64);
            rejects("psm t4 upload", [&] { upload_allocation_image(m, t4->id, sequence(2048)); });
        });

        // CLUT permutation derived from 0x1cd858: swap index bits 3 and 4.
        require(clut_scratch_word_index(0) == 0 && clut_scratch_word_index(7) == 7, "0..7 identity");
        require(clut_scratch_word_index(8) == 16 && clut_scratch_word_index(16) == 8, "8<->16");
        require(clut_scratch_word_index(24) == 24 && clut_scratch_word_index(32) == 32, "24 fixed, 32 fixed");
        require(clut_scratch_word_index(255) == 255, "255 fixed");
        require(clut_rect_rows(16) == 2 && clut_rect_rows(32) == 2 && clut_rect_rows(33) == 4 &&
                clut_rect_rows(256) == 16, "rect rows");
        scenario([&] {
            std::array<bool, 256> seen{};
            for (std::uint32_t i = 0; i < 256; ++i) seen[clut_scratch_word_index(i)] = true;
            for (bool b : seen) require(b, "n=256 permutation is a bijection");
        });
        scenario([&] {
            const auto pal = sequence(16 * 4, 9);
            const auto s = clut_scratch(16, pal);
            require(s.size() == 2048, "scratch is 2048 bytes");
            // Entry 8 -> word 16; entries 0..7 -> 0..7; words 8..15 unwritten zero.
            for (int k = 0; k < 4; ++k) require(s[16 * 4 + k] == pal[8 * 4 + k], "entry 8 at word 16");
            for (int k = 0; k < 4; ++k) require(s[8 * 4 + k] == 0, "word 8 stays zero");
            for (int k = 0; k < 4; ++k) require(s[23 * 4 + k] == pal[15 * 4 + k], "entry 15 at word 23");
            for (std::size_t i = 24 * 4; i < s.size(); ++i) require(s[i] == 0, "tail zero");
        });
        // CLUT upload: 16-entry palette zero-pads its 2x16 rectangle (proved writes).
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto a = m.allocate(0, GsPixelStorage::Psmct32, 16, 2);
            require(a.has_value(), "clut allocation");
            const auto pal = sequence(16 * 4, 11);
            const auto r = upload_clut(m, a->id, 16, pal);
            require(r.pixels_written == 32 && r.bytes_written == 128, "2 rows x 16 pixels");
            require(defined_count(m) == 128, "padding words are defined zero writes");
            const auto base = std::uint32_t(a->base_block) * 256;
            // Scratch word w is rectangle pixel (w%16, w/16) -> address of (x=w%16,y=w/16).
            // Word 16 (entry 8) = (0,1) -> +8. Word 8 (zero) = (8,0) -> +0x100.
            for (int k = 0; k < 4; ++k) require(byte_is(m, base + 8 + k, pal[8 * 4 + k]), "entry 8 at (0,1)");
            for (int k = 0; k < 4; ++k) require(byte_is(m, base + 0x100 + k, 0), "padding at (8,0)");
            rejects("n=0", [&] { upload_clut(m, a->id, 0, pal); });
            rejects("n too big", [&] { upload_clut(m, a->id, 513, sequence(513 * 4)); });
            rejects("short palette", [&] { upload_clut(m, a->id, 16, sequence(20)); });
            const auto wide = m.allocate(0, GsPixelStorage::Psmct32, 32, 2);
            rejects("wrong width", [&] { upload_clut(m, wide->id, 16, pal); });
            const auto small = m.allocate(0, GsPixelStorage::Psmct32, 16, 2);
            rejects("too few rows", [&] { upload_clut(m, small->id, 256, sequence(1024)); });
        });
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto a = m.allocate(0, GsPixelStorage::Psmct32, 16, 16);
            const auto r = upload_clut(m, a->id, 256, sequence(1024));
            require(r.pixels_written == 256 && defined_count(m) == 1024, "256-entry CLUT fills 16x16");
        });
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto raw = build_xim(19, 0, 256, 64, 64, 5);
            const OwnedRawXim xim(raw);
            const auto r = upload_xim(m, xim);
            require(r && r->palettes.size() == 1, "xim upload allocates image and one CLUT");
            const auto img = m.allocation(r->image);
            const auto pal = m.allocation(r->palettes[0]);
            require(img->psm == GsPixelStorage::Psmt8 && img->width == 64 && img->height == 64, "image alloc");
            require(pal->width == 16 && pal->height == 16 && pal->allocation_class == 0, "clut alloc 16x16");
            require(defined_count(m) == 4096 + 1024, "image bytes and CLUT bytes only");
            for (std::uint32_t y : {0u, 1u, 17u, 63u})
                for (std::uint32_t x : {0u, 1u, 9u, 63u}) {
                    const auto a = gs_texture_byte_address(19, img->base_block, img->buffer_width_64, x, y);
                    require(a && byte_is(m, *a, xim.image()[y * 64 + x]), "texel placement");
                }
            const OwnedRawXim bad(build_xim(19, 2, 256, 8, 8, 1));
            rejects("cpsm2", [&] { upload_xim(m, bad); });
            const OwnedRawXim ct24(build_xim(1, 0, 0, 8, 8, 1));
            rejects("psm1", [&] { upload_xim(m, ct24); });
            // PSM0 direct image has no CLUT.
            const OwnedRawXim direct(build_xim(0, 0, 0, 8, 8, 2));
            const auto d = upload_xim(m, direct);
            require(d && d->palettes.empty(), "direct image");
        });
        scenario([&] {
            // Image fits, CLUT does not: the image allocation is rolled back.
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            while (m.available_count() > 1)
                require(m.allocate(0, GsPixelStorage::Psmct32, 8, 8).has_value(), "tiny filler");
            const auto before = m.available_count();
            const auto r = upload_xim(m, OwnedRawXim(build_xim(19, 0, 256, 64, 64, 1)));
            require(r.has_value() && !r->complete && r->palettes.empty(), "CLUT allocation fails, result incomplete");
            require(m.available_count() == before - 1 && m.allocation(r->image).has_value(), "image stays allocated");
            require(defined_count(m) == 4096, "image bytes uploaded, no CLUT bytes");
            // Image allocation failure: nullopt, no state change.
            const auto occupancy = m.occupancy_bytes();
            require(!upload_xim(m, OwnedRawXim(build_xim(19, 0, 256, 64, 64, 1))).has_value(), "no record left");
            require(m.occupancy_bytes() == occupancy, "failed image allocation changes nothing");
        });
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto before = m.available_count();
            rejects("1x1 psm0", [&] { upload_xim(m, OwnedRawXim(build_xim(0, 0, 0, 1, 1, 1))); });
            rejects("3x3 psm19 qwc 0", [&] { upload_xim(m, OwnedRawXim(build_xim(19, 0, 16, 3, 3, 1))); });
            rejects("4096x1", [&] { upload_xim(m, OwnedRawXim(build_xim(19, 0, 16, 4096, 1, 1))); });
            rejects("2049x1", [&] { upload_xim(m, OwnedRawXim(build_xim(19, 0, 16, 2049, 1, 1))); });
            require(m.available_count() == before, "pre-check rejections allocate nothing");
            // Floor truncation through the full XIM path: 5x5 PSMT8 sends one quadword.
            const OwnedRawXim xim(build_xim(19, 0, 16, 5, 5, 3));
            const auto r = upload_xim(m, xim);
            require(r && r->complete && r->palettes.size() == 1, "16-colour 5x5");
            require(defined_count(m) == 16 + 128, "16 image bytes (not 25) plus the 16x2 CLUT rectangle");
        });
        std::cout << "texture transfer tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
