#include "okage/texture_handles.hpp"
#include "okage/texture_transfer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace okage;
using K = HandleEvent::Kind;

namespace {
void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
template <class F> void rejects(const char* label, F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("expected rejection: ") + label);
}
template <class F> void scenario(F&& f) { f(); }

std::vector<std::uint8_t> sequence(std::size_t n, std::uint8_t first = 1) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(first + i * 7);
    return v;
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int k = 0; k < 4; ++k) v[at + k] = static_cast<std::uint8_t>(x >> (8 * k));
}
// PSM19 (or PSM0 when levels == 0) XIM with `levels` palettes of `colors` words.
std::vector<std::uint8_t> build_xim(std::uint32_t w, std::uint32_t h, std::uint32_t levels,
                                    std::uint32_t colors, std::uint8_t seed) {
    const bool indexed = levels > 0;
    const std::size_t pal = indexed ? 16 + std::size_t(levels) * colors * 4 : 0;
    const std::size_t bytes = indexed ? std::size_t(w) * h : std::size_t(w) * h * 4;
    std::vector<std::uint8_t> v(16 + pal + 16 + bytes, 0);
    put32(v, 0, (indexed ? 19u : 0u) << 20);
    if (indexed) {
        put32(v, 0x10, static_cast<std::uint32_t>(pal));
        put32(v, 0x18, levels);
        put32(v, 0x1c, colors);
        for (std::size_t i = 0; i < std::size_t(levels) * colors * 4; ++i)
            v[0x20 + i] = static_cast<std::uint8_t>(seed + i * 5);
    }
    const auto ih = 16 + pal;
    put32(v, ih + 8, h);
    put32(v, ih + 12, w);
    for (std::size_t i = 0; i < bytes; ++i) v[ih + 16 + i] = static_cast<std::uint8_t>(seed * 3 + i * 11);
    return v;
}
std::size_t defined_count(const Ps2TextureMemory& m) {
    std::size_t n = 0;
    for (std::uint32_t a = 0; a < Ps2TextureMemory::vram_bytes; ++a) n += m.observe_byte(a).value.has_value();
    return n;
}
std::size_t count_events(const TextureHandleOwner& o, K kind) {
    return std::count_if(o.events().begin(), o.events().end(), [&](const HandleEvent& e) { return e.kind == kind; });
}
std::uint16_t refs(const Ps2TextureMemory& m, const OptDescriptor& d) { return m.allocation(d->id)->references; }
const auto C32 = GsPixelStorage::Psmct32;
const auto T8 = GsPixelStorage::Psmt8;
}

