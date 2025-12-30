#include "okage/model_groups.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace okage;
namespace {
void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
template<class Exception, class F> void rejects(F&& run, const char* why) {
    try { run(); } catch (const Exception&) { return; }
    throw std::runtime_error(why);
}
void word(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = std::uint8_t(value >> (8 * i));
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
std::vector<int> retired(const TextureHandleOwner& owner) {
    std::vector<int> result;
    for (const auto& event : owner.events())
        if (event.kind == HandleEvent::Kind::RetireCalled) result.push_back(int(event.a));
    return result;
}
struct Fixture {
    Ps2TextureMemory memory;
    TextureHandleOwner handles;
    ResourceTextureOwner textures;
    ModelGroupOwner groups;
    Fixture() : handles(memory), textures(handles), groups(textures) {
        memory.initialize_empty_allocator();
        handles.set_event_log_limit(10000);
    }
    ~Fixture() { handles.retire_all(); }
    ResourceTextures load(std::initializer_list<std::string> names) {
        const std::vector<std::string> list(names);
        return textures.load_xmd(list, [](std::string&) { return std::optional(xim()); });
    }
};
void bootstrap_and_ranges() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    require(f.groups.groups().empty() && f.groups.ranges().available_count() == 128,
            "pool construction must not hide embedded startup model allocations");
    // 18e4c0 explicitly reaches two one-model, zero-texture 183450 calls.
    const auto first = f.groups.insert("embedded-1fece0", 1, {}, false).range;
    const auto second = f.groups.insert("embedded-1ffc90", 1, {}, false).range;
    require(first.first == 0 && second.first == 1 && first.count == 1 && second.count == 1,
            "explicit embedded groups consume source model IDs zero and one");
    require(f.groups.find(first)->textures.slots.empty() && !f.groups.find(first)->reported &&
            !f.groups.find(second)->reported && f.handles.live_handle_count() == 0,
            "zero binding count still owns a live unreported model range");
    const auto wal = f.groups.insert("wal.xmd", 1, {}).range;
    const auto hole = f.groups.insert("unused-three-model-definition", 3, {}).range;
    const auto tail = f.groups.insert("later-definition", 2, {}).range;
    require(wal.first == 2 && hole.first == 3 && tail.first == 6,
            "model counts, not group vector positions or texture counts, advance IDs");
    require(f.groups.retire(hole), "loaded unused definition explicitly releases its whole range");
    const auto reused = f.groups.insert("actor", 2, {}).range;
    require(reused.first == 3 && reused.serial != hole.serial && f.groups.find(tail),
            "actor takes earliest adequate freed definition range with fresh lifetime");
    require(f.groups.retire(tail) && f.groups.retire(reused), "release adjacent ranges in opposite order");
    require(f.groups.ranges().free_ranges() == std::vector<ModelRangeAllocator::FreeRange>{{3,125}},
            "group retirement coalesces complete stored model counts");
    require(f.groups.find(first) && f.groups.find(second) && f.groups.find(wal),
            "range reuse preserves earlier embedded and map groups");
}
void exhaustion_preserves_reached_texture_load() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    const auto occupied = f.groups.insert("full-pool",128,{}).range;
    auto loaded = f.load({"already-loaded-before-183450"});
    const int handle = loaded.slots[0].handle;
    const auto image = *loaded.slots[0].image;
    f.handles.clear_events();
    rejects<std::runtime_error>([&] { f.groups.insert("exhausted",1,std::move(loaded)); },
                                "exhausted model pool must reject new group");
    require(f.groups.groups().size() == 1 && f.groups.find(occupied) &&
            f.groups.ranges().available_count() == 0 && f.handles.handle_live(handle) &&
            f.memory.allocation(image.id) && retired(f.handles).empty(),
            "182f08 allocation failure does not roll back already reached XMD texture effects");
    require(f.groups.retire(occupied) && f.groups.ranges().available_count() == 128 &&
            f.handles.handle_live(handle),
            "retiring unrelated full model range cannot sweep unbound successful load");
}
void shared_instances_resolve_current_numeric_group() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    const auto prefix = f.groups.insert("earlier-range", 1, {}).range;
    const auto& shared = f.groups.insert("shared-three-model-xmd", 3, f.load({"body", "eye"}));
    const auto lease = shared.range;
    const int body = shared.textures.slots[0].handle, eye = shared.textures.slots[1].handle;
    const auto image = *shared.textures.slots[0].image;
    auto retained_bytes = shared.textures.slots[0].bytes;
    f.groups.bind_instance(3, lease); f.groups.bind_instance(400, lease);
    f.handles.clear_events(); f.groups.retire_instance(3);
    require(retired(f.handles) == std::vector<int>({body,eye}) && !f.groups.find(lease) &&
            shared.retired && !f.handles.handle_live(body) && !f.memory.allocation(image.id),
            "first reached member removes whole group and ordered bindings before last instance");
    require(retained_bytes && retained_bytes->size() == xim().size(),
            "submitted immutable bytes may survive logical group retirement");
    require(f.groups.retire(prefix), "free earlier range before replacement allocation");
    const auto& replacement = f.groups.insert("new-four-model-xmd", 4, f.load({"replacement"}));
    require(replacement.range.first < lease.first &&
            replacement.range.first + replacement.range.count > lease.first,
            "replacement range contains the old instance's numeric model ID in its interior");
    const int replacement_handle = replacement.textures.slots[0].handle;
    f.handles.clear_events(); f.groups.retire_instance(400);
    require(retired(f.handles) == std::vector<int>{replacement_handle} &&
            !f.groups.find(replacement.range) && !f.handles.handle_live(replacement_handle),
            "late shared instance removes current containing group after numeric ID reuse");
    const auto& fresh = f.groups.insert("another-shared-group", 3, f.load({"fresh"}));
    const int fresh_handle = fresh.textures.slots[0].handle;
    f.handles.clear_events();
    f.groups.bind_instance(3, fresh.range); f.groups.bind_instance(400, fresh.range);
    f.groups.retire_instance(3); f.groups.retire_instance(400);
    require(retired(f.handles) == std::vector<int>({fresh_handle}),
            "later shared cleanup misses once no current group contains the numeric ID");
}
void individual_associated_before_main() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    const auto& main = f.groups.insert("actor", 1, f.load({"main-a", "main-b"}));
    const auto& shadow = f.groups.insert("shadow", 1, f.load({"shadow"}));
    const std::vector<int> expected{shadow.textures.slots[0].handle,
                                  main.textures.slots[0].handle, main.textures.slots[1].handle};
    f.groups.bind_instance(639, main.range, shadow.range);
    f.handles.clear_events(); f.groups.retire_instance(639);
    require(retired(f.handles) == expected && main.retired && shadow.retired,
            "1b8448 removes auxiliary group before ordered main bindings");
    f.groups.retire_instance(639);
    require(retired(f.handles) == expected, "retiring inactive individual instance has no effects");
}
void associated_and_main_passes_resolve_current_ids() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    const auto prefix = f.groups.insert("earlier-range",1,{}).range;
    const auto old_shadow = f.groups.insert("old-shadow",1,f.load({"old-shadow"})).range;
    const auto old_main = f.groups.insert("old-main",1,f.load({"old-main"})).range;
    f.groups.bind_instance(4,old_main,old_shadow);
    require(f.groups.retire(old_shadow) && f.groups.retire(prefix), "earlier cleanup releases shadow and lower range");
    const auto& shadow = f.groups.insert("replacement-shadow-range",2,f.load({"new-shadow"}));
    require(shadow.range.first == 0 && old_shadow.first == 1,
            "old auxiliary ID now lies inside a different current group");
    f.handles.clear_events(); f.groups.retire_associated();
    require(retired(f.handles) == std::vector<int>{shadow.textures.slots[0].handle} &&
            !f.groups.find(shadow.range) && f.groups.find(old_main),
            "associated pass resolves numeric auxiliary ID against current containing group");
    require(f.groups.retire(old_main), "earlier shared cleanup releases main ID before main pass");
    const auto& main = f.groups.insert("replacement-main-range",3,f.load({"new-main"}));
    require(main.range.first == 0 && old_main.first == 2,
            "old main ID now lies inside a different current group");
    f.handles.clear_events(); f.groups.retire_main();
    require(retired(f.handles) == std::vector<int>{main.textures.slots[0].handle} &&
            !f.groups.find(main.range),
            "main pass resolves numeric model ID rather than retaining a stale host lease");
    const auto fresh = f.groups.insert("fresh",1,{}).range;
    f.groups.bind_instance(4,fresh);
}
void associated_main_and_xta_order() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    auto high_resource = f.load({"main-high"});
    f.textures.load_xta(high_resource, text("=HIGH-A.XIM\n=HIGH-B.XIM\n"),
                       [](std::string&) { return std::optional(xim(21)); });
    std::vector<int> xta;
    for (const auto& slot : high_resource.xta_variants) xta.push_back(slot.handle);
    const auto& high = f.groups.insert("high-instance", 1, std::move(high_resource));
    const auto& high_shadow = f.groups.insert("high-shadow", 1, f.load({"shadow-high"}));
    const auto& low_shadow = f.groups.insert("low-shadow", 1, f.load({"shadow-low"}));
    const auto& unreached = f.groups.insert("loaded-unreached", 1, f.load({"unreached"}));
    auto low_resource = f.load({"main-low-a", "main-low-b"});
    f.textures.load_xta(low_resource, text("=LOW.XIM\n"),
                       [](std::string&) { return std::optional(xim(31)); });
    xta.push_back(low_resource.xta_variants[0].handle);
    const auto& low = f.groups.insert("low-instance", 1, std::move(low_resource));
    f.groups.bind_instance(639, high.range, high_shadow.range);
    f.groups.bind_instance(0, low.range, low_shadow.range);
    const auto reserves = f.handles.reserved_handles();
    require(!reserves.empty(), "XTA fixture retains unused reserve16 IDs");
    f.handles.clear_events(); f.groups.retire_associated();
    std::vector<int> expected{low_shadow.textures.slots[0].handle,high_shadow.textures.slots[0].handle};
    require(retired(f.handles) == expected && f.groups.find(low.range) && f.groups.find(high.range),
            "associated pass uses ascending instance order, not group creation order, and preserves main pass");
    for (const auto handle : xta) require(f.handles.handle_live(handle), "associated pass preserves global XTA images");
    f.groups.retire_associated();
    require(retired(f.handles) == expected, "repeated shared auxiliary list misses do not retire twice");
    f.groups.retire_main();
    expected.push_back(low.textures.slots[0].handle); expected.push_back(low.textures.slots[1].handle);
    expected.push_back(high.textures.slots[0].handle);
    expected.insert(expected.end(),xta.begin(),xta.end());
    require(retired(f.handles) == expected && f.textures.scene_images().empty(),
            "all main instances retire in ascending order before global XTA append order");
    require(f.groups.find(unreached.range) && f.handles.handle_live(unreached.textures.slots[0].handle) &&
            f.handles.reserved_handles() == reserves,
            "main pass never sweeps unreached groups or unused handle reservations");
    f.groups.retire_main();
    require(retired(f.handles) == expected, "repeated completed main/XTA pass is inert");
    // Source 1b6f08 clears active flags during the main pass, before 1010f4.
    f.groups.bind_instance(0, unreached.range); f.groups.bind_instance(639, unreached.range);
    f.groups.retire_instance(0); f.groups.retire_instance(639);
}
void explicit_unused_retirement_and_reset() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    const auto embedded0 = f.groups.insert("embedded0", 1, {}, false).range;
    const auto embedded1 = f.groups.insert("embedded1", 1, {}, false).range;
    const auto& unused = f.groups.insert("zero-placement-definition", 2, f.load({"unused"}));
    const auto& active = f.groups.insert("actor", 1, f.load({"active"}));
    f.groups.register_actor("ACTOR", {active.range,{}}); f.groups.bind_instance(8,active.range);
    const auto free_before = f.groups.ranges().free_ranges();
    const auto handles_before = f.handles.live_handle_count();
    f.handles.clear_events(); f.groups.reset_instances_and_registrations();
    require(!f.groups.actor("actor") && f.groups.ranges().free_ranges() == free_before &&
            f.groups.find(embedded0) && f.groups.find(embedded1) && f.groups.find(unused.range) &&
            f.groups.find(active.range) && f.handles.live_handle_count() == handles_before && retired(f.handles).empty(),
            "1b8028 clears registrations/instances but does not reset pool, sweep groups or retire textures");
    f.groups.retire_associated(); f.groups.retire_main();
    require(retired(f.handles).empty() && f.groups.find(unused.range) && f.groups.find(active.range),
            "unreached groups survive after reset rather than being blanket swept");
    require(f.groups.retire(unused.range) && !f.groups.retire(unused.range),
            "map zero-placement group has an explicit, once-only retirement boundary");
    require(retired(f.handles) == std::vector<int>{unused.textures.slots[0].handle} &&
            f.groups.find(active.range) && f.groups.find(embedded0) && f.groups.find(embedded1),
            "unused definition cleanup leaves other live and embedded groups intact");
    const auto fresh = f.groups.insert("new-context", 1, {}).range;
    require(fresh.first == unused.range.first, "reset preserves allocator holes for later earliest-range reuse");
    f.groups.bind_instance(8, active.range);
    f.groups.register_actor("actor", {active.range,{}});
    const auto* registered = f.groups.actor("AcToR");
    require(registered && registered->main == active.range, "reset permits fresh registration without altering live group");
}
void normalized_prefix_independent_registration() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    unsigned reads = 0; std::vector<std::string> actual_reads;
    auto reach = [&](std::string prefix, std::string name, unsigned instance) {
        const auto* cached = f.groups.actor(name);
        if (!cached) {
            const std::vector<std::string> names{"body"};
            auto resource = f.textures.load_xmd(names, [&](std::string& file) {
                ++reads; actual_reads.push_back(prefix + file); return std::optional(xim());
            });
            const auto main = f.groups.insert(name,1,std::move(resource)).range;
            f.groups.register_actor(name,{main,{}}); cached=f.groups.actor(name);
        }
        const auto lease = cached->main; f.groups.bind_instance(instance,lease,cached->shadow); return lease;
    };
    const auto field = reach("flddata/obj/", "HERO", 1);
    const auto count = f.handles.live_handle_count();
    const auto group_count = f.groups.groups().size();
    const auto battle = reach("btldata/beta2/unit/", "hErO", 2);
    require(field == battle && reads == 1 && actual_reads == std::vector<std::string>{"flddata/obj/body.xim"} &&
            f.handles.live_handle_count() == count && f.groups.groups().size() == group_count,
            "case-normalized registration hit across prefixes shares group without another load or alias");
    const auto* registered = f.groups.actor("hero");
    require(registered && !registered->shadow && !f.groups.actor("flddata/obj/hero"),
            "cache retains first registration's absent shadow and is keyed by bare caller resource name");
    rejects<std::logic_error>([&] { f.groups.register_actor("hero",{field,{}}); },
                              "duplicate case-folded registration must be guarded");
    f.groups.retire_instance(1);
    const auto* stale_cache = f.groups.actor("hero");
    require(stale_cache && stale_cache->main == field && !f.groups.find(field),
            "material retirement alone does not invent source actor-cache invalidation");
    f.groups.retire_instance(2); f.groups.reset_instances_and_registrations();
    const auto next = reach("btldata/beta2/unit/", "hero", 1);
    require(reads == 2 && actual_reads.back() == "btldata/beta2/unit/body.xim" &&
            next.first == field.first && next.serial != field.serial,
            "actual cache reset permits new-prefix load and fresh reuse of freed range");
}
void stale_lease_and_instance_guards() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    const auto old = f.groups.insert("old",2,f.load({"old"})).range;
    require(f.groups.retire(old), "retire original incarnation");
    const auto& current = f.groups.insert("current",2,f.load({"current"}));
    const auto lease = current.range; const int handle = current.textures.slots[0].handle;
    require(lease.first == old.first && lease.serial != old.serial, "fixture reuses retail ID with new lease");
    ModelRangeAllocator unrelated; const auto foreign = *unrelated.allocate(2);
    auto altered = lease; --altered.count;
    f.handles.clear_events(); const auto free_before = f.groups.ranges().free_ranges();
    for (const auto bad : {old,foreign,altered,ModelRangeLease{}}) {
        require(!f.groups.find(bad) && !f.groups.retire(bad), "stale/foreign/altered lease must not delete live replacement");
        rejects<std::logic_error>([&] { f.groups.bind_instance(0,bad); }, "invalid main lease rejected");
        rejects<std::logic_error>([&] { f.groups.bind_instance(0,lease,bad); }, "invalid auxiliary lease rejected");
        rejects<std::logic_error>([&] { f.groups.register_actor("bad",{bad,{}}); }, "invalid cache main rejected");
        rejects<std::logic_error>([&] { f.groups.register_actor("bad",{lease,bad}); }, "invalid cache auxiliary rejected");
    }
    require(f.groups.find(lease) && f.handles.handle_live(handle) && retired(f.handles).empty() &&
            f.groups.ranges().free_ranges() == free_before && !f.groups.actor("bad"),
            "guard failures mutate neither allocations, current textures nor registration cache");
    f.groups.bind_instance(0,lease);
    rejects<std::logic_error>([&] { f.groups.bind_instance(0,lease); }, "occupied instance cannot be silently replaced");
    rejects<std::logic_error>([&] { f.groups.bind_instance(640,lease); }, "instance range ends at639");
    rejects<std::out_of_range>([&] { f.groups.retire_instance(640); }, "invalid retirement index rejected");
    f.groups.retire_instance(0);
    require(retired(f.handles) == std::vector<int>{handle}, "successful current cleanup remains possible after guards");
}
void source_instance_cursors() {
    auto fixture = std::make_unique<Fixture>(); auto& f = *fixture;
    auto group=f.groups.insert("actor",1,{}).range;
    require(f.groups.allocate_instance(group,{},0)==0 &&
            f.groups.allocate_instance(group,{},0x100000)==639 &&
            f.groups.allocate_instance(group,{},0)==1,"1b8a80 opposing cursors follow source flags");
    f.groups.retire_instance(0);
    group=f.groups.insert("replacement",1,{}).range;
    require(f.groups.allocate_instance(group,{},0)==2,"individual deletion does not rewind the instance cursor");
    f.groups.reset_instances_and_registrations();
    require(f.groups.find(group),"instance reset does not reset model ranges");
    for(unsigned i=0;i<639;++i)require(f.groups.allocate_instance(group,{},0)==i,"front cursor sequence");
    rejects<std::runtime_error>([&]{f.groups.allocate_instance(group,{},0x100000);},
                               "source backward allocation requires front < decremented back");
    require(f.groups.allocate_instance(group,{},0)==639,"source front allocation can consume the last slot");
    rejects<std::runtime_error>([&]{f.groups.allocate_instance(group,{},0);},"front pool exhaustion is explicit");
}
void source_shadow_selectors() {
    auto fixture=std::make_unique<Fixture>();auto& f=*fixture;
    const auto main=f.groups.insert("cached-main",1,{}).range;
    const auto shadow=f.groups.insert("cached-shadow",1,{},false).range;
    const auto zero=f.groups.allocate_instance(main,shadow,0x80);
    const auto one=f.groups.allocate_instance(main,shadow,0xa0);
    const auto two=f.groups.allocate_instance(main,shadow,0xc0);
    const auto three=f.groups.allocate_instance(main,shadow,0xe0);
    require(!f.groups.instances()[zero]->shadow&&!f.groups.instances()[one]->shadow&&
            f.groups.instances()[two]->shadow==shadow&&f.groups.instances()[three]->shadow==shadow,
            "1b8c04 associates cached shadows only for selectors2/3, not0/1");
    f.groups.retire_instance(zero);f.groups.retire_instance(one);
    require(f.groups.find(shadow),"selector0/1 child cleanup cannot retire a merely cached shadow");
    f.groups.retire_instance(two);
    require(!f.groups.find(shadow),"first associated selector2 child actually retires the shadow group");
    f.groups.retire_instance(three);
}
}
int main() {
    try {
        bootstrap_and_ranges();
        exhaustion_preserves_reached_texture_load();
        shared_instances_resolve_current_numeric_group();
        individual_associated_before_main();
        associated_and_main_passes_resolve_current_ids();
        associated_main_and_xta_order();
        explicit_unused_retirement_and_reset();
        normalized_prefix_independent_registration();
        stale_lease_and_instance_guards();
        source_instance_cursors();
        source_shadow_selectors();
        std::cout << "model group tests passed (11 source ownership scenarios)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
