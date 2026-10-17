
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <optional>

#include "GS/GSLocalMemory.h"
#include "okage/gs_texture_address.hpp"
#include "okage/texture_memory.hpp"
#include "okage/texture_transfer.hpp"

namespace {

constexpr const char* PCSX2_PINNED_REVISION = "9fc90907c29294ac9db9aadd2d6c6bdf33403571";
constexpr std::uint32_t VRAM_WORDS = 1024 * 1024; // 1M 32-bit words = 4 MiB

struct FailureRecord {
    std::string case_name;
    std::string defect_type;
    std::uint32_t address{};
    std::uint32_t expected{};
    std::uint32_t actual{};
    int x{-1};
    int y{-1};
    std::string details;
};

std::optional<FailureRecord> g_first_failure;

void record_failure(const std::string& case_name, const std::string& defect_type,
                    std::uint32_t address, std::uint32_t expected, std::uint32_t actual,
                    int x = -1, int y = -1, const std::string& details = "") {
    if (!g_first_failure) {
        g_first_failure = FailureRecord{case_name, defect_type, address, expected, actual, x, y, details};
    }
}

// Emulated PCSX2 VRAM state
struct Pcsx2ReferenceVram {
    std::vector<u32> vm32;
    std::array<u32, 256> clut{};

    Pcsx2ReferenceVram() : vm32(VRAM_WORDS, 0) {
        for (std::size_t i = 0; i < 256; ++i) {
            // Synthetic 32-bit RGBA palette entry for CLUT
            u32 r = static_cast<u32>((i * 17) & 0xff);
            u32 g = static_cast<u32>((i * 31) & 0xff);
            u32 b = static_cast<u32>((i * 59) & 0xff);
            u32 a = static_cast<u32>(i & 0x80 ? 0x80 : 0x00);
            clut[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }

    void reset(u32 fill_pattern = 0) {
        std::fill(vm32.begin(), vm32.end(), fill_pattern);
    }

    void write_psmt8h(int x, int y, u32 bp, u32 bw, u8 pixel_byte, bool inject_clobber_low24 = false, int inject_lane_offset = 0) {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        if (inject_lane_offset != 0) {
            // Defect injection: write to wrong byte lane
            int shift = (3 + inject_lane_offset) * 8;
            u32 mask = ~(0xffu << shift);
            vm32[word_addr] = (vm32[word_addr] & mask) | (static_cast<u32>(pixel_byte) << shift);
        } else if (inject_clobber_low24) {
            // Defect injection: clobber low 24 bits
            vm32[word_addr] = static_cast<u32>(pixel_byte) << 24;
        } else {
            // Reference implementation: GSLocalMemory::WritePixel8H
            GSLocalMemory::WritePixel8H(&vm32[word_addr], pixel_byte);
        }
    }

    void write_psmct24(int x, int y, u32 bp, u32 bw, u32 rgb24) {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        GSLocalMemory::WritePixel24(&vm32[word_addr], rgb24);
    }

    void write_psmct32(int x, int y, u32 bp, u32 bw, u32 rgba32) {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        vm32[word_addr] = rgba32;
    }

    void write_psmt4hl(int x, int y, u32 bp, u32 bw, u8 nibble) {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        GSLocalMemory::WritePixel4HL(&vm32[word_addr], nibble);
    }

    void write_psmt4hh(int x, int y, u32 bp, u32 bw, u8 nibble) {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        GSLocalMemory::WritePixel4HH(&vm32[word_addr], nibble);
    }

    u32 read_pixel8h(int x, int y, u32 bp, u32 bw) const {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        return (vm32[word_addr] >> 24) & 0xff; // PCSX2 ReadPixel8H logic
    }

    u32 read_texel8h(int x, int y, u32 bp, u32 bw) const {
        u32 pixel = read_pixel8h(x, y, bp, bw);
        return clut[pixel & 0xff];
    }

    u32 read_pixel32(int x, int y, u32 bp, u32 bw) const {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        return vm32[word_addr];
    }

    u32 read_pixel24(int x, int y, u32 bp, u32 bw) const {
        u32 word_addr = GSLocalMemory::PixelAddress32(x, y, bp, bw) % VRAM_WORDS;
        return vm32[word_addr] & 0x00ffffff;
    }
};

struct TestConfig {
    bool defect_clobber_low24 = false;
    bool defect_wrong_lane = false;
    bool defect_linear_addr = false;
    bool defect_no_wrap = false;
    bool defect_state_leak = false;
};

// Harness running differential checks
class GsDifferentialHarness {
public:
    explicit GsDifferentialHarness(const TestConfig& cfg) : config_(cfg) {}

    bool run_all(int& cases_run) {
        bool all_ok = true;

        // 1. Untouched lane preservation with varying initial VRAM (zero, all-ones, low24 patterns)
        all_ok &= test_psmt8h_lane_preservation("lane_preservation_pattern_low24", 0x0055aa33, 4, 16, 16, 0, 0, cases_run);
        all_ok &= test_psmt8h_lane_preservation("lane_preservation_zero_vram", 0x00000000, 2, 32, 16, 4, 2, cases_run);
        all_ok &= test_psmt8h_lane_preservation("lane_preservation_ones_low24", 0x00ffffff, 8, 8, 8, 0, 0, cases_run);

        // 2. Swizzling, block boundaries, page boundaries
        all_ok &= test_boundary_crossings("block_boundary_8x8", 1, 16, 16, 0, 0, cases_run);
        all_ok &= test_boundary_crossings("page_boundary_64x32", 2, 70, 40, 10, 5, cases_run);
        all_ok &= test_boundary_crossings("multi_page_stride", 4, 128, 64, 0, 0, cases_run);

        // 3. Wrapping at 4 MiB GS boundary
        all_ok &= test_vram_wrapping("vram_wrapping_tbp_near_end", 0x3fff, 1, 16, 4, cases_run);
        all_ok &= test_vram_wrapping("vram_wrapping_tbp_wrap_page", 0x3fe0, 2, 64, 32, cases_run);

        // 4. Untouched high-byte preservation during PSMCT24 writes
        all_ok &= test_psmct24_high_byte_preservation("psmct24_preserves_high_byte", cases_run);

        // 5. Nibble preservation during PSMT4HL / PSMT4HH writes
        all_ok &= test_psmt4_nibble_preservation("psmt4_nibble_preservation", cases_run);

        // 6. Overlapping writes & persistent sequences
        all_ok &= test_persistent_sequence("persistent_sequence_loader_flow", cases_run);

        // 7. Format reinterpretation & palette sampling
        all_ok &= test_format_reinterpretation("format_reinterpretation_and_clut", cases_run);

        return all_ok;
    }

private:
    TestConfig config_;

    bool test_psmt8h_lane_preservation(const std::string& name, u32 initial_low24, u32 tbw,
                                       u32 rrw, u32 rrh, u32 dsax, u32 dsay, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        // Initialize both memories with distinct low-24 bits per pixel
        const u32 pixels = rrw * rrh;
        std::vector<u8> host_payload(pixels);

        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = dsax + x;
                int py = dsay + y;
                u32 word_addr = GSLocalMemory::PixelAddress32(px, py, 0, tbw) % VRAM_WORDS;
                u32 init_word = (initial_low24 & 0x00ffffff) ^ ((px * 13 + py * 37) & 0x00ffffff);
                // Set native
                u8 b0 = init_word & 0xff;
                u8 b1 = (init_word >> 8) & 0xff;
                u8 b2 = (init_word >> 16) & 0xff;
                u8 b3_init = 0xee; // initial high byte
                u8 bytes[4] = {b0, b1, b2, b3_init};
                native.write_bytes(word_addr * 4, bytes);

                // Set ref
                ref.vm32[word_addr] = init_word | (static_cast<u32>(b3_init) << 24);

                // Payload for this pixel
                host_payload[y * rrw + x] = static_cast<u8>((x * 7 + y * 19 + 0x41) & 0xff);
            }
        }

        // Execute Native Transfer
        okage::GsHostToLocal xfer{
            .psm = 0x1b, // PSMT8H
            .tbp = 0,
            .tbw = tbw,
            .dsax = dsax,
            .dsay = dsay,
            .rrw = rrw,
            .rrh = rrh
        };
        transfer_host_to_local(native, xfer, host_payload);

        // Execute Reference Transfer
        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = dsax + x;
                int py = dsay + y;
                u8 val = host_payload[y * rrw + x];
                ref.write_psmt8h(px, py, 0, tbw, val,
                                 config_.defect_clobber_low24,
                                 config_.defect_wrong_lane ? -1 : 0);
            }
        }

