#include "okage/assets.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

using okage::Bytes;
using Buf = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

static void require(bool value, const char* what = "assertion") {
    if (!value) throw std::runtime_error(std::string("test assertion failed: ") + what);
}
static void put32(Buf& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = std::uint8_t(v >> (8 * i));
}
// Expect a runtime_error whose message contains `needle`.
template <class F> static void rejects(F action, const char* needle) {
    try { action(); } catch (const std::runtime_error& e) {
        require(std::string(e.what()).find(needle) != std::string::npos, needle);
        return;
    }
    throw std::runtime_error(std::string("accepted malformed data, expected: ") + needle);
}
struct Spec {
    unsigned psm = 0, cpsm = 0, levels = 1, colors = 0, w = 2, h = 3;
    std::uint32_t stale0 = 0;
};
static unsigned bits_of(unsigned psm) { return psm == 0 ? 32 : psm == 1 ? 24 : psm == 20 ? 4 : 8; }
static Buf build(const Spec& s) {
    const bool indexed = s.psm >= 16;
    const std::size_t bpc = s.cpsm == 2 ? 2 : 4;
    const std::size_t palette = indexed ? 16 + s.levels * s.colors * bpc : 0;
    const std::size_t payload = (std::size_t(s.w) * s.h * bits_of(s.psm) + 7) / 8;
    Buf b(16 + palette + 16 + payload, 0);
    put32(b, 0, s.psm << 20);
    put32(b, 4, s.cpsm << 19);
    put32(b, 8, 0x11111111);
    put32(b, 12, 0x22222222);
    if (indexed) {
        put32(b, 16, std::uint32_t(palette));
        put32(b, 20, 0x33333333);
        put32(b, 24, s.levels);
        put32(b, 28, s.colors);
        for (std::size_t i = 32; i < 16 + palette; ++i) b[i] = std::uint8_t(i * 7 + 1);
    }
    const std::size_t ih = 16 + palette;
    put32(b, ih, s.stale0);
    put32(b, ih + 4, 0x44444444);
    put32(b, ih + 8, s.h);
    put32(b, ih + 12, s.w);
    for (std::size_t i = ih + 16; i < b.size(); ++i) b[i] = std::uint8_t(i * 13 + 5);
    return b;
}

