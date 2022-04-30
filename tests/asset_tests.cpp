#include "okage/assets.hpp"
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("test assertion failed"); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("accepted malformed data");
}
int main() {
    try {
        // Hand-encoded: literal A, distance-1 match of two bytes, end marker.
        const std::vector<std::uint8_t> packed{0,0,0,3,0x48,'A',0xff,0};
        const auto result = okage::decompress_xpf(packed);
        require(result.bytes == std::vector<std::uint8_t>({'A','A','A'}));
        require(result.consumed == packed.size());
        const std::vector<std::uint8_t> empty{0,0,0,0,0x80,0};
        require(okage::decompress_xpf(empty).bytes.empty());
        for (std::size_t n = 0; n < packed.size(); ++n)
            rejects([&] { okage::decompress_xpf(okage::Bytes(packed).first(n)); });
        rejects([&] { okage::decompress_xpf(packed, 2); });
        auto bad = packed; bad[6] = 0xfe; // distance 2 before two bytes exist
        rejects([&] { okage::decompress_xpf(bad); });
        bad = packed; bad[3] = 2;
        rejects([&] { okage::decompress_xpf(bad); });
        std::vector<std::uint8_t> archive(48, 0);
        archive[0]='X'; archive[1]='P'; archive[2]='F'; archive[3]='X';
        archive[4]=48; archive[8]=1; archive[16]='a'; archive[44]=packed.size();
        archive.insert(archive.end(), packed.begin(), packed.end());
        require(okage::read_xpf(archive).front().offset == 48);
        bad = archive; bad[16]='/';
        rejects([&] { okage::read_xpf(bad); });
        bad = archive; bad[44]=255;
        rejects([&] { okage::read_xpf(bad); });
        bad = archive; bad[4]=16;
        rejects([&] { okage::read_xpf(bad); });
        // Asymmetric 3x2 direct-color image guards the unusual height/width order.
        std::vector<std::uint8_t> xim(32 + 18, 0);
        xim[2]=0x10; xim[24]=2; xim[28]=3;
        for (unsigned i=0; i<18; ++i) xim[32+i]=i;
        auto texture = okage::decode_xim(xim);
        require(texture.width == 3 && texture.height == 2);
        require(texture.rgba[20] == 15 && texture.rgba[23] == 255);
        xim.pop_back(); rejects([&] { okage::decode_xim(xim); });
        // Two 4-bit pixels share a byte; low nibble is the first pixel.
        xim.assign(16+80+16+1,0); xim[2]=0x40; xim[3]=1;
        xim[16]=80; xim[24]=1; xim[28]=16;
        xim[32+4]=7; xim[32+7]=128;
        xim[32+8]=9; xim[32+11]=64;
        xim[104]=1; xim[108]=2; xim[112]=0x21;
        texture=okage::decode_xim(xim);
        require(texture.rgba == std::vector<std::uint8_t>({7,0,0,255,9,0,0,128}));
        xim.assign(16+1040+16+2,0);xim[2]=0x30;xim[3]=1;
        xim[16]=0x10;xim[17]=4;xim[24]=1;xim[29]=1;
        xim[32+4]=17;xim[32+7]=128;xim[32+255*4+1]=91;xim[32+255*4+3]=64;
        xim[1064]=1;xim[1068]=2;xim[1072]=1;xim[1073]=255;
        const auto ordinary=okage::decode_xim(xim);xim[2]=0xb0;
        const auto high=okage::decode_xim(xim);
        require(high.pixel_format==27&&high.rgba==ordinary.rgba);
        require(high.rgba==std::vector<std::uint8_t>({17,0,0,255,0,91,0,128}));
        std::cout << "Native asset boundary tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
