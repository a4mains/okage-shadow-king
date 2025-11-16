#include "okage/gs_texture_address.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
using okage::gs_texture_byte_address;
using okage::gs_texel_address;

void equal(std::string_view label, std::optional<std::uint32_t> actual, std::uint32_t expected) {
    if (!actual || *actual != expected) {
        std::cerr << label << ": expected 0x" << std::hex << expected << ", got ";
        if (actual) std::cerr << "0x" << *actual;
        else std::cerr << "invalid";
        std::cerr << '\n';
        std::exit(1);
    }
}

void texel(std::string_view label, std::optional<okage::GsTexelAddress> actual,
           std::uint32_t address, unsigned shift, unsigned bits) {
    if (!actual || actual->byte_address != address || actual->shift != shift || actual->bits != bits) {
        std::cerr << label << ": expected address/shift/bits " << std::hex << address
                  << '/' << shift << '/' << bits << ", got ";
        if (actual) std::cerr << actual->byte_address << '/' << unsigned(actual->shift) << '/' << unsigned(actual->bits);
        else std::cerr << "invalid";
        std::cerr << '\n';
        std::exit(1);
    }
}
void invalid_texel(std::string_view label, std::optional<okage::GsTexelAddress> actual) {
    if (actual) {
        std::cerr << label << ": expected invalid texel\n";
        std::exit(1);
    }
}

void invalid(std::string_view label, std::optional<std::uint32_t> actual) {
    if (actual) {
        std::cerr << label << ": expected invalid, got 0x" << std::hex << *actual << '\n';
        std::exit(1);
    }
}
}

