#include "okage/resource_textures.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace okage;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template<class F> void rejects(F&& run) {
    try { run(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected explicit guard");
}
void word(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = std::uint8_t(value >> (i * 8));
}
std::vector<std::uint8_t> xim(std::uint8_t value = 11) {
    std::vector<std::uint8_t> bytes(32 + 8 * 8 * 4, value);
    std::fill(bytes.begin(), bytes.begin() + 32, 0);
    word(bytes, 24, 8); word(bytes, 28, 8);
    return bytes;
}
ImmutableTextureBytes text(std::string_view value) {
    return std::make_shared<const std::vector<std::uint8_t>>(value.begin(), value.end());
}
std::size_t events(const TextureHandleOwner& owner, HandleEvent::Kind kind) {
    return std::count_if(owner.events().begin(), owner.events().end(),
                         [&](const auto& e) { return e.kind == kind; });
}
std::vector<int> retired(const TextureHandleOwner& owner) {
    std::vector<int> result;
    for (const auto& e : owner.events()) if (e.kind == HandleEvent::Kind::RetireCalled) result.push_back(int(e.a));
    return result;
}
struct Fixture {
    Ps2TextureMemory memory;
    TextureHandleOwner handles;
    ResourceTextureOwner owner;
    Fixture() : handles(memory), owner(handles) { memory.initialize_empty_allocator(); handles.set_event_log_limit(10000); }
    ~Fixture() { handles.retire_all(); }
};
}

