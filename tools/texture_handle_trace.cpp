#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "okage/assets.hpp"
#include "okage/texture_handles.hpp"
#include "okage/texture_memory.hpp"
#include "okage/texture_transfer.hpp"

using namespace okage;

namespace {

[[noreturn]] void die(const std::string& message) {
    std::cerr << "okage-texture-handle-trace: " << message << "\n";
    std::exit(2);
}

enum class Op { Cache, Lower, Reserve, Image, Xim, Name, Lookup, Retire, Palette, Bind, Xmd, Filler, Free, Retain };

struct Line {
    Op op{};
    std::string text;
    std::vector<std::string> args;  // raw tokens after the op name
    std::int64_t n[5]{};
    int label{-1};   // hK / fK operand index
    int label2{-1};  // pK operand index (bind), -1 = null
    bool has_name{};
    std::string str;
};

std::int64_t parse_number(const std::string& tok, std::int64_t lo, std::int64_t hi, bool allow_negative,
                          const std::string& what, int lineno) {
    const std::string where = "line " + std::to_string(lineno) + ": ";
    std::size_t i = 0;
    bool neg = false;
    if (allow_negative && !tok.empty() && tok[0] == '-') { neg = true; i = 1; }
    if (i >= tok.size()) die(where + "malformed number for " + what + ": '" + tok + "'");
    if (tok.size() - i > 1 && tok[i] == '0') die(where + "malformed number (leading zero) for " + what + ": '" + tok + "'");
    if (tok.size() - i > 10) die(where + "malformed number (too long) for " + what + ": '" + tok + "'");
    std::int64_t v = 0;
    for (; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9') die(where + "malformed number for " + what + ": '" + tok + "'");
        v = v * 10 + (tok[i] - '0');
    }
    if (neg) v = -v;
    if (v < lo || v > hi)
        die(where + what + " out of range [" + std::to_string(lo) + "," + std::to_string(hi) + "]: " + tok);
    return v;
}

// Label like h12; `defined` = labels of that kind defined so far.
int parse_label(const std::string& tok, char kind, int defined, int lineno) {
    const std::string where = "line " + std::to_string(lineno) + ": ";
    if (tok.size() < 2 || tok[0] != kind) die(where + "bad label '" + tok + "' (expected " + kind + "N)");
    const auto idx = parse_number(tok.substr(1), 0, 1 << 20, false, "label index", lineno);
    if (idx >= defined) die(where + "label " + tok + " not defined yet (" + std::to_string(defined) + " defined)");
    return static_cast<int>(idx);
}

std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<Line> parse_script(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) die("cannot read script: " + path);
    std::vector<Line> ops;
    int hn = 0, pn = 0, fn = 0;
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        const std::string text = trim(raw);
        if (text.empty() || text[0] == '#') continue;
        for (unsigned char c : text)
            if ((c < 0x20 && c != '\t') || c >= 0x7f)
                die("line " + std::to_string(lineno) + ": non-printable or non-ASCII character");
        const auto tok = tokens(text);
        const std::string where = "line " + std::to_string(lineno) + ": ";
        Line l;
        l.text = text;
        l.args.assign(tok.begin() + 1, tok.end());
        const std::string& name = tok[0];
        auto arity = [&](std::size_t want) {
            if (l.args.size() != want)
                die(where + "'" + name + "' takes " + std::to_string(want) + " operand(s), got " + std::to_string(l.args.size()));
        };
        auto psm_alloc = [&](const std::string& t) {
            const auto psm = parse_number(t, 0, 255, false, "PSM", lineno);
            if (!is_supported_gs_texture_format(static_cast<std::uint32_t>(psm)))
                die(where + "PSM " + t + " is not an allocator pixel storage format");
            return psm;
        };
        auto psm_image = [&](const std::string& t) {
            const auto psm = parse_number(t, 0, 255, false, "PSM", lineno);
            if (psm != 0 && psm != 19) die(where + "PSM " + t + " unsupported for image/xim (native supports 0 and 19)");
            return psm;
        };
        if (name == "cache" || name == "lower") {
            l.op = name == "cache" ? Op::Cache : Op::Lower;
            arity(1);
            if (l.args[0] != "0" && l.args[0] != "1") die(where + "'" + name + "' operand must be 0 or 1");
            l.n[0] = l.args[0] == "1";
        } else if (name == "reserve") {
            l.op = Op::Reserve;
            arity(1);
            l.n[0] = parse_number(l.args[0], 1, 8192, false, "reserve count", lineno);
        } else if (name == "image") {
            l.op = Op::Image;
            arity(3);
            l.n[0] = psm_image(l.args[0]);
            l.n[1] = parse_number(l.args[1], 1, 2048, false, "width", lineno);
            l.n[2] = parse_number(l.args[2], 1, 2048, false, "height", lineno);
            ++hn;
        } else if (name == "xim") {
            l.op = Op::Xim;
            arity(5);
            l.n[0] = psm_image(l.args[0]);
            l.n[1] = parse_number(l.args[1], 1, 2048, false, "width", lineno);
            l.n[2] = parse_number(l.args[2], 1, 2048, false, "height", lineno);
            l.n[3] = parse_number(l.args[3], 0, 64, false, "levels", lineno);
            l.n[4] = parse_number(l.args[4], 0, 256, false, "colors", lineno);
            if (l.n[0] == 19 && l.n[3] < 1)
                die(where + "PSM 19 XIM needs at least one palette level");
            if (l.n[0] == 0 && (l.n[3] != 0 || l.n[4] != 0))
                die(where + "PSM 0 XIM has no palette: levels and colors must be 0");
            if (l.n[0] == 19 && l.n[4] != 16 && l.n[4] != 256)
                die(where + "PSM 19 XIM colors must be 16 or 256 (native upload support)");
            ++hn;
        } else if (name == "name") {
            l.op = Op::Name;
            arity(2);
            l.label = parse_label(l.args[0], 'h', hn, lineno);
            l.has_name = l.args[1] != "-";
            l.str = l.args[1];
        } else if (name == "lookup") {
            l.op = Op::Lookup;
            arity(1);
            l.str = l.args[0];
            ++hn;
        } else if (name == "retire") {
            l.op = Op::Retire;
            arity(1);
            l.label = parse_label(l.args[0], 'h', hn, lineno);
        } else if (name == "palette") {
            l.op = Op::Palette;
            arity(1);
            l.n[0] = parse_number(l.args[0], 1, 512, false, "palette colors", lineno);
            ++pn;
        } else if (name == "bind") {
            l.op = Op::Bind;
            arity(3);
            l.label = parse_label(l.args[0], 'h', hn, lineno);
            l.n[0] = parse_number(l.args[1], 0, 1023, false, "level", lineno);
            l.label2 = l.args[2] == "null" ? -1 : parse_label(l.args[2], 'p', pn, lineno);
        } else if (name == "xmd") {
            l.op = Op::Xmd;
        } else if (name == "filler") {
            l.op = Op::Filler;
            arity(4);
            l.n[0] = parse_number(l.args[0], 0, 3, false, "class", lineno);
            l.n[1] = psm_alloc(l.args[1]);
            l.n[2] = parse_number(l.args[2], 1, 65535, false, "width", lineno);
            l.n[3] = parse_number(l.args[3], 1, 65535, false, "height", lineno);
            ++fn;
        } else if (name == "free") {
            l.op = Op::Free;
            arity(1);
            l.label = parse_label(l.args[0], 'f', fn, lineno);
        } else if (name == "retain") {
            l.op = Op::Retain;
            arity(2);
            l.label = parse_label(l.args[0], 'f', fn, lineno);
            l.n[0] = parse_number(l.args[1], -((1ll << 31) - 1), (1ll << 31) - 1, true, "delta", lineno);
        } else {
            die(where + "unknown op '" + name + "'");
        }
        ops.push_back(std::move(l));
    }
    return ops;
}

