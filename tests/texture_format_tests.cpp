#include "okage/texture_transfer.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace okage;

namespace {
void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
template <class F> void rejects(const char* label, F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("expected rejection: ") + label);
}
template <class F> void scenario(F&& f) { f(); }

// "f0/0f" payload: bytes F0,0F,...; pixel i is the low (i even) / high (i odd) nibble of byte i/2.
std::vector<std::uint8_t> payload_f0_0f(std::size_t bytes) {
    std::vector<std::uint8_t> v(bytes);
    for (std::size_t k = 0; k < bytes; ++k) v[k] = (k & 1) ? 0x0f : 0xf0;
    return v;
}
std::vector<std::uint8_t> ramp(std::size_t bytes, unsigned seed = 3) {
    std::vector<std::uint8_t> v(bytes);
    for (std::size_t k = 0; k < bytes; ++k) v[k] = static_cast<std::uint8_t>(k * 167u + seed * 29u + 5u);
    return v;
}
unsigned pixel_nibble(const std::vector<std::uint8_t>& d, std::size_t i) { return (d[i / 2] >> ((i & 1) * 4)) & 15u; }

// PSMCT32 byte address of (x, 0), TBP 0, TBW 1, x < 32: x bit0 +4, bit1 +0x10, bit2 +0x20, bit3 +0x100, bit4 +0x400.
std::uint32_t ct32_row0(unsigned x) {
    return (x & 1 ? 4u : 0u) + (x & 2 ? 0x10u : 0u) + (x & 4 ? 0x20u : 0u) + (x & 8 ? 0x100u : 0u) + (x & 16 ? 0x400u : 0u);
}
std::uint32_t t4_row0(unsigned x) {
    return ((x & 1) + 4 * ((x >> 1) & 1) + 8 * ((x >> 2) & 1)) * 4 + (x >> 3);
}

std::size_t known_bytes(const Ps2TextureMemory& m) { return m.known_byte_addresses().size(); }
std::size_t known_nibbles(const Ps2TextureMemory& m) {
    std::size_t n = 0;
    for (auto a : m.known_byte_addresses()) {
        const auto k = m.observe_byte_masked(a).known_mask;
        n += (k & 1) + ((k >> 1) & 1);
    }
    return n;
}
}  // namespace

