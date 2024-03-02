#include "okage/pcm_stream.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F&& f, const char* message) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
void u32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.at(at + i) = std::uint8_t(value >> (i * 8));
}
void u16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t value) {
    b.at(at) = std::uint8_t(value);
    b.at(at + 1) = std::uint8_t(value >> 8);
}
void tag(std::vector<std::uint8_t>& b, std::size_t at, const char* value) {
    std::memcpy(b.data() + at, value, 4);
}
struct Fixture {
    std::vector<std::uint8_t> xwh = std::vector<std::uint8_t>(112);
    std::vector<std::uint8_t> xwb = std::vector<std::uint8_t>(16);
    std::array<std::int16_t, okage::TitlePcmStream::gain_table_size> gains{};
    Fixture() {
        tag(xwh, 0, "RXWS"); u32(xwh, 4, 96); u32(xwh, 8, 0x200);
        tag(xwh, 16, "FORM"); u32(xwh, 20, 32); u32(xwh, 24, 0x100);
        u32(xwh, 32, 1);
        xwh[36] = 1; xwh[37] = 0x1c; xwh[38] = 3; xwh[39] = 0x80;
        xwh[40] = 127; xwh[41] = 127; xwh[45] = 2;
        u16(xwh, 46, 48000); u32(xwh, 52, 0); u32(xwh, 56, 16); u32(xwh, 60, 8);
        tag(xwh, 64, "FTXT"); u32(xwh, 68, 32); u32(xwh, 72, 0x100);
        u32(xwh, 80, 1); u32(xwh, 84, 8);
        std::memcpy(xwh.data() + 88, "thema_1.aif", 12);
        u16(xwb, 0, 1000); u16(xwb, 2, std::uint16_t(-1000));
        u16(xwb, 4, 2000); u16(xwb, 6, std::uint16_t(-2000));
        u16(xwb, 8, 3000); u16(xwb, 10, std::uint16_t(-3000));
        u16(xwb, 12, 4000); u16(xwb, 14, std::uint16_t(-4000));
        for (int i = 0; i < 257; ++i) gains[i] = std::int16_t((i - 128) * 32);
        // Pinned IOP table's title gain127 maps to Q12 unity, as does128.
        gains[255] = gains[256] = 4096;
    }
};
}