// Synthetic payload rule of the spec.
std::uint8_t synth(std::uint64_t k, std::uint64_t seed) {
    unsigned v = static_cast<unsigned>((k * 167 + seed * 29) % 250 + 1);
    return static_cast<std::uint8_t>(v >= 165 ? v + 1 : v);
}
std::vector<std::uint8_t> synth_bytes(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t k = 0; k < n; ++k) v[k] = synth(k, seed);
    return v;
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int k = 0; k < 4; ++k) v[at + k] = static_cast<std::uint8_t>(x >> (8 * k));
}
// Image payload seed S, palette level seed S+1000+level.
std::vector<std::uint8_t> build_xim(std::uint32_t psm, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t levels, std::uint32_t colors, std::uint64_t seed) {
    const bool indexed = psm == 19;
    const std::size_t pal = indexed ? 16 + std::size_t(levels) * colors * 4 : 0;
    const std::size_t bytes = psm == 0 ? std::size_t(w) * h * 4 : std::size_t(w) * h;
    std::vector<std::uint8_t> v(16 + pal + 16 + bytes, 0);
    put32(v, 0, psm << 20);
    if (indexed) {
        put32(v, 0x10, static_cast<std::uint32_t>(pal));
        put32(v, 0x18, levels);
        put32(v, 0x1c, colors);
        for (std::uint32_t lv = 0; lv < levels; ++lv)
            for (std::size_t i = 0; i < std::size_t(colors) * 4; ++i)
                v[0x20 + std::size_t(lv) * colors * 4 + i] = synth(i, seed + 1000 + lv);
    }
    const auto ih = 16 + pal;
    put32(v, ih + 8, h);
    put32(v, ih + 12, w);
    for (std::size_t i = 0; i < bytes; ++i) v[ih + 16 + i] = synth(i, seed);
    return v;
}