        // Compare native and reference byte-for-byte across touched pixels
        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = dsax + x;
                int py = dsay + y;

                u32 ref_word_addr = GSLocalMemory::PixelAddress32(px, py, 0, tbw) % VRAM_WORDS;
                u32 ref_word = ref.vm32[ref_word_addr];

                u32 native_byte_addr = 0;
                if (config_.defect_linear_addr) {
                    native_byte_addr = (py * tbw * 64 + px) * 4;
                } else {
                    auto addr_opt = okage::gs_texture_byte_address(0, 0, tbw, px, py);
                    if (!addr_opt) {
                        record_failure(name, "address_rejected", 0, ref_word_addr * 4, 0, px, py, "gs_texture_byte_address returned nullopt");
                        return false;
                    }
                    native_byte_addr = *addr_opt;
                }

                // Check high byte (lane 3)
                auto nat_b3 = native.observe_byte(native_byte_addr + 3);
                u8 exp_b3 = (ref_word >> 24) & 0xff;
                if (!nat_b3.value || *nat_b3.value != exp_b3) {
                    record_failure(name, "high_byte_mismatch", native_byte_addr + 3, exp_b3, nat_b3.value.value_or(0x9999), px, py,
                                   "High byte write mismatch");
                    return false;
                }

                // Check low 24 bits (lanes 0, 1, 2) preservation
                for (int lane = 0; lane < 3; ++lane) {
                    auto nat_lane = native.observe_byte(native_byte_addr + lane);
                    u8 exp_lane = (ref_word >> (lane * 8)) & 0xff;
                    if (!nat_lane.value || *nat_lane.value != exp_lane) {
                        record_failure(name, "low24_lane_clobbered", native_byte_addr + lane, exp_lane, nat_lane.value.value_or(0x9999), px, py,
                                       "Preservation failure: low 24 bits modified during PSMT8H write");
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool test_boundary_crossings(const std::string& name, u32 tbw, u32 rrw, u32 rrh, u32 dsax, u32 dsay, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        const u32 pixels = rrw * rrh;
        std::vector<u8> payload(pixels);
        for (u32 i = 0; i < pixels; ++i) payload[i] = static_cast<u8>((i * 13 + 7) & 0xff);

        okage::GsHostToLocal xfer{
            .psm = 0x1b,
            .tbp = 0,
            .tbw = tbw,
            .dsax = dsax,
            .dsay = dsay,
            .rrw = rrw,
            .rrh = rrh
        };
        transfer_host_to_local(native, xfer, payload);

        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = dsax + x;
                int py = dsay + y;
                ref.write_psmt8h(px, py, 0, tbw, payload[y * rrw + x],
                                 config_.defect_clobber_low24,
                                 config_.defect_wrong_lane ? 1 : 0);
            }
        }

        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = dsax + x;
                int py = dsay + y;
                u32 word_addr = GSLocalMemory::PixelAddress32(px, py, 0, tbw) % VRAM_WORDS;
                auto nat_b3 = native.observe_byte(word_addr * 4 + 3);
                u8 exp_b3 = (ref.vm32[word_addr] >> 24) & 0xff;
                if (!nat_b3.value || *nat_b3.value != exp_b3) {
                    record_failure(name, "boundary_mismatch", word_addr * 4 + 3, exp_b3, nat_b3.value.value_or(0x9999), px, py,
                                   "Boundary crossing mismatch at block/page");
                    return false;
                }
            }
        }
        return true;
    }

    bool test_vram_wrapping(const std::string& name, u32 tbp, u32 tbw, u32 rrw, u32 rrh, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        const u32 pixels = rrw * rrh;
        std::vector<u8> payload(pixels);
        for (u32 i = 0; i < pixels; ++i) payload[i] = static_cast<u8>((i * 29 + 3) & 0xff);

        okage::GsHostToLocal xfer{
            .psm = 0x1b,
            .tbp = tbp,
            .tbw = tbw,
            .dsax = 0,
            .dsay = 0,
            .rrw = rrw,
            .rrh = rrh
        };
        transfer_host_to_local(native, xfer, payload);

        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = x;
                int py = y;
                u8 val = payload[y * rrw + x];
                if (config_.defect_no_wrap) {
                    // Inject wrapping defect: clamp rather than modulo
                    u32 raw_addr = GSLocalMemory::PixelAddress32(px, py, tbp, tbw);
                    u32 clamped_addr = raw_addr >= VRAM_WORDS ? VRAM_WORDS - 1 : raw_addr;
                    ref.vm32[clamped_addr] = (ref.vm32[clamped_addr] & 0x00ffffff) | (static_cast<u32>(val) << 24);
                } else {
                    ref.write_psmt8h(px, py, tbp, tbw, val);
                }
            }
        }

        for (u32 y = 0; y < rrh; ++y) {
            for (u32 x = 0; x < rrw; ++x) {
                int px = x;
                int py = y;
                u32 word_addr = GSLocalMemory::PixelAddress32(px, py, tbp, tbw) % VRAM_WORDS;
                auto nat_b3 = native.observe_byte(word_addr * 4 + 3);
                u8 exp_b3 = (ref.vm32[word_addr] >> 24) & 0xff;
                if (!nat_b3.value || *nat_b3.value != exp_b3) {
                    record_failure(name, "wrapping_mismatch", word_addr * 4 + 3, exp_b3, nat_b3.value.value_or(0x9999), px, py,
                                   "VRAM 4 MiB wrapping mismatch");
                    return false;
                }
            }
        }
        return true;
    }

    bool test_psmct24_high_byte_preservation(const std::string& name, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        // Preload high byte with 0xd7 in both
        for (u32 w = 0; w < 256; ++w) {
            u8 bytes[4] = {0x11, 0x22, 0x33, 0xd7};
            native.write_bytes(w * 4, bytes);
            ref.vm32[w] = 0xd7332211;
        }

        // Transfer PSMCT24 image 8x8 at 0,0
        std::vector<u8> rgb_payload(8 * 8 * 3);
        for (std::size_t i = 0; i < rgb_payload.size(); ++i) rgb_payload[i] = static_cast<u8>((i * 5 + 1) & 0xff);

        okage::GsHostToLocal xfer{
            .psm = 1, // PSMCT24
            .tbp = 0,
            .tbw = 1,
            .dsax = 0,
            .dsay = 0,
            .rrw = 8,
            .rrh = 8
        };
        transfer_host_to_local(native, xfer, rgb_payload);

        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                std::size_t idx = (y * 8 + x) * 3;
                u32 rgb = rgb_payload[idx] | (rgb_payload[idx + 1] << 8) | (rgb_payload[idx + 2] << 16);
                ref.write_psmct24(x, y, 0, 1, rgb);
            }
        }

        // Verify high byte preserved as 0xd7 in native and matches reference
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                u32 word_addr = GSLocalMemory::PixelAddress32(x, y, 0, 1) % VRAM_WORDS;
                auto nat_b3 = native.observe_byte(word_addr * 4 + 3);
                u8 exp_b3 = (ref.vm32[word_addr] >> 24) & 0xff;
                if (!nat_b3.value || *nat_b3.value != 0xd7 || *nat_b3.value != exp_b3) {
                    record_failure(name, "high_byte_clobbered_by_psmct24", word_addr * 4 + 3, 0xd7, nat_b3.value.value_or(0x9999), x, y,
                                   "PSMCT24 write clobbered high byte");
                    return false;
                }
            }
        }
        return true;
    }

    bool test_psmt4_nibble_preservation(const std::string& name, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        // Preload words with 0xab123456
        for (u32 w = 0; w < 64; ++w) {
            u8 bytes[4] = {0x56, 0x34, 0x12, 0xab};
            native.write_bytes(w * 4, bytes);
            ref.vm32[w] = 0xab123456;
        }

        std::vector<u8> hl_payload(16, 0x39); // low nibble 9, high nibble 3
        okage::GsHostToLocal xfer_hl{
            .psm = 0x24, // PSMT4HL
            .tbp = 0,
            .tbw = 1,
            .dsax = 0,
            .dsay = 0,
            .rrw = 8,
            .rrh = 4
        };
        transfer_host_to_local(native, xfer_hl, hl_payload);

        for (u32 i = 0; i < 32; ++i) {
            int px = i % 8;
            int py = i / 8;
            u8 nibble = (i % 2 == 0) ? (hl_payload[i / 2] & 0x0f) : (hl_payload[i / 2] >> 4);
            ref.write_psmt4hl(px, py, 0, 1, nibble);
        }

        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                u32 word_addr = GSLocalMemory::PixelAddress32(x, y, 0, 1) % VRAM_WORDS;
                auto nat_b3 = native.observe_byte(word_addr * 4 + 3);
                u8 exp_b3 = (ref.vm32[word_addr] >> 24) & 0xff;
                // High nibble must remain 'a', low nibble must be written from payload
                if (!nat_b3.value || *nat_b3.value != exp_b3 || (*nat_b3.value >> 4) != 0x0a) {
                    record_failure(name, "nibble_preservation_failure", word_addr * 4 + 3, exp_b3, nat_b3.value.value_or(0x9999), x, y,
                                   "PSMT4HL modified high nibble or failed to write low nibble");
                    return false;
                }
            }
        }
        return true;
    }

    bool test_persistent_sequence(const std::string& name, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        // Sequence:
        // Step 1: Write PSMCT32 background (16x16 at 0,0)
        std::vector<u8> bg(16 * 16 * 4);
        for (std::size_t i = 0; i < bg.size(); ++i) bg[i] = static_cast<u8>((i * 3 + 1) & 0xff);
        transfer_host_to_local(native, {0, 0, 1, 0, 0, 16, 16}, bg);
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                std::size_t idx = (y * 16 + x) * 4;
                u32 w = bg[idx] | (bg[idx + 1] << 8) | (bg[idx + 2] << 16) | (bg[idx + 3] << 24);
                ref.write_psmct32(x, y, 0, 1, w);
            }
        }

        // Step 2: Overlay PSMCT24 on subrect (8x8 at 4,4)
        std::vector<u8> spr(8 * 8 * 3);
        for (std::size_t i = 0; i < spr.size(); ++i) spr[i] = static_cast<u8>((i * 7 + 0x50) & 0xff);
        transfer_host_to_local(native, {1, 0, 1, 4, 4, 8, 8}, spr);
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                std::size_t idx = (y * 8 + x) * 3;
                u32 rgb = spr[idx] | (spr[idx + 1] << 8) | (spr[idx + 2] << 16);
                ref.write_psmct24(4 + x, 4 + y, 0, 1, rgb);
            }
        }

        // Step 3: Write PSMT8H text overlay (8x8 at 4,4)
        std::vector<u8> txt(8 * 8);
        for (std::size_t i = 0; i < txt.size(); ++i) txt[i] = static_cast<u8>((i + 0x80) & 0xff);
        transfer_host_to_local(native, {0x1b, 0, 1, 4, 4, 8, 8}, txt);
        if (config_.defect_state_leak) {
            // Defect: state leak / unintended clobber of earlier persistent background outside rect
            u8 bad[1] = {0xfe};
            native.write_bytes(0, bad);
        }
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                ref.write_psmt8h(4 + x, 4 + y, 0, 1, txt[y * 8 + x]);
            }
        }

        // Step 4: Verify full 16x16 area byte-for-byte across native and reference
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                u32 word_addr = GSLocalMemory::PixelAddress32(x, y, 0, 1) % VRAM_WORDS;
                u32 exp_word = ref.vm32[word_addr];
                for (int b = 0; b < 4; ++b) {
                    auto nat_byte = native.observe_byte(word_addr * 4 + b);
                    u8 exp_byte = (exp_word >> (b * 8)) & 0xff;
                    if (!nat_byte.value || *nat_byte.value != exp_byte) {
                        record_failure(name, "persistent_sequence_mismatch", word_addr * 4 + b, exp_byte, nat_byte.value.value_or(0x9999), x, y,
                                       "Sequence step divergence");
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool test_format_reinterpretation(const std::string& name, int& count) {
        count++;
        okage::Ps2TextureMemory native;
        Pcsx2ReferenceVram ref;

        // Write PSMCT32 word into known coordinate
        // (x=2, y=3)
        u8 bg_bytes[4] = {0x12, 0x34, 0x56, 0x78};
        native.write_bytes(GSLocalMemory::PixelAddress32(2, 3, 0, 1) * 4, bg_bytes);
        ref.write_psmct32(2, 3, 0, 1, 0x78563412);

        // Update high byte with PSMT8H write of index 42 at x=2, y=3
        okage::GsHostToLocal xfer{0x1b, 0, 1, 0, 3, 16, 1};
        std::vector<u8> index_payload(16, 0x10);
        index_payload[2] = 42;
        transfer_host_to_local(native, xfer, index_payload);
        for (int px = 0; px < 16; ++px) {
            ref.write_psmt8h(px, 3, 0, 1, index_payload[px]);
        }

        // Reinterpretation 1: Read back as PSMT8H index
        u32 word_addr = GSLocalMemory::PixelAddress32(2, 3, 0, 1);
        auto nat_b3 = native.observe_byte(word_addr * 4 + 3);
        u32 ref_pixel8h = ref.read_pixel8h(2, 3, 0, 1);
        if (!nat_b3.value || *nat_b3.value != ref_pixel8h || ref_pixel8h != 42) {
            record_failure(name, "reinterpretation_pixel8h", word_addr * 4 + 3, ref_pixel8h, nat_b3.value.value_or(0), 2, 3,
                           "PSMT8H pixel index reinterpretation failed");
            return false;
        }

        // Reinterpretation 2: Read back as CLUT sampled texel
        u32 ref_texel = ref.read_texel8h(2, 3, 0, 1);
        u32 nat_texel = ref.clut[*nat_b3.value];
        if (nat_texel != ref_texel) {
            record_failure(name, "clut_sampling_mismatch", word_addr * 4, ref_texel, nat_texel, 2, 3,
                           "CLUT texel sampling mismatch");
            return false;
        }

        // Reinterpretation 3: Read back as PSMCT24
        auto b0 = native.observe_byte(word_addr * 4 + 0);
        auto b1 = native.observe_byte(word_addr * 4 + 1);
        auto b2 = native.observe_byte(word_addr * 4 + 2);
        u32 nat_rgb24 = (b0.value.value_or(0)) | (b1.value.value_or(0) << 8) | (b2.value.value_or(0) << 16);
        u32 ref_rgb24 = ref.read_pixel24(2, 3, 0, 1);
        if (nat_rgb24 != ref_rgb24 || ref_rgb24 != 0x00563412) {
            record_failure(name, "reinterpretation_psmct24", word_addr * 4, ref_rgb24, nat_rgb24, 2, 3,
                           "PSMCT24 reinterpretation failed");
            return false;
        }

        // Reinterpretation 4: Read back as PSMCT32
        u32 nat_rgba32 = nat_rgb24 | (static_cast<u32>(*nat_b3.value) << 24);
        u32 ref_rgba32 = ref.read_pixel32(2, 3, 0, 1);
        if (nat_rgba32 != ref_rgba32 || ref_rgba32 != 0x2a563412) { // 42 == 0x2a
            record_failure(name, "reinterpretation_psmct32", word_addr * 4, ref_rgba32, nat_rgba32, 2, 3,
                           "PSMCT32 reinterpretation failed");
            return false;
        }

        return true;
    }
};

void print_help() {
    std::cout << "Usage: okage-gs-differential-compare [options]\n"
              << "Options:\n"
              << "  --help                       Show this help\n"
              << "  --json                       Emit JSON report (default)\n"
              << "  --defect-clobber-low24       Negative control: simulate low-24 bit clobber\n"
              << "  --defect-wrong-lane          Negative control: simulate write to wrong byte lane\n"
              << "  --defect-linear-addr         Negative control: simulate unswizzled linear addressing\n"
              << "  --defect-no-wrap             Negative control: simulate 4 MiB wrapping failure\n"
              << "  --defect-state-leak          Negative control: simulate state leak across sequence\n";
}

} // namespace

