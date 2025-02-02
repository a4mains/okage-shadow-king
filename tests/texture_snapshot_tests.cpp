#include "okage/texture_memory.hpp"
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace okage;
namespace {
void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
constexpr auto C32 = GsPixelStorage::Psmct32;
Ps2TextureMemory fresh() { Ps2TextureMemory m; m.initialize_empty_allocator(); return m; }
const std::array<std::uint8_t, 4> A{1, 2, 3, 4};
const std::array<std::uint8_t, 4> B{9, 8, 7, 6};
}

int main() {
    try {
        // Write after snapshot.
        {
            auto m = fresh();
            m.write_bytes(100, A);
            auto s = m.snapshot();
            require(s.shares_storage_with(m), "Snapshot shares storage until first write");
            m.write_bytes(100, B);
            m.write_bytes(500, A);
            require(!s.shares_storage_with(m), "First write detaches");
            require(s.observe_byte(100).value == 1 && s.observe_byte(103).value == 4,
                    "Snapshot bytes unchanged");
            require(!s.observe_byte(500).value, "Snapshot definedness unchanged");
            require(m.observe_byte(100).value == 9 && m.observe_byte(500).value == 1,
                    "Live updated");
        }
        // Multiple snapshots at different generations.
        {
            auto m = fresh();
            auto s0 = m.snapshot();
            m.write_bytes(0, A);
            auto s1 = m.snapshot();
            m.write_bytes(0, B);
            auto s2 = m.snapshot();
            m.write_bytes(0, A);
            require(!s0.observe_byte(0).value && s1.observe_byte(0).value == 1 &&
                    s2.observe_byte(0).value == 9 && m.observe_byte(0).value == 1,
                    "Each snapshot keeps its own view");
            require(s0.identity().content_generation == 0 && s1.identity().content_generation == 1 &&
                    s2.identity().content_generation == 2 && m.identity().content_generation == 3,
                    "Snapshot identity at capture");
        }
        // Identity semantics.
        {
            auto m = fresh();
            auto id0 = m.identity();
            require(id0.ownership_epoch == 1 && id0.content_generation == 0, "init advances epoch only");
            auto a = m.allocate(0, C32, 8, 8);
            auto id1 = m.identity();
            require(id1.ownership_epoch == 2 && id1.content_generation == 0, "allocate: epoch only");
            m.write_bytes(0, A);
            auto id2 = m.identity();
            require(id2.content_generation == 1 && id2.ownership_epoch == 2, "write: content only");
            m.write_bytes(0, std::span<const std::uint8_t>{});
            require(m.identity() == id2, "Empty write advances nothing");
            m.retain(a->id);
            require(m.identity().ownership_epoch == 3 && m.identity().content_generation == 1,
                    "retain advances epoch");
            m.release(a->id);  // refs 1 -> 0, then freed: two epoch steps
            require(!m.allocation(a->id), "release freed");
            require(m.identity().ownership_epoch == 5 && m.identity().content_generation == 1,
                    "release advances epoch");
            auto b = m.allocate(0, C32, 8, 8);
            m.retain(b->id);
            auto before = m.identity();
            require(!m.free_allocation(b->id) && m.identity() == before, "failed free: no advance");
            bool threw = false;
            try { m.adjust_references(3000, 1); } catch (const std::out_of_range&) { threw = true; }
            require(threw && m.identity() == before, "failed adjust: no advance");
            require(!m.allocate(9, C32, 8, 8) && m.identity() == before, "failed allocate: no advance");
        }
        // FIFO id reuse at same base: snapshot keeps old incarnation.
        {
            auto m = fresh();
            std::optional<GsTextureAllocation> old;
            for (int i = 0; i < 4096; ++i) {
                auto a = m.allocate(0, C32, 8, 8);
                require(a.has_value(), "pool allocation");
                if (a->id == 7) old = a;
            }
            m.adjust_references(7, 2);
            const auto addr = old->base_block * 256u;
            m.write_bytes(addr, A);
            auto s = m.snapshot();
            m.adjust_references(7, -2);
            require(m.free_allocation(7), "free 7");
            auto again = m.allocate(0, C32, 8, 8);
            require(again && again->id == old->id && again->base_block == old->base_block &&
                    again->descriptor == old->descriptor, "Same id/base/descriptor reused");
            require(again->serial > old->serial, "New serial");
            m.write_bytes(addr, A);  // identical bytes
            auto so = s.allocation(7);
            require(so && so->serial == old->serial && so->references == 2, "Snapshot old incarnation");
            require(m.allocation(7)->serial == again->serial && m.allocation(7)->references == 0,
                    "Live new incarnation");
            require(s.owner_of_byte(addr) == 7u && s.owner_of_lane(addr, 0) == 7u &&
                    s.block_address(7, 0, 0).byte_address == addr, "Snapshot owner queries");
            require(s.available_count() == 0 && m.available_count() == 0, "free state");
            require(s.observe_byte(addr).value == 1, "bytes");
            require(s.identity().ownership_epoch < m.identity().ownership_epoch, "epoch differs");
        }
        // Free after snapshot: snapshot still sees allocation; bytes preserved.
        {
            auto m = fresh();
            auto a = m.allocate(0, C32, 8, 8);
            m.write_bytes(a->base_block * 256u, A);
            auto s = m.snapshot();
            m.adjust_references(a->id, 3);
            require(s.allocation(a->id)->references == 0, "Reference change invisible to snapshot");
            m.adjust_references(a->id, -3);
            require(m.free_allocation(a->id), "free");
            require(!m.allocation(a->id) && !m.owner_of_byte(0), "live freed");
            require(s.allocation(a->id) && s.owner_of_byte(0) == a->id, "snapshot still allocated");
            require(m.observe_byte(0).value == 1 && s.observe_byte(0).value == 1, "free preserves bytes");
            require(s.available_count() == 4095 && m.available_count() == 4096, "queue state");
            require(s.free_queue_ids().size() == 4095 && s.occupancy_bytes()[0] == 0x0f &&
                    m.occupancy_bytes()[0] == 0, "occupancy snapshot");
        }
        // Uninitialised allocator.
        {
            Ps2TextureMemory m;
            m.write_bytes(5, A);
            auto s = m.snapshot();
            require(!s.allocator_initialized() && !s.observe_byte(5).allocation_state_known &&
                    s.observe_byte(5).value == 1, "uninitialised snapshot");
            m.initialize_empty_allocator();
            require(!s.allocator_initialized() && m.allocator_initialized(), "init not visible");
            bool threw = false;
            try { (void)s.allocation(0); } catch (const std::logic_error&) { threw = true; }
            require(threw, "allocation() on unknown state throws as live does");
            require(s.identity().ownership_epoch == 0 && m.identity().ownership_epoch == 1, "epoch");
        }
        // Outlive the original.
        {
            Ps2TextureSnapshot s;
            require(!s.valid(), "default empty");
            {
                auto m = std::make_unique<Ps2TextureMemory>();
                m->initialize_empty_allocator();
                (void)m->allocate(0, C32, 8, 8);
                m->write_bytes(0, A);
                s = m->snapshot();
                m.reset();
            }
            require(s.valid() && s.observe_byte(2).value == 3 && s.owner_of_byte(0) == 0u &&
                    s.allocation(0)->serial == 1, "snapshot survives original");
            bool threw = false;
            try { (void)Ps2TextureSnapshot{}.identity(); } catch (const std::logic_error&) { threw = true; }
            require(threw, "empty snapshot throws");
        }
        // Copy / move semantics.
        {
            auto m = fresh();
            m.write_bytes(0, A);
            Ps2TextureMemory c = m;
            require(c.shares_storage_with(m), "copy shares");
            c.write_bytes(0, B);
            require(m.observe_byte(0).value == 1 && c.observe_byte(0).value == 9 &&
                    !c.shares_storage_with(m), "copy detaches on write");
            Ps2TextureMemory mv = std::move(c);
            require(mv.observe_byte(0).value == 9, "move");
            c = fresh();  // reuse moved-from via assignment
            c.write_bytes(0, A);
            require(c.observe_byte(0).value == 1 && mv.observe_byte(0).value == 9, "reuse moved-from");
            auto src = fresh();
            Ps2TextureMemory tgt = std::move(src);
            src.write_bytes(1, A);  // moved-from write lazily rebuilds planes
            require(src.observe_byte(1).value == 1 && !tgt.observe_byte(1).value,
                    "moved-from write is isolated");
            auto s = m.snapshot();
            auto s2 = s;  // cheap copy
            auto s3 = std::move(s);
            require(!s.valid() && s2.valid() && s3.valid() &&
                    s2.storage_identity() == s3.storage_identity(), "snapshot copy/move");
            auto& alias = m;
            m = alias;  // self-assign harmless
            require(m.observe_byte(0).value == 1, "self assign");
        }
        // Deferred consumption.
        {
            auto m = fresh();
            (void)m.allocate(0, C32, 8, 8);
            m.write_bytes(0, A);
            auto s = m.snapshot();
            const auto cap = s.identity();
            std::function<std::uint64_t()> deferred = [s]() {
                auto o = s.allocation(0);
                return std::uint64_t(s.observe_byte(0).value.value()) * 1000 + o->serial * 10 +
                       o->references;
            };
            s = Ps2TextureSnapshot{};
            for (int i = 0; i < 50; ++i) {
                m.write_bytes(0, B);
                m.adjust_references(0, 1);
            }
            m.adjust_references(0, -50);
            require(m.free_allocation(0), "free");
            auto n = m.allocate(0, C32, 8, 8);
            require(n && n->id == 1, "FIFO continues");
            m.write_bytes(0, B);
            require(deferred() == 1000 + 10 + 0, "Deferred snapshot read is as captured");
            require(m.identity() != cap, "identity moved");
        }
        // Storage not copied when no snapshot outstanding.
        {
            auto m = fresh();
            m.write_bytes(0, A);
            const void* p0 = m.storage_identity();
            m.write_bytes(10, A);
            require(m.storage_identity() == p0, "No snapshot: no copy");
            { auto dead = m.snapshot(); }
            m.write_bytes(20, A);
            require(m.storage_identity() == p0, "Dead snapshot: no copy");
            auto s = m.snapshot();
            m.write_bytes(30, A);
            const void* p1 = m.storage_identity();
            require(p1 != p0 && s.storage_identity() == p0, "Live snapshot: detach once");
            m.write_bytes(40, A);
            require(m.storage_identity() == p1, "Detached once only");
            require(!s.observe_byte(30).value, "snapshot clean");
        }
        std::cout << "texture snapshot tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "texture snapshot tests failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