int main() {
    try {
        // ---- QWC floor for the 4-bit selectors (retail 0x1c9b48: w*h >> 5) ----
        require(retail_transfer_qwc(0x24, 64, 64) == 128 && retail_transfer_qwc(0x2c, 64, 64) == 128 &&
                retail_transfer_qwc(0x14, 64, 64) == 128, "4-bit qwc");
        require(retail_transfer_qwc(0x24, 5, 7) == 1 && retail_transfer_qwc(0x2c, 33, 17) == 17 &&
                retail_transfer_qwc(0x14, 5, 6) == 0, "4-bit floor");

        // ---- PSMT4HL: only bits 27..24 of each touched word are written ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = payload_f0_0f(16);  // 32 pixels, one row of 32
            const auto r = transfer_host_to_local(m, {0x24, 0, 1, 0, 0, 32, 1}, d);
            require(r.pixels_written == 32 && r.bytes_written == 16, "HL result");
            for (unsigned x = 0; x < 32; ++x) {
                const auto o = m.observe_byte_masked(ct32_row0(x) + 3);
                require(o.known_mask == 1, "HL writes only the low nibble of byte 3");
                require(o.value == pixel_nibble(d, x), "HL nibble value");
                require(!m.observe_byte(ct32_row0(x) + 3).value, "half-known byte has no whole-byte value");
                for (unsigned b = 0; b < 3; ++b)
                    require(m.observe_byte_masked(ct32_row0(x) + b).known_mask == 0, "other 24 bits stay unknown");
            }
            require(known_bytes(m) == 32 && known_nibbles(m) == 32, "exactly 32 nibbles known");
            const auto n = m.observe_nibble(ct32_row0(1) + 3, 0);
            require(n.known && n.value == 15, "observe_nibble known");
            require(!m.observe_nibble(ct32_row0(1) + 3, 1).known, "observe_nibble unknown");
        });
        // ---- PSMT4HH: bits 31..28 ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = payload_f0_0f(16);
            transfer_host_to_local(m, {0x2c, 0, 1, 0, 0, 32, 1}, d);
            for (unsigned x = 0; x < 32; ++x) {
                const auto o = m.observe_byte_masked(ct32_row0(x) + 3);
                require(o.known_mask == 2 && o.value == (pixel_nibble(d, x) << 4), "HH nibble in the high lane");
            }
            require(known_nibbles(m) == 32, "32 nibbles");
        });
        // ---- HL then HH to the same pixels: whole high byte becomes known; other 24 bits still unknown ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto lo = ramp(16, 1), hi = ramp(16, 2);
            transfer_host_to_local(m, {0x24, 0, 1, 0, 0, 32, 1}, lo);
            transfer_host_to_local(m, {0x2c, 0, 1, 0, 0, 32, 1}, hi);
            for (unsigned x = 0; x < 32; ++x) {
                const auto a = ct32_row0(x) + 3;
                const auto o = m.observe_byte_masked(a);
                require(o.known_mask == 3 && o.value == (pixel_nibble(lo, x) | (pixel_nibble(hi, x) << 4)),
                        "HL|HH combine into one known byte");
                require(m.observe_byte(a).value == o.value, "whole-byte observation now engaged");
                require(m.observe_byte_masked(a - 1).known_mask == 0, "lower bytes unknown");
            }
        });
        // ---- Pre-existing words: HL/HH replace one nibble, preserve the other 28 bits ----
        scenario([&] {
            Ps2TextureMemory m;
            const std::array<std::uint8_t, 4> word{0x11, 0x22, 0x33, 0xab};
            m.write_bytes(0, word);
            const auto d = payload_f0_0f(16);  // pixel 0 nibble = 0
            transfer_host_to_local(m, {0x24, 0, 1, 0, 0, 32, 1}, d);
            auto o = m.observe_byte_masked(3);
            require(o.known_mask == 3 && o.value == 0xa0, "HL replaced low nibble 0xb->0x0, kept 0xa");
            require(m.observe_byte(0).value == 0x11 && m.observe_byte(1).value == 0x22 &&
                    m.observe_byte(2).value == 0x33, "low 24 bits preserved");
            const auto e = ramp(16, 9);
            transfer_host_to_local(m, {0x2c, 0, 1, 0, 0, 32, 1}, e);
            o = m.observe_byte_masked(3);
            require(o.known_mask == 3 && o.value == ((pixel_nibble(e, 0) << 4) | 0x0), "HH replaced high nibble, kept HL");
        });
        // ---- Unknown stays unknown beside a known neighbour in the same word ----
        scenario([&] {
            Ps2TextureMemory m;
            m.write_nibble(3, 1, 0x7);  // word 0: only byte 3 high nibble known
            transfer_host_to_local(m, {0x24, 0, 1, 0, 0, 32, 1}, payload_f0_0f(16));
            const auto o = m.observe_byte_masked(3);
            require(o.known_mask == 3, "HL joined the previously known HH nibble");
            require((o.value >> 4) == 7, "pre-existing HH nibble kept");
        });

        // ---- PSMT4: hand-derived row and a pair of pixels sharing a byte ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(16);
            transfer_host_to_local(m, {0x14, 0, 1, 0, 0, 32, 1}, d);
            for (unsigned x = 0; x < 32; ++x) {
                const auto o = m.observe_byte_masked(t4_row0(x));
                require((o.known_mask & 1) && o.value % 16 == pixel_nibble(d, x), "T4 row 0 pixel");
            }
            require(known_nibbles(m) == 32, "32 nibbles known");
        });
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(48, 5);
            transfer_host_to_local(m, {0x14, 0, 1, 0, 0, 32, 3}, d);
            // (0,0) is the low nibble and (4,2) the high nibble of byte 0 (row 2 adds 4 to the nibble shift).
            const auto o = m.observe_byte_masked(0);
            require(o.known_mask == 3 && o.value == (pixel_nibble(d, 2 * 32 + 4) << 4 | pixel_nibble(d, 0)),
                    "T4 pixels (0,0),(4,2) share byte 0");
            require(m.observe_byte(0).value == o.value, "whole byte known");
        });
        // ---- A whole 128x128 PSMT4 page / 64x32 PSMT4HL page is covered exactly once ----
        scenario([&] {
            Ps2TextureMemory m;
            transfer_host_to_local(m, {0x14, 0, 2, 0, 0, 128, 128}, ramp(128 * 128 / 2));
            require(known_bytes(m) == 8192 && known_nibbles(m) == 16384, "T4 page is a bijection onto 8 KiB");
            const auto a = m.known_byte_addresses();
            require(a.front() == 0 && a.back() == 8191, "within the page");
        });
        scenario([&] {
            Ps2TextureMemory m;
            transfer_host_to_local(m, {0x24, 0, 1, 0, 0, 64, 32}, ramp(64 * 32 / 2));
            require(known_bytes(m) == 2048 && known_nibbles(m) == 2048, "HL page: one nibble per word");
            for (auto a : m.known_byte_addresses()) require(a % 4 == 3 && a < 8192, "byte 3 only, one page");
        });
        // ---- TBW stride: 64-wide rows at TBW 2 leave the second 64 columns of the 128 page untouched ----
        scenario([&] {
            Ps2TextureMemory m;
            transfer_host_to_local(m, {0x14, 0, 2, 0, 0, 64, 64}, ramp(64 * 64 / 2));
            require(known_nibbles(m) == 4096, "4096 pixels");
            Ps2TextureMemory full;
            transfer_host_to_local(full, {0x14, 0, 2, 0, 0, 128, 128}, ramp(128 * 128 / 2));
            require(known_nibbles(full) == 16384, "full page");
        });
        // ---- Odd TBW page rows (floor(page_y*TBW/2)): y=128 with TBW 3 starts page 1 (+8192) ----
        scenario([&] {
            Ps2TextureMemory m;
            transfer_host_to_local(m, {0x14, 0, 3, 0, 128, 32, 1}, ramp(16));
            const auto a = m.known_byte_addresses();
            require(!a.empty() && a.front() >= 8192 && a.back() < 16384, "TBW3 page row 1 -> page floor(3/2)=1");
            Ps2TextureMemory n;
            transfer_host_to_local(n, {0x14, 0, 3, 0, 256, 32, 1}, ramp(16));
            const auto b = n.known_byte_addresses();
            require(!b.empty() && b.front() >= 3 * 8192 && b.back() < 4 * 8192, "TBW3 page row 2 -> page 3");
        });
        // ---- Wrap over the 4 MiB space ----
        scenario([&] {
            Ps2TextureMemory m;
            transfer_host_to_local(m, {0x14, 0x3fe0, 2, 0, 0, 128, 128}, ramp(128 * 128 / 2));
            const auto a = m.known_byte_addresses();
            require(known_bytes(m) == 8192 && a.front() == 0x3fe000 && a.back() == 0x3fffff, "T4 page at the top");
            Ps2TextureMemory w;
            transfer_host_to_local(w, {0x14, 0x3fe0, 2, 128, 0, 32, 1}, ramp(16));
            const auto b = w.known_byte_addresses();
            require(!b.empty() && b.back() < 8192, "second page wraps to 0");
        });
        // ---- Odd tail: floor QWC drops the final pixels, origin offset honoured ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto src = ramp(88);
            const auto r = upload_retail_rect(m, {0x24, 0, 1, 7, 9, 35, 5}, src);  // 175 px -> 5 qwc = 160 px
            require(r.pixels_written == 160 && r.bytes_written == 80, "floor qwc");
            require(known_nibbles(m) == 160, "160 nibbles known");
            // Pixel 160 = (x=7+160%35=7+20, y=9+4): not written; pixel 159 = (7+19, 13) is.
            Ps2TextureMemory n;
            upload_retail_rect(n, {0x24, 0, 1, 7, 9, 35, 5}, src);
            rejects("sub-qword 4-bit", [&] { upload_retail_rect(n, {0x14, 0, 1, 0, 0, 5, 6}, src); });
        });
        // ---- Atomic rejection and guards ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(16);
            // Row 2047 + 1 leaves the coordinate range after the first row: nothing written.
            rejects("range 4-bit", [&] { transfer_host_to_local(m, {0x24, 0, 1, 0, 2047, 8, 8}, d); });
            rejects("range t4", [&] { transfer_host_to_local(m, {0x14, 0, 1, 2040, 0, 64, 1}, d); });
            rejects("payload too large", [&] { transfer_host_to_local(m, {0x2c, 0, 1, 0, 0, 4, 4}, d); });
            rejects("non-qword", [&] { transfer_host_to_local(m, {0x14, 0, 1, 0, 0, 32, 1}, ramp(20)); });
            require(known_bytes(m) == 0, "rejections wrote nothing");
            for (const std::uint32_t psm : {2u, 10u, 27u, 0x30u, 0x31u, 0x32u, 0x3au, 18u, 21u, 35u, 45u})
                rejects("guarded psm", [&] { transfer_host_to_local(m, {psm, 0, 1, 0, 0, 32, 1}, d); });
            require(known_bytes(m) == 0, "guards wrote nothing");
        });
        // ---- Allocation-driven upload for the new formats (allocator TBW, allocation texel size) ----
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            for (const auto psm : {GsPixelStorage::Psmt4, GsPixelStorage::Psmt4Hl, GsPixelStorage::Psmt4Hh}) {
                const auto a = m.allocate(1, psm, 64, 64);
                require(a.has_value(), "alloc");
                const auto r = upload_allocation_image(m, a->id, ramp(2048));
                require(r.pixels_written == 4096, "4096 pixels");
            }
            require(known_nibbles(m) == 3 * 4096, "three images, 4096 nibbles each");
        });
        // ---- PSM0 / PSM19 behaviour unchanged: whole bytes known, mask 3 ----
        scenario([&] {
            Ps2TextureMemory m;
            const auto d = ramp(16);
            transfer_host_to_local(m, {0, 0, 1, 0, 0, 4, 1}, d);
            for (unsigned i = 0; i < 16; ++i)
                require(m.observe_byte_masked(ct32_row0(i / 4) + i % 4).known_mask == 3 &&
                        m.observe_byte(ct32_row0(i / 4) + i % 4).value == d[i], "ct32 bytes whole");
            Ps2TextureMemory n;
            transfer_host_to_local(n, {19, 0, 2, 0, 0, 16, 1}, d);
            require(known_bytes(n) == 16 && known_nibbles(n) == 32, "t8 bytes whole");
        });

        // ---- Nibble ownership: PSMT4HL and PSMT4HH share a block with distinct owners ----
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto lo = m.allocate(0, GsPixelStorage::Psmt4Hl, 8, 8);
            // Only the low lane owned: observe_byte must not throw, and reports "no whole-byte owner".
            auto b = m.observe_byte(3);
            require(b.allocation_state_known && !b.allocation_owner && b.owner_split, "partial byte: split, no false owner");
            auto mb = m.observe_byte_masked(3);
            require(mb.nibble_owner[0] == lo->id && !mb.nibble_owner[1] && mb.owner_split && !mb.whole_byte_owner,
                    "low nibble owned, high free");
            require(!m.observe_byte_masked(0).owner_split && !m.observe_byte_masked(0).whole_byte_owner,
                    "bytes 0..2 unowned and not split");
            const auto hi = m.allocate(0, GsPixelStorage::Psmt4Hh, 8, 8);
            require(lo->base_block == hi->base_block, "share a block");
            mb = m.observe_byte_masked(3);
            require(mb.nibble_owner[0] == lo->id && mb.nibble_owner[1] == hi->id && mb.owner_split &&
                    !mb.whole_byte_owner, "distinct nibble owners");
            b = m.observe_byte(3);
            require(!b.allocation_owner && b.owner_split, "no whole-byte owner for split nibbles");
            require(m.owner_of_nibble(3, 0) == lo->id && m.owner_of_nibble(3, 1) == hi->id, "owner_of_nibble");
            require(m.owner_of_nibble(2, 0) == std::nullopt && m.owner_of_nibble(2, 1) == std::nullopt,
                    "lanes 0/1 unowned");
            bool threw = false;
            try { (void)m.owner_of_byte(3); } catch (const std::logic_error&) { threw = true; }
            require(threw, "owner_of_byte still refuses a split byte");
            // PSMCT32 over the same block is a different allocation class of ownership: whole-byte owner engaged.
            const auto z = m.allocate(0, GsPixelStorage::Psmz24, 8, 8);
            require(z.has_value(), "z24 allocation");
            const auto o = m.observe_byte_masked(m.block_address(z->id, 0, 0).byte_address);
            require(!o.owner_split && o.whole_byte_owner == z->id, "z24 owns whole low bytes");
        });
        // ---- Out-of-range and state guards for the new queries ----
        scenario([&] {
            Ps2TextureMemory m;
            bool a = false, b = false, c = false;
            try { (void)m.observe_nibble(Ps2TextureMemory::vram_bytes, 0); } catch (const std::out_of_range&) { a = true; }
            try { (void)m.observe_nibble(0, 2); } catch (const std::out_of_range&) { b = true; }
            try { m.write_nibble(0, 2, 1); } catch (const std::out_of_range&) { c = true; }
            require(a && b && c, "range checks");
            const auto u = m.observe_byte_masked(5);
            require(!u.allocation_state_known && u.known_mask == 0 && !u.whole_byte_owner, "uninitialised allocator: no owner claim");
            bool threw = false;
            try { (void)m.owner_of_nibble(0, 0); } catch (const std::logic_error&) { threw = true; }
            require(threw, "owner_of_nibble needs an initialised allocator");
        });

        // ---- Snapshots keep partial-lane state across later writes, frees and reuse ----
        scenario([&] {
            Ps2TextureMemory m;
            m.initialize_empty_allocator();
            const auto lo = m.allocate(0, GsPixelStorage::Psmt4Hl, 8, 8);
            transfer_host_to_local(m, {0x24, lo->base_block, 1, 0, 0, 32, 1}, ramp(16, 1));
            const auto s1 = m.snapshot();
            require(s1.shares_storage_with(m), "shares until the next write");
            const auto s1_value = s1.observe_byte_masked(3);
            require(s1_value.known_mask == 1, "snapshot has the HL nibble only");
            // Later HH write into the same byte must not leak into s1.
            m.free_allocation(lo->id);
            const auto hi = m.allocate(0, GsPixelStorage::Psmt4Hh, 8, 8);
            transfer_host_to_local(m, {0x2c, hi->base_block, 1, 0, 0, 32, 1}, ramp(16, 2));
            const auto live = m.observe_byte_masked(3);
            require(live.known_mask == 3 && live.owner_split, "live has both nibbles; HL freed, HH owned -> split");
            const auto old = s1.observe_byte_masked(3);
            require(old.known_mask == 1 && old.value == s1_value.value, "snapshot nibble state unchanged");
            require(old.nibble_owner[0] == lo->id && !old.nibble_owner[1] && old.owner_split,
                    "snapshot owners unchanged by free/reuse");
            require(live.nibble_owner[0] == std::nullopt && live.nibble_owner[1] == hi->id, "live owners");
            require(!s1.observe_byte(3).value && s1.observe_nibble(3, 0).known && !s1.observe_nibble(3, 1).known,
                    "snapshot nibble API");
            require(s1.known_byte_addresses().size() == 32, "snapshot known set unchanged");
            // A second snapshot after write_nibble keeps its own partial view.
            const auto s2 = m.snapshot();
            m.write_nibble(100, 0, 5);
            require(!s2.observe_nibble(100, 0).known && m.observe_nibble(100, 0).known, "write_nibble detaches");
            require(m.identity().content_generation > s2.identity().content_generation, "generation advanced");
        });

        std::cout << "texture format tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