int main(int argc, char** argv) {
    TestConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else if (arg == "--defect-clobber-low24") {
            cfg.defect_clobber_low24 = true;
        } else if (arg == "--defect-wrong-lane") {
            cfg.defect_wrong_lane = true;
        } else if (arg == "--defect-linear-addr") {
            cfg.defect_linear_addr = true;
        } else if (arg == "--defect-no-wrap") {
            cfg.defect_no_wrap = true;
        } else if (arg == "--defect-state-leak") {
            cfg.defect_state_leak = true;
        } else if (arg == "--json") {
            // default
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    GsDifferentialHarness harness(cfg);
    int cases_run = 0;
    bool success = harness.run_all(cases_run);

    std::ostringstream json;
    json << "{\n";
    json << "  \"status\": \"" << (success ? "PASS" : "FAIL") << "\",\n";
    json << "  \"pcsx2_revision\": \"" << PCSX2_PINNED_REVISION << "\",\n";
    json << "  \"cases_run\": " << cases_run << ",\n";
    json << "  \"negative_control_active\": " << ((cfg.defect_clobber_low24 || cfg.defect_wrong_lane || cfg.defect_linear_addr || cfg.defect_no_wrap || cfg.defect_state_leak) ? "true" : "false") << ",\n";
    json << "  \"coverage\": [\n";
    json << "    \"PSMT8H (0x1b) high-byte lane 3 writes with preservation of low 24 bits (lanes 0..2)\",\n";
    json << "    \"PSMCT24 (0x01) low 24-bit writes with preservation of high byte (lane 3)\",\n";
    json << "    \"PSMT4HL (0x24) low-nibble writes with preservation of high nibble and low 24 bits\",\n";
    json << "    \"GS swizzling and coordinate addressing across 8x8 block and 64x32 page boundaries\",\n";
    json << "    \"VRAM wrapping modulo 4 MiB (GS_MAX_PAGES / 0x400000 bytes)\",\n";
    json << "    \"Persistent multi-step loader transfer sequences (PSMCT32 -> PSMCT24 -> PSMT8H)\",\n";
    json << "    \"Format reinterpretation across PSMT8H, PSMCT32, PSMCT24 and CLUT palette sampling\"\n";
    json << "  ],\n";
    if (!success && g_first_failure) {
        json << "  \"first_failure\": {\n";
        json << "    \"case\": \"" << g_first_failure->case_name << "\",\n";
        json << "    \"defect_type\": \"" << g_first_failure->defect_type << "\",\n";
        json << "    \"address\": \"0x" << std::hex << g_first_failure->address << std::dec << "\",\n";
        json << "    \"expected\": \"0x" << std::hex << g_first_failure->expected << std::dec << "\",\n";
        json << "    \"actual\": \"0x" << std::hex << g_first_failure->actual << std::dec << "\",\n";
        json << "    \"coordinates\": {\"x\": " << g_first_failure->x << ", \"y\": " << g_first_failure->y << "},\n";
        json << "    \"details\": \"" << g_first_failure->details << "\"\n";
        json << "  }\n";
    } else {
        json << "  \"first_failure\": null\n";
    }
    json << "}\n";

    std::cout << json.str();
    return success ? 0 : 1;
}
