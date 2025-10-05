#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "okage/assets.hpp"
#include "okage/texture_memory.hpp"

namespace okage {

struct TextureDescriptor {
    TextureAllocationId id{};
    std::uint64_t serial{};
    friend bool operator==(const TextureDescriptor&, const TextureDescriptor&) = default;
};
using OptDescriptor = std::optional<TextureDescriptor>;

enum class PaletteBindResult : std::uint8_t {
    InvalidHandle,       // bad/unmapped handle: nothing happens
    Unchanged,
    Bound,
    ExtraPoolExhausted,
};

// Ordered side-effect log for lifecycle comparison with original execution.
struct HandleEvent {
    enum class Kind : std::uint8_t {
        DescriptorRetain, DescriptorRelease, DescriptorFree, RecordFreed,
        ExtraFreed, HandleFreed, HandleCreated, AliasCreated, Upload,
        Allocate, PaletteBind, Named, Reserved, RetireCalled,
    } kind{};
    std::int64_t a{};
    std::int64_t b{};
    std::int64_t c{};
    std::string text;
    friend bool operator==(const HandleEvent&, const HandleEvent&) = default;
};

struct PaletteLevelOutcome {
    std::uint32_t level{};
    OptDescriptor palette;
    bool uploaded{};
    PaletteBindResult bind{};
};
struct XimLoadReport {
    int handle{-1};
    std::vector<PaletteLevelOutcome> palettes;
};

struct XmdNameOutcome {
    enum class Kind : std::uint8_t { CacheHit, Loaded, LoadReturnedFailure } kind{};
    std::string file_name;
    int handle{-1};
};
struct XmdLoadResult {
    int first{-1};
    int reservation{-1};      // 1cd5c8 result (-1 when not attempted)
    bool aborted{};
    std::vector<XmdNameOutcome> names;
};

class TextureHandleOwner {
public:
    static constexpr int handle_capacity = 0x1000;
    static constexpr int record_capacity = 0x200;
    static constexpr int extra_palette_capacity = 0x80;
    static constexpr int hash_size = 0x400;  // max(0x200, 2 * records)
    static constexpr std::size_t name_chars = 19;

    explicit TextureHandleOwner(Ps2TextureMemory& memory);
    TextureHandleOwner(const TextureHandleOwner&) = delete;
    TextureHandleOwner& operator=(const TextureHandleOwner&) = delete;
    Ps2TextureMemory& memory() noexcept { return *memory_; }
    const Ps2TextureMemory& memory() const noexcept { return *memory_; }

    // 0x1cc880. Takes exactly one reference on `image` (null accepted).
    // Returns -1 with no mutation when no record or no handle id is free.
    int create_handle(OptDescriptor image);
    int create_image(GsPixelStorage psm, std::uint16_t width, std::uint16_t height,
                     std::span<const std::uint8_t> pixels);
    // 0x18f7a8: create_image + one 1cd7d0/1cd298 per palette level, continuing
    // after a failed palette allocation (null bound). Unsupported XIMs throw
    // before any state change.
    XimLoadReport load_xim(const OwnedRawXim& xim);

    OptDescriptor create_palette(std::uint32_t count, std::span<const std::uint8_t> entries, bool& uploaded);

    // 0x1cd5c8. Releases the still-unused reserved ids, first-fit allocates n
    // contiguous ids, queues them FIFO. -1 on failure (releases persist).
    // n <= 0 is unguarded in retail and rejected here.
    int reserve_handles(int count);
    // 0x1cbdd8: selects one of two independent reservation queues. Invalid
    // values leave the current class unchanged, exactly as the source setter.
    void set_reservation_class(int value) noexcept;
    unsigned reservation_class() const noexcept { return reservation_class_; }
    // 0x1cc4e0. Null name clears. Lower-cased, 19 chars. Duplicate: newest wins.
    void set_name(int handle, const char* name);
    // 0x1cce78. Hit creates an alias handle (no descriptor change). -1 on miss
    // or handle exhaustion.
    int lookup_alias(std::string_view name);
    // 0x1cca30.
    void retire(int handle);
    // 0x1cd298 / getters 0x1cd500, 0x1cc2b0.
    PaletteBindResult bind_palette(int handle, int level, OptDescriptor palette);
    OptDescriptor palette(int handle, int level) const;
    OptDescriptor image(int handle) const;
    int extra_chain_length(int handle) const;