int main() {
    try {
        Fixture f;
        okage::TitlePcmStream stream(f.xwh, f.xwb, f.gains);
        check(stream.frame_count() == 4 && stream.loop_start_frame() == 2 &&
              stream.cursor_frame() == 0, "parsed frame and loop bounds");
        check(stream.authored_left_gain() == 127 && stream.authored_right_gain() == 127,
              "authored signed gains");

        std::array<std::int16_t, 8> one_block{};
        stream.mix(one_block);
        check(one_block == std::array<std::int16_t, 8>{1000,-1000,2000,-2000,
                                                        3000,-3000,4000,-4000},
              "signed little-endian stereo and title unity gain");
        check(stream.cursor_frame() == 4, "end is exclusive until next sample request");
        std::array<std::int16_t, 4> loop_tail{};
        stream.mix(loop_tail);
        check(loop_tail == std::array<std::int16_t, 4>{3000,-3000,4000,-4000} &&
              stream.cursor_frame() == 4, "loop wraps at arbitrary render boundary");

        Fixture partitioned;
        okage::TitlePcmStream whole(partitioned.xwh, partitioned.xwb, partitioned.gains);
        okage::TitlePcmStream pieces(partitioned.xwh, partitioned.xwb, partitioned.gains);
        std::array<std::int16_t, 12> whole_out{}, pieces_out{};
        whole.mix(whole_out);
        pieces.mix(std::span<std::int16_t>(pieces_out).first(4));
        pieces.mix(std::span<std::int16_t>(pieces_out).subspan(4));
        check(whole_out == pieces_out, "block partitioning preserves cursor/mix");
        check(whole_out == std::array<std::int16_t, 12>{1000,-1000,2000,-2000,
              3000,-3000,4000,-4000,3000,-3000,4000,-4000}, "exclusive end then loop");

        Fixture nonlinear;
        nonlinear.gains[255] = 2048;
        okage::TitlePcmStream nonlinear_stream(nonlinear.xwh, nonlinear.xwb, nonlinear.gains);
        std::array<std::int16_t, 2> nonlinear_out{};
        nonlinear_stream.mix(nonlinear_out);
        check(nonlinear_out == std::array<std::int16_t, 2>{500,-500}, "caller Q12 table lookup");

        Fixture saturation;
        okage::TitlePcmStream saturating(saturation.xwh, saturation.xwb, saturation.gains);
        std::array<std::int16_t, 2> accumulated{32700, -32700};
        saturating.mix(accumulated);
        check(accumulated == std::array<std::int16_t, 2>{32767, -32768},
              "additive saturation");

        Fixture signed_extremes;
        u16(signed_extremes.xwb, 0, 0x7fff); u16(signed_extremes.xwb, 2, 0x8000);
        signed_extremes.xwh[40] = signed_extremes.xwh[41] = 0x80; // Authored gain -128.
        okage::TitlePcmStream polarity(signed_extremes.xwh, signed_extremes.xwb,
                                       signed_extremes.gains);
        std::array<std::int16_t, 2> inverted{};
        polarity.mix(inverted);
        check(inverted == std::array<std::int16_t, 2>{-32767, 32767},
              "signed PCM extremes and negative authored gain");

        Fixture rounding;
        rounding.gains[128] = -1;
        okage::TitlePcmStream negative_q12(rounding.xwh, rounding.xwb, rounding.gains);
        std::array<std::int16_t, 2> rounded{};
        negative_q12.mix(rounded, {1,1,1,1});
        check(rounded == std::array<std::int16_t, 2>{-1,0},
              "negative Q12 uses source arithmetic-shift rounding");
        rejects([&] { std::array<std::int16_t, 1> odd{}; saturating.mix(odd); },
                "odd stereo destination rejected");
        rejects([&] { std::array<std::int16_t, 2> out{};
                      saturating.mix(out, {129,128,128,128}); }, "runtime gain range");

        // Constructor owns the bounded sample bytes and gain table.
        Fixture owned;
        auto owned_xwb = owned.xwb;
        auto owned_gains = owned.gains;
        okage::TitlePcmStream owner(owned.xwh, owned_xwb, owned_gains);
        owned_xwb[0] = owned_xwb[1] = owned_xwb[2] = owned_xwb[3] = 0;
        owned_gains[255] = 0;
        std::array<std::int16_t, 2> owned_out{};
        owner.mix(owned_out);
        check(owned_out == std::array<std::int16_t, 2>{1000,-1000}, "owned PCM/table data");

        for (std::size_t n = 0; n < f.xwh.size(); ++n)
            rejects([&] { okage::TitlePcmStream truncated(okage::Bytes(f.xwh).first(n),
                         f.xwb, f.gains); }, "truncated RXWS rejected");
        auto malformed = f.xwh; tag(malformed, 16, "MARK");
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "MARK rejected");
        malformed = f.xwh; tag(malformed, 64, "BRCH");
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "BRCH rejected");
        malformed = f.xwh; malformed[36] = 0;
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "other codec rejected");
        malformed = f.xwh; malformed[45] = 1;
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "mono mode rejected");
        malformed = f.xwh; u16(malformed, 46, 44100);
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "other rate rejected");
        malformed = f.xwh; u32(malformed, 60, 16);
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "loop at end rejected");
        malformed = f.xwh; u32(malformed, 56, 18);
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "unaligned end rejected");
        malformed = f.xwh; u32(malformed, 56, 20);
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "body overrun rejected");
        malformed = f.xwh; u32(malformed, 60, 0);
        rejects([&] { okage::TitlePcmStream bad(malformed, f.xwb, f.gains); }, "loop at base rejected");
        auto bad_gain_table = f.gains; bad_gain_table[0] = -4097;
        rejects([&] { okage::TitlePcmStream bad(f.xwh, f.xwb, bad_gain_table); },
                "out-of-Q12 lookup rejected");
        auto short_body = std::span<const std::uint8_t>(f.xwb).first(15);
        rejects([&] { okage::TitlePcmStream bad(f.xwh, short_body, f.gains); },
                "body bound rejected");

        std::cout << "Title format-1 PCM metadata, cursor, Q12 mix and bounds passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
