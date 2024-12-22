#include "okage/gs_texture_address.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
using okage::gs_texture_byte_address;

void equal(std::string_view label, std::optional<std::uint32_t> actual, std::uint32_t expected) {
    if (!actual || *actual != expected) {
        std::cerr << label << ": expected 0x" << std::hex << expected << ", got ";
        if (actual) std::cerr << "0x" << *actual;
        else std::cerr << "invalid";
        std::cerr << '\n';
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
    return 0;
}