static void formats() {
    struct Row { unsigned psm, cpsm, colors; okage::XimUploadStatus status; };
    using S = okage::XimUploadStatus;
    const Row rows[] = {
        {0, 0, 0, S::Supported}, {1, 0, 0, S::UnsupportedImageFormat},
        {19, 0, 16, S::Supported}, {19, 0, 256, S::Supported},
        {19, 0, 32, S::UnsupportedPaletteShape}, {19, 2, 256, S::UnsupportedPaletteFormat},
        {20, 0, 16, S::UnsupportedImageFormat}, {20, 2, 16, S::UnsupportedImageFormat},
        {27, 0, 256, S::UnsupportedImageFormat},
    };
    for (const auto& r : rows) {
        for (unsigned levels : {1u, r.psm >= 16 ? 3u : 1u}) {
            Spec s; s.psm = r.psm; s.cpsm = r.cpsm; s.colors = r.colors; s.levels = levels; s.w = 5; s.h = 3;
            s.stale0 = 0xdeadbeef;
            const auto b = build(s);
            const auto l = okage::parse_xim_layout(b);
            require(l.pixel_format == r.psm && l.palette_format == r.cpsm, "formats");
            require(l.width == 5 && l.height == 3, "dims from +12/+8, not +0");
            require(l.indexed == (r.psm >= 16), "indexed");
            require(l.bits_per_pixel == bits_of(r.psm), "bpp");
            require(l.header_words[2] == 0x11111111 && l.header_words[3] == 0x22222222, "header words");
            require(l.image_header_words[0] == 0xdeadbeef && l.image_header_words[1] == 0x44444444, "image words verbatim");
            require(l.image_payload_size == (5 * 3 * bits_of(r.psm) + 7) / 8, "payload size");
            require(l.image_payload_offset + l.image_payload_size == b.size(), "payload end");
            if (l.indexed) {
                require(l.palette_header_offset == 16 && l.palette_data_offset == 32, "palette offsets");
                require(l.palette_level_count == levels && l.palette_color_count == r.colors, "palette counts");
                require(l.palette_unused_word == 0x33333333, "unused word");
                require(l.image_header_offset == 16 + l.palette_block_bytes, "image header offset");
                require(l.palette_level_stride == r.colors * (r.cpsm == 2 ? 2 : 4), "stride");
                require(l.palette_levels_extent == levels * l.palette_level_stride, "extent");
            } else {
                require(l.image_header_offset == 16 && l.palette_level_count == 0, "direct");
            }
            const auto u = okage::xim_upload_support(l);
            require(u.status == r.status && !u.reason.empty(), "upload status");
            okage::OwnedRawXim o(b);
            require(o.image().size() == l.image_payload_size, "image span");
            require(std::equal(o.image().begin(), o.image().end(), b.begin() + l.image_payload_offset), "image bytes");
            for (unsigned i = 0; i < l.palette_level_count; ++i) {
                const auto level = o.palette_level(i);
                require(std::equal(level.begin(), level.end(),
                                   b.begin() + l.palette_data_offset + i * l.palette_level_stride), "level bytes");
            }
            rejects([&] { (void)o.palette_level(l.palette_level_count); }, "level index");
        }
    }
}
static void truncation() {
    for (const Spec s : {Spec{0, 0, 1, 0, 2, 2}, Spec{19, 0, 1, 16, 2, 2}, Spec{20, 2, 2, 16, 3, 1}}) {
        const auto b = build(s);
        okage::parse_xim_layout(b);
        for (std::size_t n = 0; n < b.size(); ++n) {
            // Exactly-sized heap block so ASan flags any over-read.
            Buf exact(b.begin(), b.begin() + n);
            bool threw = false;
            try { okage::parse_xim_layout(exact); } catch (const std::runtime_error&) { threw = true; }
            require(threw, "every prefix must throw");
        }
    }
}
static void malformed() {
    Spec base; base.psm = 19; base.colors = 16;
    const auto b = build(base);
    auto edit = [&](std::size_t at, std::uint32_t v) { auto c = b; put32(c, at, v); return c; };
    rejects([&] { okage::parse_xim_layout(Buf(15)); }, "main header");
    rejects([&] { okage::parse_xim_layout(edit(0, 2u << 20)); }, "unsupported pixel format");
    rejects([&] { okage::parse_xim_layout(edit(0, 63u << 20)); }, "unsupported pixel format");
    rejects([&] { okage::parse_xim_layout(edit(4, 1u << 19)); }, "unsupported palette format");
    rejects([&] { okage::parse_xim_layout(edit(24, 0)); }, "level count is zero");
    rejects([&] { okage::parse_xim_layout(edit(28, 0)); }, "color count");
    rejects([&] { okage::parse_xim_layout(edit(28, 257)); }, "color count");
    rejects([&] { okage::parse_xim_layout(edit(28, 0xffffffffu)); }, "color count");
    rejects([&] { okage::parse_xim_layout(edit(16, 8)); }, "smaller than its header");
    rejects([&] { okage::parse_xim_layout(edit(16, 0xffffffffu)); }, "block extent");
    rejects([&] { okage::parse_xim_layout(edit(24, 0xffffffffu)); }, "block extent");
    rejects([&] { okage::parse_xim_layout(edit(24, 2)); }, "block extent");
    {
        auto c = b; put32(c, 24, 1000); put32(c, 16, 16 + 1000 * 16 * 4);
        rejects([&] { okage::parse_xim_layout(c); }, "palette extent outside buffer");
    }
    const std::size_t ih = 16 + 16 + 16 * 4;
    rejects([&] { okage::parse_xim_layout(edit(ih + 12, 0)); }, "zero image dimension");
    rejects([&] { okage::parse_xim_layout(edit(ih + 8, 0)); }, "zero image dimension");
    rejects([&] { okage::parse_xim_layout(edit(ih + 12, 4097)); }, "exceeds 4096");
    rejects([&] { okage::parse_xim_layout(edit(ih + 8, 0xffffffffu)); }, "exceeds 4096");
    rejects([&] { okage::parse_xim_layout(edit(ih + 12, 4096)); }, "payload size");
    auto trail = b; trail.push_back(0);
    rejects([&] { okage::parse_xim_layout(trail); }, "payload size");
    Spec direct; direct.psm = 0;
    auto d = build(direct); put32(d, 16 + 12, 4096); put32(d, 16 + 8, 4096);
    rejects([&] { okage::parse_xim_layout(d); }, "payload size");
    // Stale +0 field must not matter.
    for (std::uint32_t v : {0u, 1u, 0xffffffffu}) {
        const auto l = okage::parse_xim_layout(edit(ih, v));
        require(l.width == 2 && l.height == 3 && l.image_header_words[0] == v, "stale +0");
    }
    Spec rect; rect.psm = 0; rect.w = 7; rect.h = 2;
    const auto l = okage::parse_xim_layout(build(rect));
    require(l.width == 7 && l.height == 2, "width/height orientation");
}
static void lifetime() {
    Spec s; s.psm = 19; s.colors = 256; s.levels = 2; s.w = 4; s.h = 4;
    Buf source = build(s);
    const Buf expected = source;
    auto owned = std::make_unique<okage::OwnedRawXim>(source);
    {
        okage::RawXimView view(source);
        require(view.layout().palette_level_count == 2, "view parse");
        require(std::equal(view.image().begin(), view.image().end(),
                           expected.begin() + view.layout().image_payload_offset), "view image");
    }
    std::fill(source.begin(), source.end(), 0xaa);
    source.clear(); source.shrink_to_fit();
    okage::OwnedRawXim moved(std::move(*owned));
    owned.reset();
    okage::OwnedRawXim copied(moved);
    okage::OwnedRawXim assigned(moved);
    assigned = copied;
    for (const okage::OwnedRawXim* x : {&moved, &copied, &assigned}) {
        require(Buf(x->bytes().begin(), x->bytes().end()) == expected, "bytes equal after destroy/move/copy");
        const auto& l = x->layout();
        const auto image = x->image(), level1 = x->palette_level(1);
        require(std::equal(image.begin(), image.end(), expected.begin() + l.image_payload_offset), "image");
        require(std::equal(level1.begin(), level1.end(),
                           expected.begin() + l.palette_data_offset + l.palette_level_stride), "level 1");
    }
    require(moved.bytes().data() != copied.bytes().data(), "copy owns separate storage");
    okage::OwnedRawXim thief(std::move(moved));
    require(moved.bytes().empty() && moved.image().empty(), "moved-from is empty");
    require(Buf(thief.bytes().begin(), thief.bytes().end()) == expected, "move preserves");
}
static Buf read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Buf(std::istreambuf_iterator<char>(in), {});
}
static void corpus() {
    const char* root_env = std::getenv("OKAGE_LOCAL_ROOT");
    const fs::path root = fs::path(root_env ? root_env : "local") / "xpf-decoded";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { std::cout << "xim corpus absent; skipping corpus comparison\n"; return; }
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ".xim") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    const std::size_t step = std::max<std::size_t>(1, files.size() / 600);
    std::size_t compared = 0;
    for (std::size_t i = 0; i < files.size(); i += step) {
        const auto data = read_file(files[i]);
        okage::Texture t;
        try { t = okage::decode_xim(data); } catch (const std::runtime_error&) { continue; }
        const auto l = okage::parse_xim_layout(data);  // decoder-accepted => parser-accepted
        require(l.width == t.width && l.height == t.height, "corpus dims");
        require(l.pixel_format == t.pixel_format && l.palette_format == t.palette_format, "corpus formats");
        require(l.palette_level_count == (l.indexed ? 1u : 0u), "corpus single level");
        ++compared;
    }
    std::cout << "compared " << compared << " corpus XIMs\n";
}
int main() {
    try {
        formats(); truncation(); malformed(); lifetime(); corpus();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "xim raw tests passed\n";
}