int main() {
    try {
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            f.handles.enable_name_cache();
            std::vector<std::string> names{"same", "same"};
            unsigned reads = 0;
            auto reader = [&](std::string& name) -> std::optional<std::vector<std::uint8_t>> {
                require(name == "same.xim", "XMD derives .xim suffix"); ++reads; return xim();
            };
            auto a = f.owner.load_xmd(names, reader);
            require(reads == 1 && a.slots.size() == 2 && a.slots[0].handle != a.slots[1].handle,
                    "duplicate XMD name uses two handles and one actual read");
            require(a.slots[0].record == a.slots[1].record && a.slots[0].record_serial == a.slots[1].record_serial &&
                    a.slots[0].record_serial != 0 && a.slots[0].image == a.slots[1].image &&
                    a.slots[0].bytes == a.slots[1].bytes,
                    "cache alias has identical descriptor and immutable payload");
            const auto image = *a.slots[0].image;
            require(f.memory.allocation(image.id)->references == 1, "record aliases do not add descriptor references");
            const auto uploads = events(f.handles, HandleEvent::Kind::Upload);
            auto b = f.owner.load_xmd(names, reader);
            require(reads == 1 && b.xmd_reads.empty() && events(f.handles, HandleEvent::Kind::Upload) == uploads,
                    "next resource reuses cache without reads or uploads");
            require(b.slots[0].bytes == a.slots[0].bytes && b.slots[0].image == a.slots[0].image,
                    "cross-resource cache keeps payload identity");
            auto retained = a.slots[0].bytes;
            f.owner.retire_xmd(a);
            require(f.handles.handle_live(b.slots[0].handle) && f.memory.allocation(image.id).has_value(),
                    "retiring first resource leaves alias resource alive");
            f.owner.retire_xmd(b);
            require(!f.memory.allocation(image.id) && retained && retained->size() == xim().size(),
                    "last alias frees descriptor while retained immutable bytes survive");
            const auto calls = retired(f.handles).size(); f.owner.retire_xmd(a);
            require(retired(f.handles).size() == calls, "resource retirement is idempotent");
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            for (unsigned i = 0; i < 511; ++i) require(f.handles.create_handle({}) >= 0, "fill records");
            std::vector<std::string> names{"a", "b", "c"};
            auto r = f.owner.load_xmd(names, [](std::string&) { return std::optional(xim()); });
            require(r.xmd.names[1].handle == -1 && r.slots.size() == 3, "later image failures retain binding count");
            require(r.slots[0].handle == 511 && r.slots[1].handle == -7 && r.slots[2].handle == -1,
                    "walk preserves source tail and invalid sentinel order");
            f.owner.retire_xmd(r);
            require(f.handles.live_handle_count() == 511, "negative slots retire as no-ops");
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            f.handles.enable_name_cache();
            const std::vector<std::string> names{"same"};
            const auto reader = [](std::string&) { return std::optional(xim()); };
            auto survivor = f.owner.load_xmd(names, reader);
            auto stale = f.owner.load_xmd(names, reader);
            const auto old = stale.slots[0];
            require(old.handle_serial > survivor.slots[0].handle_serial && old.handle_serial != 0,
                    "alias creation advances host incarnation serial");
            f.handles.retire(old.handle);
            require(f.handles.handle_serial(old.handle) == 0 && f.handles.handle_serial(-1) == 0 &&
                    f.handles.handle_serial(TextureHandleOwner::handle_capacity) == 0,
                    "retired and invalid handles expose zero serial");
            auto replacement = f.owner.load_xmd(names, reader);
            const auto fresh = replacement.slots[0];
            require(fresh.handle == old.handle && fresh.record == old.record && fresh.image == old.image &&
                    fresh.handle_serial > old.handle_serial && fresh.bytes == old.bytes,
                    "same alias handle/record/image reuse has a fresh serial and preserves payload identity");
            f.handles.clear_events();
            rejects([&] { f.owner.retire_xmd(stale); });
            require(f.handles.handle_live(fresh.handle) && f.handles.handle_serial(fresh.handle) == fresh.handle_serial &&
                    retired(f.handles).empty() && !stale.xmd_retired,
                    "stale resource cannot retire replacement alias of surviving source record");
            f.owner.retire_xmd(replacement); f.owner.retire_xmd(survivor);
            require(f.handles.handle_serial(fresh.handle) == 0, "retiring new resource clears exposed live serial");
            const int next = f.handles.create_handle({});
            require(f.handles.handle_serial(next) > fresh.handle_serial,
                    "fresh record creation keeps serial monotonic after retirement");
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            std::vector<std::string> names{"a", "missing"};
            auto r = f.owner.load_xmd(names, [](std::string& name) -> std::optional<std::vector<std::uint8_t>> {
                return name == "a.xim" ? std::optional(xim()) : std::nullopt;
            });
            require(r.xmd.aborted && r.xmd.first == -1 && r.slots.empty() && f.handles.live_handle_count() == 1,
                    "aborted XMD does not bind or roll back earlier images");
            require(r.xmd_reads.size() == 2 && r.xmd_reads[0] && !r.xmd_reads[1], "retain actual partial-read receipts");
            f.owner.retire_xmd(r);
            require(f.handles.live_handle_count() == 1, "source unbound prefix is not silently reclaimed");
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            const auto h = f.handles.load_xim(OwnedRawXim(xim())).handle;
            f.handles.set_name(h, "old.xim"); f.handles.enable_name_cache();
            const std::vector<std::string> names{"old"};
            auto r = f.owner.load_xmd(names, [](std::string&) -> std::optional<std::vector<std::uint8_t>> {
                throw std::runtime_error("cache hit must not read");
            });
            require(r.slots[0].image == f.handles.image(h) && !r.slots[0].bytes,
                    "preexisting-cache payload is never invented");
            f.owner.retire_xmd(r); f.handles.retire(h);
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            const std::vector<std::string> names{"known"}, other{"other"};
            auto known = f.owner.load_xmd(names, [](std::string&) { return std::optional(xim()); });
            const int separate = f.handles.create_handle(known.slots[0].image);
            f.handles.set_name(separate, "other.xim"); f.handles.enable_name_cache();
            auto alias = f.owner.load_xmd(other, [](std::string&) -> std::optional<std::vector<std::uint8_t>> {
                throw std::runtime_error("cache hit must not read");
            });
            require(alias.slots[0].image == known.slots[0].image && alias.slots[0].record != known.slots[0].record &&
                    !alias.slots[0].bytes, "different record sharing image cannot inherit unrelated XIM payload");
            f.owner.retire_xmd(known); f.owner.retire_xmd(alias); f.handles.retire(separate);
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            const std::vector<std::string> original{"original"}, names{"replacement"};
            auto old = f.owner.load_xmd(original, [](std::string&) { return std::optional(xim()); });
            const auto prior = old.slots[0];
            const int guardian = f.handles.create_handle(prior.image);
            for (unsigned i = 0; i < TextureHandleOwner::record_capacity - 2; ++i)
                require(f.handles.create_handle({}) >= 0, "occupy other records before FIFO reuse");
            f.handles.retire(prior.handle);
            require(f.handles.record_serial(prior.record) == 0 && f.handles.record_serial(-1) == 0 &&
                    f.handles.record_serial(TextureHandleOwner::record_capacity) == 0,
                    "dead and invalid record serials are zero before slot reuse");
            const int replacement = f.handles.create_handle(prior.image);
            require(f.handles.record_of(replacement) == prior.record && f.handles.image(replacement) == prior.image &&
                    f.handles.record_serial(prior.record) > prior.record_serial &&
                    old.slots[0].bytes && f.memory.allocation(prior.image->id),
                    "same old record/image tuple reappears while original bytes and allocation survive");
            bool uploaded = false;
            const auto palette = f.handles.create_palette(16, {}, uploaded);
            f.handles.bind_palette(replacement, 0, palette);
            require(f.handles.palette(replacement,0) && !f.owner.describe(replacement).bytes,
                    "new record incarnation with different palette must not inherit old raw XIM payload");
            f.handles.set_name(replacement, "replacement.xim"); f.handles.enable_name_cache();
            auto alias = f.owner.load_xmd(names, [](std::string&) -> std::optional<std::vector<std::uint8_t>> {
                throw std::runtime_error("replacement record cache hit must not read");
            });
            require(alias.slots[0].record == prior.record && alias.slots[0].image == prior.image &&
                    alias.slots[0].record_serial == f.handles.record_serial(prior.record) &&
                    alias.slots[0].record_serial != prior.record_serial && !alias.slots[0].bytes && alias.xmd_reads.empty(),
                    "cache alias of reused record cannot recover stale prior-incarnation payload");
            f.owner.retire_xmd(alias); f.handles.retire(replacement); f.handles.retire(guardian);
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            f.handles.enable_name_cache();
            const std::vector<std::string> names{"eye"};
            auto r = f.owner.load_xmd(names, [](std::string&) { return std::optional(xim()); });
            const auto named = f.handles.hash_entries();
            unsigned reads = 0;
            require(f.owner.load_xta(r, text("\t1\r\n2 trailing\r\n=EYE.XIM\r\n=EYE.XIM\r\n=RAW\n"),
                [&](std::string& name) -> std::optional<std::vector<std::uint8_t>> {
                    require(name == (reads < 2 ? "eye.xim" : "raw"), "XTA lowercase full name without appending extension");
                    ++reads; return xim(std::uint8_t(30 + reads));
                }), "present XTA loads");
            require(reads == 3 && r.xta_groups == std::vector<std::int32_t>({1,2}) && r.xta_variants.size() == 3,
                    "source row order and atoi decimal prefixes preserved");
            require(r.xta_first == r.xta_variants[0].handle && r.xta_variants[0].image != r.xta_variants[1].image,
                    "first XTA binding and uncached duplicate images remain distinct");
            require(f.handles.hash_entries() == named && f.handles.reserved_handles().size() == 13,
                    "XTA neither names images nor releases unused reserve16 ids");
            auto retained = r.xta_variants[0].bytes;
            f.handles.clear_events();
            f.owner.retire_xmd(r);
            for (const auto& s : r.xta_variants) require(f.handles.handle_live(s.handle), "resource retirement preserves XTA list");
            f.owner.retire_scene_images();
            require(retired(f.handles) == std::vector<int>({r.slots[0].handle,r.xta_variants[0].handle,
                    r.xta_variants[1].handle,r.xta_variants[2].handle}), "retirement source order: model then global XTA list");
            require(f.owner.scene_images().empty() && f.handles.reserved_handles().size() == 13 && retained,
                    "global retirement clears list only; pending reservations and retained bytes survive");
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            ResourceTextures absent, empty, partial;
            const auto reader = [](std::string& name) -> std::optional<std::vector<std::uint8_t>> {
                return name == "ok.xim" ? std::optional(xim()) : std::nullopt;
            };
            require(!f.owner.load_xta(absent, nullptr, reader) && !f.owner.load_xta(empty, text(""), reader),
                    "failed XTA read does not reserve");
            require(events(f.handles, HandleEvent::Kind::Reserved) == 0, "no missing-sidecar effects");
            rejects([&] { f.owner.load_xta(partial, text("1\n=OK.XIM\n=MISSING.XIM\n"), reader); });
            require(partial.xta_groups == std::vector<std::int32_t>({1}) && partial.xta_variants.size() == 1 &&
                    f.owner.scene_images().size() == 1 && f.handles.reserved_handles().size() == 15,
                    "failed later XTA image preserves reached group, image, ownership and reservation");
            f.owner.retire_scene_images();
        }
        {
            auto fixture = std::make_unique<Fixture>();
            auto& f = *fixture;
            for (unsigned i = 0; i < 511; ++i) f.handles.create_handle({});
            ResourceTextures r;
            f.owner.load_xta(r, text("=A.XIM\n=B.XIM\n"), [](std::string&) { return std::optional(xim()); });
            require(r.xta_variants.size() == 2 && r.xta_variants[0].handle == 511 &&
                    r.xta_variants[1].handle == -1 && f.owner.scene_images()[1].handle == -1,
                    "XTA retains failed returned handle in both ordered lists");
            require(r.xta_first == 511, "failed later variant does not replace first binding");
            f.owner.retire_scene_images();
            require(f.handles.live_handle_count() == 511, "XTA negative retirement is a no-op");
        }
        std::cout << "resource texture tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