int main() {
    // PSMCT32 page/block and 32-bit column pattern.
    equal("ct32 origin", gs_texture_byte_address(0, 0, 1, 0, 0), 0x0000);
    equal("ct32 x bit 0", gs_texture_byte_address(0, 0, 1, 1, 0), 0x0004);
    equal("ct32 x bit 1", gs_texture_byte_address(0, 0, 1, 2, 0), 0x0010);
    equal("ct32 y parity", gs_texture_byte_address(0, 0, 1, 0, 1), 0x0008);
    equal("ct32 x bit 2", gs_texture_byte_address(0, 0, 1, 4, 0), 0x0020);
    equal("ct32 row band", gs_texture_byte_address(0, 0, 1, 0, 2), 0x0040);
    equal("ct32 block column", gs_texture_byte_address(0, 0, 1, 8, 0), 0x0100);
    equal("ct32 block row", gs_texture_byte_address(0, 0, 1, 0, 8), 0x0200);
    equal("ct32 next page x", gs_texture_byte_address(0, 0, 1, 64, 0), 0x2000);
    equal("ct32 next page row", gs_texture_byte_address(0, 0, 2, 0, 32), 0x4000);

    // PSMT8's byte lanes, interleaved column word, 16x16 blocks and page rows.
    equal("t8 origin", gs_texture_byte_address(19, 0, 2, 0, 0), 0x0000);
    equal("t8 x lane", gs_texture_byte_address(19, 0, 2, 8, 0), 0x0002);
    equal("t8 column x", gs_texture_byte_address(19, 0, 2, 1, 0), 0x0004);
    equal("t8 y parity", gs_texture_byte_address(19, 0, 2, 0, 1), 0x0008);
    equal("t8 high x column bit", gs_texture_byte_address(19, 0, 2, 4, 0), 0x0020);
    equal("t8 y column selector", gs_texture_byte_address(19, 0, 2, 0, 2), 0x0021);
    equal("t8 next 64-byte band", gs_texture_byte_address(19, 0, 2, 0, 4), 0x0060);
    equal("t8 block column", gs_texture_byte_address(19, 0, 2, 16, 0), 0x0100);
    equal("t8 block row", gs_texture_byte_address(19, 0, 2, 0, 16), 0x0200);
    equal("t8 next page x", gs_texture_byte_address(19, 0, 2, 128, 0), 0x2000);
    equal("t8 even TBW page row", gs_texture_byte_address(19, 0, 2, 0, 64), 0x2000);

    // Odd TBW preserves the hardware/indexor multiply-before-divide behavior.
    equal("t8 odd TBW first page row", gs_texture_byte_address(19, 0, 1, 0, 64), 0x0000);
    equal("t8 odd TBW second page row", gs_texture_byte_address(19, 0, 1, 0, 128), 0x2000);
    equal("t8 TBW zero aliases vertical pages", gs_texture_byte_address(19, 0, 0, 0, 64), 0x0000);
    equal("ct32 TBW zero aliases vertical pages", gs_texture_byte_address(0, 0, 0, 0, 32), 0x0000);

    // TBP is in 256-byte units and physical addresses wrap at 4 MiB.
    equal("ct32 base plus page wraps", gs_texture_byte_address(0, 0x3fff, 1, 64, 0), 0x001f00);
    equal("t8 base plus page wraps", gs_texture_byte_address(19, 0x3fff, 2, 128, 0), 0x001f00);
    equal("max coordinate accepted", gs_texture_byte_address(0, 2, 1, 2047, 2047), 0x0be1fc);

    invalid("unsupported PSM", gs_texture_byte_address(1, 0, 1, 0, 0));
    invalid("TBP outside 14-bit field", gs_texture_byte_address(0, 0x4000, 1, 0, 0));
    invalid("TBW outside 6-bit field", gs_texture_byte_address(19, 0, 64, 0, 0));
    invalid("x outside GS coordinate", gs_texture_byte_address(19, 0, 2, 2048, 0));
    invalid("y outside GS coordinate", gs_texture_byte_address(0, 0, 2, 0, 2048));
    static_assert(noexcept(gs_texel_address(0, 0, 0, 0, 0)));
    texel("raw ct32", gs_texel_address(0, 0, 1, 8, 0), 0x100, 0, 32);
    texel("raw ct24 shares word", gs_texel_address(1, 0, 1, 8, 0), 0x100, 0, 24);
    texel("raw t8", gs_texel_address(19, 0, 2, 0, 2), 0x21, 0, 8);

    texel("t4 origin", gs_texel_address(20, 0, 2, 0, 0), 0, 0, 4);
    texel("t4 word x", gs_texel_address(20, 0, 2, 1, 0), 4, 0, 4);
    texel("t4 byte lane1", gs_texel_address(20, 0, 2, 8, 0), 1, 0, 4);
    texel("t4 byte lane2", gs_texel_address(20, 0, 2, 16, 0), 2, 0, 4);
    texel("t4 byte lane3", gs_texel_address(20, 0, 2, 24, 0), 3, 0, 4);
    texel("t4 same byte other nibble", gs_texel_address(20, 0, 2, 4, 2), 0, 4, 4);
    texel("t4 row selector", gs_texel_address(20, 0, 2, 0, 2), 0x20, 4, 4);
    texel("t4 alternating band", gs_texel_address(20, 0, 2, 0, 4), 0x60, 0, 4);
    texel("t4 block column", gs_texel_address(20, 0, 2, 32, 0), 0x200, 0, 4);
    texel("t4 block row", gs_texel_address(20, 0, 2, 0, 16), 0x100, 0, 4);
    texel("t4 page last nibble", gs_texel_address(20, 0, 2, 127, 127), 0x1fff, 4, 4);
    texel("t4 page x", gs_texel_address(20, 0, 2, 128, 0), 0x2000, 0, 4);
    texel("t4 page y", gs_texel_address(20, 0, 2, 0, 128), 0x2000, 0, 4);
    texel("t4 odd TBW first row", gs_texel_address(20, 0, 1, 0, 128), 0, 0, 4);
    texel("t4 odd TBW second row", gs_texel_address(20, 0, 1, 0, 256), 0x2000, 0, 4);
    texel("t4 odd TBW third row", gs_texel_address(20, 0, 3, 0, 384), 0x8000, 0, 4);
    texel("t4 zero TBW", gs_texel_address(20, 0, 0, 0, 128), 0, 0, 4);
    texel("t4 base plus page wraps", gs_texel_address(20, 0x3fff, 2, 128, 0), 0x1f00, 0, 4);
    texel("t4 last byte wraps", gs_texel_address(20, 0x3fff, 2, 127, 127), 0x1eff, 4, 4);

    // Each pixel in one PSMT4 page must select a distinct physical nibble.
    std::array<bool, 16384> seen{};
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        const auto a = gs_texel_address(20, 0, 2, x, y);
        if (!a || a->byte_address >= 8192 || a->bits != 4 || (a->shift != 0 && a->shift != 4)) {
            std::cerr << "t4 page lane outside its extent\n"; return 1;
        }
        const auto lane = a->byte_address * 2 + a->shift / 4;
        if (seen[lane]) { std::cerr << "t4 page aliases a nibble\n"; return 1; }
        seen[lane] = true;
    }

    for (unsigned tbp : {0u, 2u, 0x3fffu}) for (unsigned tbw : {0u, 1u, 3u, 63u})
        for (const auto xy : {std::array{0u, 0u}, std::array{63u, 31u}, std::array{64u, 32u}, std::array{2047u, 2047u}}) {
            const auto word = gs_texture_byte_address(0, tbp, tbw, xy[0], xy[1]);
            texel("ct24 word alias", gs_texel_address(1, tbp, tbw, xy[0], xy[1]), *word, 0, 24);
            texel("t8h high byte alias", gs_texel_address(27, tbp, tbw, xy[0], xy[1]), *word + 3, 0, 8);
            texel("t4hl low nibble alias", gs_texel_address(36, tbp, tbw, xy[0], xy[1]), *word + 3, 0, 4);
            texel("t4hh high nibble alias", gs_texel_address(44, tbp, tbw, xy[0], xy[1]), *word + 3, 4, 4);
        }
    texel("high byte at VRAM end", gs_texel_address(27, 0x3fff, 1, 7, 7), 0x3fffff, 0, 8);
    texel("high nibble at VRAM end", gs_texel_address(44, 0x3fff, 1, 7, 7), 0x3fffff, 4, 4);
    texel("high nibble page wraps", gs_texel_address(44, 0x3fff, 1, 64, 0), 0x1f03, 4, 4);

    for (unsigned psm : {0u, 1u, 19u, 20u, 27u, 36u, 44u}) {
        invalid_texel("raw TBP field", gs_texel_address(psm, 0x4000, 1, 0, 0));
        invalid_texel("raw TBW field", gs_texel_address(psm, 0, 64, 0, 0));
        invalid_texel("raw x field", gs_texel_address(psm, 0, 1, 2048, 0));
        invalid_texel("raw y field", gs_texel_address(psm, 0, 1, 0, 2048));
        invalid_texel("raw unsigned coordinate overflow", gs_texel_address(psm, 0, 1, 0xffffffffu, 0));
        if (!gs_texel_address(psm, 0x3fff, 63, 2047, 2047)) return 1;
        // The old byte API must keep rejecting its newly supported siblings.
        if (psm != 0 && psm != 19) invalid("byte API domain unchanged", gs_texture_byte_address(psm, 0, 1, 0, 0));
    }
    for (unsigned psm : {2u, 18u, 24u, 28u, 48u, 49u, 0xffffffffu})
        invalid_texel("unsupported raw PSM", gs_texel_address(psm, 0, 1, 0, 0));
    return 0;
}