int main() {
    try {
        // Creation, record/handle/descriptor identities, upload.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const auto px = sequence(8 * 8 * 4);
            const int a = o.create_image(C32, 8, 8, px), b = o.create_image(C32, 8, 8, px);
            require(a == 0 && b == 1, "fresh ids are first-fit ascending");
            require(o.record_of(a) == 0 && o.record_of(b) == 1, "records are allocated FIFO");
            const auto da = o.image(a);
            require(da && refs(m, da) == 1, "record holds exactly one descriptor reference");
            require(o.live_handle_count() == 2 && o.record_handle_count(0) == 1, "counts");
            require(defined_count(m) == 2 * 256, "two 8x8 PSM0 uploads");
            const int nu = o.create_image(C32, 8, 8, {});
            require(nu == 2 && defined_count(m) == 2 * 256, "no pixel pointer: no upload");
            rejects("psm27", [&] { o.create_image(GsPixelStorage::Psmt8H, 8, 8, {}); });
            {
                const int h24 = o.create_image(GsPixelStorage::Psmct24, 8, 8, sequence(8 * 8 * 3));
                require(h24 >= 0, "psm1 image created");
                const auto base = m.allocation(o.image(h24)->id)->base_block * 256u;
                require(m.observe_byte(base).value.has_value() && !m.observe_byte(base + 3).value.has_value(),
                        "psm1 writes bytes 0..2 only");
                o.retire(h24);
            }
            rejects("qwc 0", [&] { o.create_image(C32, 1, 1, {}); });
            rejects("short pixels", [&] { o.create_image(C32, 8, 8, sequence(16)); });
            require(o.live_handle_count() == 3, "rejections changed nothing");
        });
        // Record exhaustion: 0x1cd1e0 frees the fresh descriptor; no other cleanup.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            o.set_event_log_limit(1 << 16);
            for (int i = 0; i < TextureHandleOwner::record_capacity; ++i)
                require(o.create_image(C32, 2, 2, sequence(16)) == i, "fill records");
            const auto before_avail = m.available_count();
            const auto bytes = defined_count(m);
            o.clear_events();
            require(o.create_image(C32, 2, 2, sequence(16)) == -1, "513th image fails");
            require(m.available_count() == before_avail, "fresh descriptor was freed again (record returned)");
            require(count_events(o, K::DescriptorFree) == 1 && defined_count(m) == bytes, "freed once, no upload");
            require(o.free_record_count() == 0 && o.live_handle_count() == 512, "state kept");
            // Aliases are not limited by records: only by ids.
            o.set_name(0, "x");
            o.enable_name_cache();
            require(o.lookup_alias("x") == 512, "alias takes the next handle id");
        });
        // Aliases, names, case folding, 19-character truncation, duplicates.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            o.set_event_log_limit(1 << 16);
            const int h = o.create_image(C32, 8, 8, sequence(256));
            o.set_name(h, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.xim");
            require(o.record_name(o.record_of(h)) == "abcdefghijklmnopqrs", "lower-cased, 19 chars");
            o.clear_events();
            const int alias = o.lookup_alias("AbCdEfGhIjKlMnOpQrStUvW.xim");
            require(alias == 1 && o.record_of(alias) == o.record_of(h), "case-insensitive 19-char match");
            require(o.record_handle_count(0) == 2 && o.live_handle_count() == 2, "alias counts");
            require(count_events(o, K::Upload) == 0 && count_events(o, K::DescriptorRetain) == 0,
                    "hit does not upload or touch descriptors");
            require(refs(m, o.image(h)) == 1, "descriptor reference unchanged by alias");
            require(o.lookup_alias("nothere") == -1 && o.lookup_alias("abcdefghijklmnopqrs") == 2, "miss/hit");
            // Retire alias, then last handle: record and descriptor freed on the last.
            o.clear_events();
            o.retire(alias);
            require(o.record_handle_count(0) == 2 && o.live_record_count() == 1, "record survives alias retire");
            o.retire(2);
            o.retire(h);
            require(o.live_record_count() == 0 && o.live_handle_count() == 0, "destroyed on last");
            const auto& ev = o.events();
            const auto rel = std::find_if(ev.begin(), ev.end(), [](auto& e) { return e.kind == K::DescriptorRelease; });
            const auto fre = std::find_if(ev.begin(), ev.end(), [](auto& e) { return e.kind == K::DescriptorFree; });
            const auto rec = std::find_if(ev.begin(), ev.end(), [](auto& e) { return e.kind == K::RecordFreed; });
            require(rel < fre && fre < rec, "descriptor released/freed before the record node");
            require(o.lookup_alias("abcdefghijklmnopqrs") == -1, "hash entry tombstoned");
            o.retire(h); o.retire(-1); o.retire(5000);  // invalid: ignored
            require(o.live_handle_count() == 0, "invalid retire ignored");
        });
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const int a = o.create_image(C32, 8, 8, sequence(256));
            const int b = o.create_image(C32, 8, 8, sequence(256));
            o.set_name(a, "dup.xim");
            const int slot_a = o.record_hash_slot(o.record_of(a));
            o.set_name(b, "dup.xim");
            require(o.record_hash_slot(o.record_of(b)) == slot_a && o.record_hash_slot(o.record_of(a)) == -1,
                    "newest duplicate takes the hash entry");
            require(o.record_name(o.record_of(a)) == "dup.xim", "older record keeps its text");
            o.enable_name_cache();
            const int hit = o.lookup_alias("dup.xim");
            require(o.record_of(hit) == o.record_of(b), "lookup resolves to the newest");
            o.retire(hit); o.retire(b);
            require(o.lookup_alias("dup.xim") == -1, "older same-name record is unreachable once newest is gone");
            o.set_name(a, "dup.xim");
            require(o.lookup_alias("dup.xim") >= 0, "re-registering makes it reachable");
            o.set_name(a, nullptr);
            require(o.record_name(o.record_of(a)).empty() && o.record_hash_slot(o.record_of(a)) == -1, "null clears");
        });
        // Reservation queue.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            require(o.reserve_handles(4) == 0 && o.reserved_handles() == std::vector<int>({0, 1, 2, 3}), "reserve");
            require(o.create_image(C32, 8, 8, sequence(256)) == 0, "create consumes the queue head");
            require(o.reserve_handles(2) == 1, "second reserve releases unused 1..3 then first-fits at 1");
            require(o.reserved_handles() == std::vector<int>({1, 2}), "queue");
            o.set_name(0, "n");
            o.enable_name_cache();
            require(o.lookup_alias("n") == 1, "alias consumes the reserved head too");
            require(o.reserve_handles(TextureHandleOwner::handle_capacity) == -1, "too many");
            require(o.reserved_handles().empty(), "failed reserve does not undo the phase-1 release");
            require(o.reserve_handles(TextureHandleOwner::handle_capacity - 2) == 2, "exact remaining run");
            rejects("n<=0", [&] { o.reserve_handles(0); });
        });
        // Palette binding.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            o.set_event_log_limit(1 << 16);
            const int h = o.create_image(T8, 16, 16, sequence(256));
            auto pal = [&](int n) {
                const auto a = m.allocate(0, C32, 16, 2);
                require(a.has_value(), "palette alloc");
                upload_clut(m, a->id, 16, sequence(64, static_cast<std::uint8_t>(n)));
                return OptDescriptor(TextureDescriptor{a->id, a->serial});
            };
            const auto p1 = pal(1), p2 = pal(2);
            require(o.bind_palette(h, 0, std::nullopt) == PaletteBindResult::Unchanged, "null == null");
            require(o.bind_palette(h, 0, p1) == PaletteBindResult::Bound && refs(m, p1) == 1, "bind retains");
            require(o.bind_palette(h, 0, p1) == PaletteBindResult::Unchanged && refs(m, p1) == 1, "same: no change");
            o.clear_events();
            require(o.bind_palette(h, 0, p2) == PaletteBindResult::Bound, "replace");
            require(!m.allocation(p1->id) && refs(m, p2) == 1, "old freed (last reference), new retained");
            const auto& ev = o.events();
            require(ev.size() == 4 && ev[0].kind == K::PaletteBind && ev[0].a == h && ev[0].c == p2->id &&
                    ev[1].kind == K::DescriptorRelease && ev[1].b == 0 && ev[2].kind == K::DescriptorFree &&
                    ev[3].kind == K::DescriptorRetain && ev[3].b == 1, "release old (and free) BEFORE retaining new");
            require(o.bind_palette(h, 0, std::nullopt) == PaletteBindResult::Bound && !o.palette(h, 0) &&
                    !m.allocation(p2->id), "null releases the old and stores null");
            require(o.bind_palette(99, 0, p1) == PaletteBindResult::InvalidHandle, "bad handle");
            // Extra levels.
            const auto p3 = pal(3);
            require(o.bind_palette(h, 3, p3) == PaletteBindResult::Bound && o.palette(h, 3) == p3 &&
                    !o.palette(h, 1) && !o.palette(h, 2), "level 3 creates a 3-link chain");
            require(o.extra_chain_length(h) == 3, "chain length counts null-bound links");
            require(o.free_extra_count() == TextureHandleOwner::extra_palette_capacity - 3, "three nodes in use");
            rejects("negative level", [&] { o.bind_palette(h, -1, p3); });
            o.retire(h);
            require(o.free_extra_count() == TextureHandleOwner::extra_palette_capacity && !m.allocation(p3->id),
                    "retire releases the chain and its descriptors");
        });
        // Extra pool exhaustion keeps links already made, no bind, no rollback.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const int h = o.create_image(T8, 16, 16, sequence(256));
            const auto a = m.allocate(0, C32, 16, 2);
            const TextureDescriptor p{a->id, a->serial};
            require(o.bind_palette(h, 128, p) == PaletteBindResult::Bound, "128 links fit exactly");
            require(o.free_extra_count() == 0, "pool empty");
            const int h2 = o.create_image(T8, 16, 16, sequence(256));
            require(o.bind_palette(h2, 1, p) == PaletteBindResult::ExtraPoolExhausted, "exhausted");
            require(!o.palette(h2, 1) && refs(m, p) == 1, "nothing bound, no retain");
            o.retire(h);
            require(!m.allocation(p.id), "chain release freed the palette (last reference)");
            const auto b = m.allocate(0, C32, 16, 2);
            require(o.bind_palette(h2, 1, TextureDescriptor{b->id, b->serial}) == PaletteBindResult::Bound, "freed nodes reusable");
        });
        // load_xim: all levels attempted; failed palette allocations bind null and continue.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const OwnedRawXim xim(build_xim(16, 16, 3, 16, 7));
            const auto r = o.load_xim(xim);
            require(r.handle == 0 && r.palettes.size() == 3, "image + 3 levels");
            std::vector<TextureAllocationId> ids;
            for (std::uint32_t i = 0; i < 3; ++i) {
                require(r.palettes[i].palette && r.palettes[i].uploaded && r.palettes[i].bind == PaletteBindResult::Bound,
                        "level bound");
                require(o.palette(0, static_cast<int>(i)) == r.palettes[i].palette, "binding visible per level");
                require(refs(m, r.palettes[i].palette) == 1, "one reference from the record");
                ids.push_back(r.palettes[i].palette->id);
            }
            require(std::set<TextureAllocationId>(ids.begin(), ids.end()).size() == 3, "distinct descriptors");
            require(defined_count(m) == 256 + 3 * 128, "image bytes + three 16x2 CLUT rectangles");
            o.retire(0);
            for (auto id : ids) require(!m.allocation(id), "all levels freed on destruction");
        });
        scenario([&] {
            // Leave exactly 3 allocation records: image, level 0, level 1 succeed; level 2 fails (null bound).
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            while (m.available_count() > 3) require(m.allocate(0, C32, 8, 8).has_value(), "filler");
            const auto r = o.load_xim(OwnedRawXim(build_xim(16, 16, 3, 16, 7)));
            require(r.handle == 0 && r.palettes.size() == 3, "all three levels attempted");
            require(r.palettes[0].palette && r.palettes[1].palette && !r.palettes[2].palette, "third allocation fails");
            require(!r.palettes[2].uploaded && r.palettes[2].bind == PaletteBindResult::Unchanged, "null bound = unchanged");
            require(m.available_count() == 0 && o.image(0), "image and earlier levels stay live");
            // Everything exhausted: image allocation fails -> -1, no palette attempts.
            const auto none = o.load_xim(OwnedRawXim(build_xim(16, 16, 3, 16, 9)));
            require(none.handle == -1 && none.palettes.empty(), "image failure: no levels");
        });
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            while (m.available_count() > 1) require(m.allocate(0, C32, 8, 8).has_value(), "filler");
            const auto r = o.load_xim(OwnedRawXim(build_xim(16, 16, 3, 16, 7)));
            require(r.handle == 0 && r.palettes.size() == 3, "every level still attempted");
            for (const auto& p : r.palettes) require(!p.palette && !p.uploaded, "all null, nothing uploaded");
            require(defined_count(m) == 256 && o.live_handle_count() == 1, "image uploaded, kept");
            std::vector<std::uint8_t> raw(16 + 16 + 16 * 2 + 16 + 64, 0);
            put32(raw, 0, 19u << 20); put32(raw, 4, 2u << 19);
            put32(raw, 0x10, 16 + 16 * 2); put32(raw, 0x18, 1); put32(raw, 0x1c, 16);
            put32(raw, 0x30 + 16 + 8, 8); put32(raw, 0x30 + 16 + 12, 8);
            const OwnedRawXim cpsm2(raw);
            rejects("cpsm2", [&] { o.load_xim(cpsm2); });
        });
        // XMD name processing.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            std::vector<std::string> requests;
            auto reader = [&](const std::string& f) -> std::optional<std::vector<std::uint8_t>> {
                requests.push_back(f);
                if (f.rfind("fail", 0) == 0) return std::nullopt;
                return build_xim(16, 16, 1, 16, static_cast<std::uint8_t>(f.size()));
            };
            const std::vector<std::string> names{"Grass", "tree", "Grass"};
            // Cache off: every name loads, duplicates are separate records, newest wins the hash entry.
            auto r = o.load_xmd_names(names, reader);
            require(r.reservation == 0 && r.first == 0 && r.names.size() == 3, "result");
            require(requests.size() == 3 && requests[0] == "Grass.xim", "file name as given, plus .xim");
            require(o.live_record_count() == 3 && o.live_handle_count() == 3, "three records");
            require(o.record_hash_slot(0) == -1 && o.record_hash_slot(2) >= 0, "newest duplicate owns the entry");
            o.enable_name_cache();
            requests.clear();
            r = o.load_xmd_names(names, reader);
            require(requests.empty() && r.names.size() == 3, "cache on: all hits, no reads");
            for (const auto& n : r.names) require(n.kind == XmdNameOutcome::Kind::CacheHit, "hit");
            require(o.record_of(r.names[0].handle) == 2 && o.record_of(r.names[2].handle) == 2, "hits resolve to the newest");
            require(o.live_record_count() == 3 && o.record_handle_count(2) == 3, "aliases only");
            const auto bytes = defined_count(m);
            const std::vector<std::string> mixed{"TREE", "new"};
            requests.clear();
            r = o.load_xmd_names(mixed, reader);
            require(requests == std::vector<std::string>({"new.xim"}) && r.names[0].kind == XmdNameOutcome::Kind::CacheHit &&
                    r.names[1].kind == XmdNameOutcome::Kind::Loaded, "case-insensitive hit, miss loads");
            require(defined_count(m) > bytes, "only the miss uploads");
            // Read failure aborts with -1; earlier handles and the reservation stay.
            const std::vector<std::string> bad{"ok1", "fail1", "ok2"};
            requests.clear();
            const int live = o.live_handle_count();
            r = o.load_xmd_names(bad, reader);
            require(r.aborted && r.first == -1 && r.names.size() == 1 && requests.size() == 2, "abort");
            require(o.live_handle_count() == live + 1 && !o.reserved_handles().empty(), "partial state kept, reserve alive");
            require(o.load_xmd_names({}, reader).first == 0 && o.load_xmd_names({}, reader).reservation == -1, "empty table");
        });
        scenario([&] {
            // 18f7a8 failure inside an XMD list does not abort: no handle consumed for it.
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            while (m.available_count() > 1) require(m.allocate(0, C32, 8, 8).has_value(), "filler");
            auto reader = [&](const std::string& f) -> std::optional<std::vector<std::uint8_t>> {
                return build_xim(16, 16, 1, 16, static_cast<std::uint8_t>(f.size()));
            };
            const std::vector<std::string> names{"aa", "bb"};
            const auto r = o.load_xmd_names(names, reader);
            require(!r.aborted && r.names.size() == 2, "continues");
            require(r.names[0].kind == XmdNameOutcome::Kind::Loaded && r.names[0].handle == 0, "first took the last record");
            require(r.names[1].kind == XmdNameOutcome::Kind::LoadReturnedFailure && r.names[1].handle == -1, "second fails");
            require(r.first == 0, "first non-negative handle");
        });
        // Snapshots stay immutable across handle lifecycle changes; separate owners are separate lineages.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const int h = o.create_image(C32, 8, 8, sequence(256));
            const auto d = *o.image(h);
            const auto snap = m.snapshot();
            const auto id0 = snap.identity();
            o.retire(h);
            const int h2 = o.create_image(C32, 8, 8, sequence(256, 9));  // FIFO id reuse is possible
            require(snap.allocation(d.id) && snap.allocation(d.id)->serial == d.serial && snap.allocation(d.id)->references == 1,
                    "snapshot keeps the old incarnation and reference count");
            require(snap.identity() == id0 && m.identity() != id0, "identity advanced only on the live memory");
            const auto d2 = *o.image(h2);
            require(d2.serial != d.serial, "new incarnation has a new serial");
            Ps2TextureMemory other; other.initialize_empty_allocator();
            TextureHandleOwner o2(other);
            const int g = o2.create_image(C32, 8, 8, sequence(256));
            require(g == h && o2.image(g)->id == d.id && o2.image(g)->serial == d.serial,
                    "equal ids/serials across separate memories: never a cross-owner key");
        });
        // Review findings: stale descriptor leaves a bind untouched; saturated hash fails explicitly.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const int h = o.create_image(T8, 16, 16, sequence(256));
            const auto a = m.allocate(0, C32, 16, 2), b = m.allocate(0, C32, 16, 2);
            const TextureDescriptor p1{a->id, a->serial}, p2{b->id, b->serial};
            o.bind_palette(h, 0, p1);
            m.free_allocation(p2.id);
            bool threw = false;
            try { o.bind_palette(h, 0, p2); } catch (const std::logic_error&) { threw = true; }
            require(threw && o.palette(h, 0) == OptDescriptor(p1) && m.allocation(p1.id), "bind validated before releasing");
            o.retire(h);  // still retireable
            require(o.live_handle_count() == 0 && !m.allocation(p1.id), "retire works after the failed bind");
        });
        scenario([&] {
            // Saturate every slot with tombstones: one name per byte-sum residue, each registered then retired.
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            auto name_for_sum = [](int target) {
                std::string n;
                while (target > 0) {
                    int c = std::min(122, target);
                    while (c >= 65 && c <= 90) --c;  // uppercase would be folded
                    n.push_back(static_cast<char>(c));
                    target -= c;
                }
                return n;
            };
            for (int residue = 0; residue < TextureHandleOwner::hash_size; ++residue) {
                const int h = o.create_image(C32, 2, 2, sequence(16));
                const auto name = name_for_sum(residue + 1024);
                require(name.size() <= 19, "name fits the 19-character field");
                o.set_name(h, name.c_str());
                o.retire(h);
            }
            require(o.hash_entries().size() == TextureHandleOwner::hash_size, "every slot is a tombstone");
            const int h = o.create_image(C32, 2, 2, sequence(16));
            bool threw = false;
            try { o.set_name(h, "anything"); } catch (const std::logic_error&) { threw = true; }
            require(threw, "retail would spin forever; native fails explicitly");
        });
        // Unsupported name domain (high-bit / NUL) is rejected before any mutation; ASCII is untouched.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            const int h = o.create_image(C32, 2, 2, sequence(16));
            o.set_name(h, "Keep");
            const auto before_hash = o.hash_entries();
            const auto before_epoch = m.identity();
            const std::string high = "caf\xc3\xa9", nul(std::string("a\0b", 3));
            auto invalid = [&](const char* what, auto&& call) {
                bool threw = false;
                try { call(); } catch (const std::invalid_argument&) { threw = true; }
                require(threw, what);
            };
            invalid("set_name high", [&] { o.set_name(h, high.c_str()); });
            invalid("lookup high", [&] { o.lookup_alias(high); });
            invalid("lookup nul", [&] { o.lookup_alias(nul); });
            auto reader = [&](const std::string&) -> std::optional<std::vector<std::uint8_t>> {
                throw std::logic_error("reader must not run");
            };
            const std::vector<std::string> names{"fine", high};
            invalid("xmd high", [&] { o.load_xmd_names(names, reader); });
            require(o.hash_entries() == before_hash && o.record_name(o.record_of(h)) == "keep", "name state intact");
            require(o.live_handle_count() == 1 && o.reserved_handles().empty() && m.identity() == before_epoch,
                    "no reservation, handle or allocation after rejected names");
            require(o.lookup_alias("KEEP") >= 0, "ASCII lookup still folds case");
            o.set_name(h, nullptr);  // null name clears
            require(o.record_name(o.record_of(h)).empty(), "null clears");
        });
        // Event log is bounded/disabled; retire_all tears down without touching the memory.
        scenario([&] {
            Ps2TextureMemory m; m.initialize_empty_allocator();
            TextureHandleOwner o(m);
            o.create_image(C32, 2, 2, sequence(16));
            require(o.events().empty() && o.event_log_limit() == 0, "logging disabled by default");
            o.set_event_log_limit(3);
            for (int i = 0; i < 5; ++i) o.create_handle(std::nullopt);
            require(o.events().size() == 3 && o.events_dropped() == 2, "keeps the newest N, counts the rest");
            o.set_event_log_limit(1);
            require(o.events().size() == 1 && o.events_dropped() == 4, "shrinking drops the oldest");
            o.set_event_log_limit(0);
            for (int i = 0; i < 100; ++i) o.retire(o.create_handle(std::nullopt));
            require(o.events().empty(), "disabled log stays empty");
            o.reserve_handles(8);
            o.create_image(T8, 16, 16, sequence(256));
            o.load_xim(OwnedRawXim(build_xim(16, 16, 3, 16, 7)));
            o.retire_all();
            require(o.live_handle_count() == 0 && o.live_record_count() == 0 && o.reserved_handles().empty(),
                    "retire_all empties handles, records and the reserved queue");
            require(m.available_count() == 4096, "every descriptor reference released");
            o.retire_all();  // idempotent
        });
        std::cout << "texture handle tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