std::string hex(const unsigned char* d, std::size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}
std::string sha256_hex(const std::uint8_t* d, std::size_t n) {
    unsigned char dg[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d, static_cast<CC_LONG>(n), dg);
    return hex(dg, sizeof dg);
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += char(c);
    }
    return out + "\"";
}

struct Tracer {
    TextureHandleOwner& owner;
    std::map<int, int> chain_len;
    std::vector<std::string> requests;

    Ps2TextureMemory& mem() { return owner.memory(); }

    std::string state_json(int op, const std::string& text, const std::string& result) {
        std::ostringstream o;
        o << "{\"op\":" << op << ",\"text\":" << json_string(text) << ",\"result\":" << result;
        std::map<int, int> first_handle;
        o << ",\"handles\":[";
        bool first = true;
        for (int h = 0; h < TextureHandleOwner::handle_capacity; ++h) {
            if (!owner.handle_live(h)) continue;
            const int r = owner.record_of(h);
            o << (first ? "" : ",") << "[" << h << "," << r << "]";
            first = false;
            first_handle.emplace(r, h);
        }
        o << "],\"records\":[";
        first = true;
        for (int r = 0; r < TextureHandleOwner::record_capacity; ++r) {
            if (owner.record_handle_count(r) == 0) { chain_len.erase(r); continue; }
            const auto it = first_handle.find(r);
            if (it == first_handle.end()) die("internal: live record " + std::to_string(r) + " has no handle");
            const int h = it->second;
            auto id_of = [](const OptDescriptor& d) { return d ? std::int64_t(d->id) : -1; };
            o << (first ? "" : ",") << "{\"r\":" << r << ",\"image\":" << id_of(owner.image(h))
              << ",\"pal0\":" << id_of(owner.palette(h, 0)) << ",\"name\":" << json_string(owner.record_name(r))
              << ",\"hash_slot\":" << owner.record_hash_slot(r) << ",\"handles\":" << owner.record_handle_count(r)
              << ",\"extra\":[";
            const int links = chain_len.count(r) ? chain_len[r] : 0;
            for (int lv = 1; lv <= links; ++lv) o << (lv > 1 ? "," : "") << id_of(owner.palette(h, lv));
            o << "]}";
            first = false;
        }
        o << "],\"hash\":[";
        first = true;
        for (const auto& [slot, value] : owner.hash_entries()) {
            o << (first ? "" : ",") << "[" << slot << "," << value << "]";
            first = false;
        }
        o << "],\"live_handles\":" << owner.live_handle_count() << ",\"reserved\":[";
        first = true;
        for (int id : owner.reserved_handles()) { o << (first ? "" : ",") << id; first = false; }
        o << "],\"free_records\":" << owner.free_record_count() << ",\"free_extras\":" << owner.free_extra_count()
          << ",\"descriptors\":[";
        first = true;
        for (TextureAllocationId id = 0; id < 4096; ++id) {
            const auto a = mem().allocation(id);
            if (!a) continue;
            o << (first ? "" : ",") << "[" << id << "," << a->references << "]";
            first = false;
        }
        const auto occ = mem().occupancy_bytes();
        o << "],\"vram\":{\"available\":" << mem().available_count() << ",\"cursors\":[";
        first = true;
        for (auto c : mem().search_cursors()) { o << (first ? "" : ",") << c; first = false; }
        o << "],\"occupancy_sha256\":\"" << sha256_hex(occ.data(), occ.size()) << "\"";
        CC_SHA256_CTX ctx;
        CC_SHA256_Init(&ctx);
        std::vector<std::uint8_t> buf;
        buf.reserve(5 * 4096 + 5);
        std::uint64_t touched = 0;
        for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a) {
            const auto v = mem().observe_byte(a).value;
            if (!v) continue;
            ++touched;
            for (int k = 0; k < 4; ++k) buf.push_back(static_cast<std::uint8_t>(a >> (8 * k)));
            buf.push_back(*v);
            if (buf.size() >= 5 * 4096) { CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(buf.size())); buf.clear(); }
        }
        if (!buf.empty()) CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(buf.size()));
        unsigned char dg[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(dg, &ctx);
        o << ",\"touched\":" << touched << ",\"touched_sha256\":\"" << hex(dg, sizeof dg) << "\"},\"file_requests\":[";
        first = true;
        for (const auto& r : requests) { o << (first ? "" : ",") << json_string(r); first = false; }
        o << "]}";
        return o.str();
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) die("usage: okage-texture-handle-trace SCRIPT TRACE.jsonl");
    const auto ops = parse_script(argv[1]);
    const int fd = ::open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) die(std::string("refusing to overwrite or cannot create trace file: ") + argv[2]);
    ::close(fd);
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    if (!out) die("cannot write trace file");

    auto mem = std::make_unique<Ps2TextureMemory>();
    mem->initialize_empty_allocator();
    TextureHandleOwner owner(*mem);
    Tracer tr{owner, {}, {}};
    std::vector<int> hl;
    std::vector<OptDescriptor> pl, fl;
    bool cache_on = false;

    out << tr.state_json(-1, "init", "null") << "\n";
    out.flush();

    int opi = 0;
    try {
        for (const auto& l : ops) {
            tr.requests.clear();
            std::string result = "null";
            const auto seed = static_cast<std::uint64_t>(opi);
            const auto extras_before = owner.free_extra_count();
            switch (l.op) {
                case Op::Cache:
                    if (l.n[0]) { owner.enable_name_cache(); cache_on = true; }
                    else if (cache_on)
                        die("op " + std::to_string(opi) + ": 'cache 0' after 'cache 1' is not supported by the native API (the switch only turns on)");
                    break;
                case Op::Lower: owner.set_lower_case_file_names(l.n[0] != 0); break;
                case Op::Reserve: result = std::to_string(owner.reserve_handles(static_cast<int>(l.n[0]))); break;
                case Op::Image: {
                    const auto w = static_cast<std::uint16_t>(l.n[1]), h = static_cast<std::uint16_t>(l.n[2]);
                    const auto px = synth_bytes(std::size_t(w) * h * (l.n[0] == 0 ? 4 : 1), seed);
                    const int hd = owner.create_image(static_cast<GsPixelStorage>(l.n[0]), w, h, px);
                    hl.push_back(hd);
                    result = std::to_string(hd);
                    if (hd >= 0) tr.chain_len[owner.record_of(hd)] = 0;
                    break;
                }
                case Op::Xim: {
                    const OwnedRawXim xim(build_xim(static_cast<std::uint32_t>(l.n[0]), static_cast<std::uint32_t>(l.n[1]),
                                                    static_cast<std::uint32_t>(l.n[2]), static_cast<std::uint32_t>(l.n[3]),
                                                    static_cast<std::uint32_t>(l.n[4]), seed));
                    const auto rep = owner.load_xim(xim);
                    hl.push_back(rep.handle);
                    result = std::to_string(rep.handle);
                    if (rep.handle >= 0)
                        tr.chain_len[owner.record_of(rep.handle)] = int(extras_before - owner.free_extra_count());
                    break;
                }
                case Op::Name: owner.set_name(hl[l.label], l.has_name ? l.str.c_str() : nullptr); break;
                case Op::Lookup: {
                    const int hd = owner.lookup_alias(l.str);
                    hl.push_back(hd);
                    result = std::to_string(hd);
                    break;
                }
                case Op::Retire: owner.retire(hl[l.label]); break;
                case Op::Palette: {
                    const auto n = static_cast<std::uint32_t>(l.n[0]);
                    const auto a = tr.mem().allocate(0, GsPixelStorage::Psmct32, 16,
                                                     static_cast<std::uint16_t>(clut_rect_rows(n)));
                    if (a) {
                        upload_clut(tr.mem(), a->id, n, synth_bytes(std::size_t(n) * 4, seed));
                        pl.push_back(TextureDescriptor{a->id, a->serial});
                        result = std::to_string(a->id);
                    } else {
                        pl.push_back(std::nullopt);
                    }
                    break;
                }
                case Op::Bind: {
                    const int hd = hl[l.label];
                    owner.bind_palette(hd, static_cast<int>(l.n[0]), l.label2 < 0 ? OptDescriptor{} : pl[l.label2]);
                    if (owner.handle_live(hd))
                        tr.chain_len[owner.record_of(hd)] += int(extras_before - owner.free_extra_count());
                    break;
                }
                case Op::Xmd: {
                    const std::vector<std::string> names(l.args.begin(), l.args.end());
                    const auto reader = [&](const std::string& file) -> std::optional<std::vector<std::uint8_t>> {
                        tr.requests.push_back(file);
                        if (file.rfind("fail", 0) == 0) return std::nullopt;
                        std::uint64_t sum = 0;
                        for (unsigned char c : file) sum += c;
                        return build_xim(19, 32, 32, 1, 16, sum);
                    };
                    result = std::to_string(owner.load_xmd_names(names, reader).first);
                    break;
                }
                case Op::Filler: {
                    const auto a = tr.mem().allocate(static_cast<std::uint8_t>(l.n[0]), static_cast<GsPixelStorage>(l.n[1]),
                                                     static_cast<std::uint16_t>(l.n[2]), static_cast<std::uint16_t>(l.n[3]));
                    if (a) {
                        fl.push_back(TextureDescriptor{a->id, a->serial});
                        result = std::to_string(a->id);
                    } else {
                        fl.push_back(std::nullopt);
                    }
                    break;
                }
                case Op::Free:
                    if (fl[l.label]) tr.mem().free_allocation(fl[l.label]->id);
                    break;
                case Op::Retain:
                    if (fl[l.label]) tr.mem().adjust_references(fl[l.label]->id, static_cast<std::int32_t>(l.n[0]));
                    break;
            }
            out << tr.state_json(opi, l.text, result) << "\n";
            out.flush();
            ++opi;
        }
    } catch (const std::exception& e) {
        out.flush();
        die("op " + std::to_string(opi) + " rejected by the native API: " + e.what());
    }
    return 0;
}