    // 0x186a50(1): the cache switch only ever turns on (never cleared).
    void enable_name_cache() noexcept { cache_enabled_ = true; }
    bool name_cache_enabled() const noexcept { return cache_enabled_; }
    // 0x186a40(1): lower-cases the file name passed to the reader (names are
    // always lower-cased inside lookup/registration).
    void set_lower_case_file_names(bool on) noexcept { lower_file_names_ = on; }
    bool lower_case_file_names() const noexcept { return lower_file_names_; }

    using FileReader = std::function<std::optional<std::vector<std::uint8_t>>(std::string&)>;
    XmdLoadResult load_xmd_names(std::span<const std::string> names, const FileReader& read_file);

    int live_successor(int handle) const;
    std::vector<int> live_order() const;

    // Inspection.
    bool handle_live(int handle) const;
    int record_of(int handle) const;  // -1 if unmapped
    int live_handle_count() const noexcept { return live_handles_; }
    int live_record_count() const noexcept;
    std::uint16_t record_handle_count(int record) const;
    std::string record_name(int record) const;
    int record_hash_slot(int record) const;
    std::vector<int> reserved_handles() const;
    std::vector<int> reserved_handles(unsigned reservation_class) const;
    std::size_t free_record_count() const noexcept { return free_records_.size(); }
    std::size_t free_extra_count() const noexcept { return free_extras_.size(); }
    std::vector<std::pair<int, int>> hash_entries() const;  // (slot, value)
    void set_event_log_limit(std::size_t limit);
    std::size_t event_log_limit() const noexcept { return event_limit_; }
    std::size_t events_dropped() const noexcept { return events_dropped_; }
    const std::deque<HandleEvent>& events() const noexcept { return events_; }
    void clear_events() { events_.clear(); events_dropped_ = 0; }

    void retire_all();

private:
    struct Record {
        OptDescriptor image, palette0;
        std::array<char, 20> name{};
        std::int16_t hash_slot{-1};
        std::uint32_t user{};
        std::uint16_t handles{};
        std::int16_t extra_head{-1};
        bool live{};
    };
    Ps2TextureMemory* memory_;
    std::array<std::int16_t, handle_capacity> map_;
    std::array<bool, handle_capacity> id_used_{};  // range allocator (HR)
    std::array<std::deque<int>, 2> reserved_;
    unsigned reservation_class_{};
    std::array<Record, record_capacity> records_{};
    std::deque<int> free_records_;
    std::array<OptDescriptor, extra_palette_capacity> extra_desc_{};
    std::array<std::int16_t, extra_palette_capacity> extra_next_;
    std::deque<int> free_extras_;
    std::array<std::int16_t, hash_size> hash_{};
    int live_handles_{};
    bool cache_enabled_{};
    bool lower_file_names_{};
    std::array<std::int16_t, handle_capacity> live_next_, live_prev_;
    int live_head_{-1}, live_tail_{-1};
    void link_live(int id);
    void unlink_live(int id);
    std::deque<HandleEvent> events_;
    std::size_t event_limit_{};
    std::size_t events_dropped_{};
    void log_event(const HandleEvent& e);
    void log_allocation(const GsTextureAllocation& a);

    void validate(const OptDescriptor& d) const;  // throws std::logic_error when stale
    OptDescriptor retain(OptDescriptor d);   // 1cee50(+1)
    void release(OptDescriptor d);           // 1cee50(-1) + 1cf728
    int pick_handle_id();                    // 1cc808 (no pop of reserved head)
    int take_handle_id();
    int alloc_range(int count);
    void free_id(int id);
    int create_alias(int record);            // 1ccd48
    int lookup_slot(const std::string& lowered, int* first_free) const;
    OptDescriptor make_palette(std::uint32_t count, std::span<const std::uint8_t> level_bytes,
                               bool& uploaded);  // 1cd7d0
};

} // namespace okage
