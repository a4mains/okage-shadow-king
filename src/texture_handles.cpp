#include "okage/texture_handles.hpp"

#include <algorithm>
#include <stdexcept>

#include "okage/texture_transfer.hpp"

namespace okage {
namespace {

std::string lower_ascii(std::string_view s, std::size_t limit = std::string::npos) {
    std::string out;
    for (char c : s) {
        if (out.size() >= limit) break;
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

void require_supported_name(std::string_view s) {
    for (unsigned char c : s)
        if (c == 0 || c >= 0x80)
            throw std::invalid_argument("texture name contains a NUL or non-ASCII byte (unsupported domain)");
}

std::uint32_t byte_sum(const std::string& s) {
    std::uint32_t sum = 0;
    for (unsigned char c : s) sum += c;
    return sum;
}

} // namespace

TextureHandleOwner::TextureHandleOwner(Ps2TextureMemory& memory) : memory_(&memory) {
    map_.fill(-1);
    extra_next_.fill(-1);
    hash_.fill(0);
    for (int i = 0; i < record_capacity; ++i) free_records_.push_back(i);
    for (int i = 0; i < extra_palette_capacity; ++i) free_extras_.push_back(i);
    for (auto& r : records_) r.name[0] = 0;
}

void TextureHandleOwner::validate(const OptDescriptor& d) const {
    if (!d) return;
    const auto a = memory_->allocation(d->id);
    if (!a || a->serial != d->serial) throw std::logic_error("texture descriptor is stale");
}

OptDescriptor TextureHandleOwner::retain(OptDescriptor d) {
    if (!d) return std::nullopt;
    const auto a = memory_->allocation(d->id);
    if (!a || a->serial != d->serial) throw std::logic_error("texture descriptor is stale");
    memory_->adjust_references(d->id, 1);
    log_event({HandleEvent::Kind::DescriptorRetain, d->id, memory_->allocation(d->id)->references});
    return d;
}

void TextureHandleOwner::release(OptDescriptor d) {
    if (!d) return;
    const auto a = memory_->allocation(d->id);
    if (!a || a->serial != d->serial) throw std::logic_error("texture descriptor is stale");
    memory_->adjust_references(d->id, -1);
    log_event({HandleEvent::Kind::DescriptorRelease, d->id, memory_->allocation(d->id)->references});
    if (memory_->free_allocation(d->id))
        log_event({HandleEvent::Kind::DescriptorFree, d->id, 0});
}

int TextureHandleOwner::alloc_range(int count) {
    int run = 0;
    for (int i = 0; i < handle_capacity; ++i) {
        run = id_used_[i] ? 0 : run + 1;
        if (run == count) {
            const int start = i - count + 1;
            for (int k = start; k <= i; ++k) id_used_[k] = true;
            return start;
        }
    }
    return -1;
}

void TextureHandleOwner::free_id(int id) { id_used_[id] = false; }

int TextureHandleOwner::pick_handle_id() {
    if (!reserved_.empty()) return reserved_.front();
    for (int i = 0; i < handle_capacity; ++i)
        if (!id_used_[i]) return i;
    return -1;
}

int TextureHandleOwner::take_handle_id() {
    if (!reserved_.empty()) {
        const int id = reserved_.front();
        reserved_.pop_front();
        return id;
    }
    return alloc_range(1);
}

int TextureHandleOwner::create_handle(OptDescriptor image) {
    if (free_records_.empty()) return -1;
    if (pick_handle_id() < 0) return -1;
    if (image) {
        const auto a = memory_->allocation(image->id);
        if (!a || a->serial != image->serial) throw std::logic_error("texture descriptor is stale");
    }
    const int record = free_records_.front();
    free_records_.pop_front();
    const int id = take_handle_id();
    map_[id] = static_cast<std::int16_t>(record);
    auto& r = records_[record];
    r = Record{};
    r.live = true;
    r.handles = 1;
    r.image = retain(image);
    ++live_handles_;
    log_event({HandleEvent::Kind::HandleCreated, id, record});
    return id;
}

int TextureHandleOwner::create_alias(int record) {
    if (pick_handle_id() < 0) return -1;
    const int id = take_handle_id();
    map_[id] = static_cast<std::int16_t>(record);
    ++records_[record].handles;
    ++live_handles_;
    log_event({HandleEvent::Kind::AliasCreated, id, record});
    return id;
}

int TextureHandleOwner::create_image(GsPixelStorage psm, std::uint16_t width, std::uint16_t height,
                                     std::span<const std::uint8_t> pixels) {
    if (psm != GsPixelStorage::Psmct32 && psm != GsPixelStorage::Psmt8 && psm != GsPixelStorage::Psmt4 &&
        psm != GsPixelStorage::Psmt4Hl && psm != GsPixelStorage::Psmt4Hh)
        throw std::invalid_argument("texture handle: unsupported pixel storage");
    if (!width || !height || width > 2048 || height > 2048)
        throw std::invalid_argument("texture handle: dimension outside 1..2048");
    const auto qwc = retail_transfer_qwc(static_cast<std::uint32_t>(psm), width, height);
    if (!qwc) throw std::invalid_argument("texture handle: image smaller than one quadword");
    if (!pixels.empty() && pixels.size() < std::size_t(qwc) * 16)
        throw std::invalid_argument("texture handle: pixel data shorter than the quadwords sent");
    const auto a = memory_->allocate(1, psm, width, height);
    if (!a) return -1;
    log_allocation(*a);
    const TextureDescriptor d{a->id, a->serial};
    const int handle = create_handle(d);
    if (handle < 0) {
        if (memory_->free_allocation(a->id))
            log_event({HandleEvent::Kind::DescriptorFree, a->id, 0});
        return -1;
    }
    if (!pixels.empty()) {
        upload_allocation_image(*memory_, a->id, pixels);
        log_event({HandleEvent::Kind::Upload, a->id, std::int64_t(qwc) * 16});
    }
    return handle;
}

OptDescriptor TextureHandleOwner::create_palette(std::uint32_t count,
                                                 std::span<const std::uint8_t> entries,
                                                 bool& uploaded) {
    uploaded = false;
    if (count == 0 || count > 512) throw std::invalid_argument("texture handle: palette entry count outside 1..512");
    if (entries.empty()) {
        const auto a = memory_->allocate(0, GsPixelStorage::Psmct32, 16,
                                         static_cast<std::uint16_t>(clut_rect_rows(count)));
        if (!a) return std::nullopt;
        log_allocation(*a);
        return TextureDescriptor{a->id, a->serial};
    }
    if (entries.size() < std::size_t(count) * 4)
        throw std::invalid_argument("texture handle: palette source shorter than 4 bytes per entry");
    return make_palette(count, entries, uploaded);
}

OptDescriptor TextureHandleOwner::make_palette(std::uint32_t count,
                                               std::span<const std::uint8_t> level_bytes,
                                               bool& uploaded) {
    uploaded = false;
    const auto a = memory_->allocate(0, GsPixelStorage::Psmct32, 16,
                                     static_cast<std::uint16_t>(clut_rect_rows(count)));
    if (!a) return std::nullopt;  // 1cd7d0 returns 0 and uploads nothing
    log_allocation(*a);
    upload_clut(*memory_, a->id, count, level_bytes);
    uploaded = true;
    log_event({HandleEvent::Kind::Upload, a->id, std::int64_t(clut_rect_rows(count)) * 64});
    return TextureDescriptor{a->id, a->serial};
}

XimLoadReport TextureHandleOwner::load_xim(const OwnedRawXim& xim) {
    const auto& l = xim.layout();
    const auto support = xim_upload_support(l);
    if (support.status != XimUploadStatus::Supported)
        throw std::invalid_argument("texture handle: XIM upload unsupported: " + support.reason);
    XimLoadReport report;
    report.handle = create_image(static_cast<GsPixelStorage>(l.pixel_format),
                                 static_cast<std::uint16_t>(l.width),
                                 static_cast<std::uint16_t>(l.height), xim.image());
    if (report.handle < 0) return report;
    const bool indexed = l.pixel_format == 19 || l.pixel_format == 20;
    for (std::uint32_t i = 0; indexed && i < l.palette_level_count; ++i) {
        PaletteLevelOutcome outcome;
        outcome.level = i;
        outcome.palette = make_palette(l.palette_color_count, xim.palette_level(i), outcome.uploaded);
        outcome.bind = bind_palette(report.handle, static_cast<int>(i), outcome.palette);
        report.palettes.push_back(outcome);
    }
    return report;
}

int TextureHandleOwner::reserve_handles(int count) {
    if (count <= 0) throw std::invalid_argument("texture handle: reservation count must be positive");
    for (int id : reserved_) free_id(id);  // phase 1; not undone on failure
    reserved_.clear();
    const int first = alloc_range(count);
    log_event({HandleEvent::Kind::Reserved, count, first});
    if (first < 0) return -1;
    for (int i = 0; i < count; ++i) reserved_.push_back(first + i);
    return first;
}

int TextureHandleOwner::lookup_slot(const std::string& lowered, int* first_free) const {
    const auto sum = byte_sum(lowered);
    int i = static_cast<int>(sum % hash_size);
    const int step = static_cast<int>(((~sum) & 0xfu) << 1) + 1;
    if (first_free) *first_free = -1;
    for (int probes = 0; probes < hash_size; ++probes) {
        const int e = hash_[i];
        if (e > 0) {
            const auto& stored = records_[e - 8].name;
            if (lowered == stored.data()) return i;
        } else {
            if (first_free && *first_free < 0) *first_free = i;
            if (e == 0) return -1;
        }
        i = (i + step) % hash_size;
    }
    throw std::logic_error("texture name hash table has no empty slot");
}

void TextureHandleOwner::set_name(int handle, const char* name) {
    if (name) require_supported_name(name);
    if (handle < 0 || handle >= handle_capacity || map_[handle] < 0) return;
    {
        HandleEvent named{HandleEvent::Kind::Named, handle, name ? 1 : 0};
        if (name) named.text = name;
        log_event(named);
    }
    const int r = map_[handle];
    auto& rec = records_[r];
    if (rec.hash_slot >= 0) {
        hash_[rec.hash_slot] = -1;
        rec.hash_slot = -1;
    }
    rec.name[0] = 0;
    if (!name) return;
    const auto lowered = lower_ascii(name, name_chars);
    std::copy(lowered.begin(), lowered.end(), rec.name.begin());
    rec.name[lowered.size()] = 0;
    int first_free = -1;
    const int found = lookup_slot(lowered, &first_free);
    if (found >= 0) {
        const int old_record = hash_[found] - 8;
        rec.hash_slot = records_[old_record].hash_slot;
        records_[old_record].hash_slot = -1;
    } else {
        rec.hash_slot = static_cast<std::int16_t>(first_free);
    }
    hash_[rec.hash_slot] = static_cast<std::int16_t>(r + 8);
}

int TextureHandleOwner::lookup_alias(std::string_view name) {
    require_supported_name(name);
    const auto lowered = lower_ascii(name, name_chars);
    const int slot = lookup_slot(lowered, nullptr);
    if (slot < 0) return -1;
    return create_alias(hash_[slot] - 8);
}

void TextureHandleOwner::retire(int handle) {
    if (handle < 0 || handle >= handle_capacity || map_[handle] < 0) return;
    log_event({HandleEvent::Kind::RetireCalled, handle, 0});
    const int r = map_[handle];
    auto& rec = records_[r];
    if (rec.handles == 1) {
        validate(rec.image);
        validate(rec.palette0);
        for (int e = rec.extra_head; e != -1; e = extra_next_[e]) validate(extra_desc_[e]);
    }
    --rec.handles;
    if (static_cast<std::int16_t>(rec.handles) <= 0) {
        if (rec.hash_slot >= 0) hash_[rec.hash_slot] = -1;  // slot field stays stale
        int e = rec.extra_head;
        rec.extra_head = -1;
        while (e != -1) {
            release(extra_desc_[e]);
            extra_desc_[e] = std::nullopt;
            free_extras_.push_back(e);
            log_event({HandleEvent::Kind::ExtraFreed, e, 0});
            const int next = extra_next_[e];
            extra_next_[e] = -1;
            e = next;
        }
        release(rec.image);
        release(rec.palette0);
        rec.live = false;
        free_records_.push_back(r);
        log_event({HandleEvent::Kind::RecordFreed, r, 0});
    }
    map_[handle] = -1;
    free_id(handle);
    --live_handles_;
    log_event({HandleEvent::Kind::HandleFreed, handle, r});
}

PaletteBindResult TextureHandleOwner::bind_palette(int handle, int level, OptDescriptor palette) {
    if (handle < 0 || handle >= handle_capacity || map_[handle] < 0) return PaletteBindResult::InvalidHandle;
    if (level < 0) throw std::invalid_argument("texture handle: negative palette level is undefined in retail");
    auto& rec = records_[map_[handle]];
    validate(palette);
    log_event({HandleEvent::Kind::PaletteBind, handle, level, palette ? std::int64_t(palette->id) : -1});
    if (level == 0) {
        if (rec.palette0 == palette) return PaletteBindResult::Unchanged;
        release(rec.palette0);
        rec.palette0 = retain(palette);
        return PaletteBindResult::Bound;
    }
    std::int16_t* cell = &rec.extra_head;
    int e = -1;
    for (int k = 0; k < level; ++k) {
        e = *cell;
        if (e == -1) {
            if (free_extras_.empty()) return PaletteBindResult::ExtraPoolExhausted;
            const int fresh = free_extras_.front();
            free_extras_.pop_front();
            extra_next_[fresh] = -1;
            *cell = static_cast<std::int16_t>(fresh);
        }
        e = *cell;
        cell = &extra_next_[e];
    }
    if (extra_desc_[e] == palette) return PaletteBindResult::Unchanged;
    release(extra_desc_[e]);
    extra_desc_[e] = retain(palette);
    return PaletteBindResult::Bound;
}

OptDescriptor TextureHandleOwner::palette(int handle, int level) const {
    if (handle < 0 || handle >= handle_capacity || map_[handle] < 0 || level < 0) return std::nullopt;
    const auto& rec = records_[map_[handle]];
    if (level == 0) return rec.palette0;
    int e = rec.extra_head;
    for (int k = 1; k < level && e != -1; ++k) e = extra_next_[e];
    return e == -1 ? std::nullopt : extra_desc_[e];
}

OptDescriptor TextureHandleOwner::image(int handle) const {
    if (handle < 0 || handle >= handle_capacity || map_[handle] < 0) return std::nullopt;
    return records_[map_[handle]].image;
}

int TextureHandleOwner::extra_chain_length(int handle) const {
    if (handle < 0 || handle >= handle_capacity || map_[handle] < 0) return 0;
    int n = 0;
    for (int e = records_[map_[handle]].extra_head; e != -1; e = extra_next_[e]) ++n;
    return n;
}

XmdLoadResult TextureHandleOwner::load_xmd_names(std::span<const std::string> names,
                                                 const FileReader& read_file) {
    for (const auto& name : names) require_supported_name(name);  // before the reservation
    XmdLoadResult result;
    if (names.empty()) {
        result.first = 0;  // 186af8: no reservation, returns 0
        return result;
    }
    result.reservation = reserve_handles(static_cast<int>(names.size()));
    if (result.reservation < 0) return result;
    for (const auto& name : names) {
        XmdNameOutcome outcome;
        outcome.file_name = (lower_file_names_ ? lower_ascii(name) : name) + ".xim";
        int handle = -1;
        if (cache_enabled_) handle = lookup_alias(outcome.file_name);
        if (handle >= 0) {
            outcome.kind = XmdNameOutcome::Kind::CacheHit;
        } else {
            const auto bytes = read_file(outcome.file_name);
            if (!bytes) {
                result.aborted = true;
                result.first = -1;
                return result;
            }
            handle = load_xim(OwnedRawXim(*bytes)).handle;
            set_name(handle, outcome.file_name.c_str());
            outcome.kind = handle >= 0 ? XmdNameOutcome::Kind::Loaded
                                       : XmdNameOutcome::Kind::LoadReturnedFailure;
        }
        outcome.handle = handle;
        if (result.first < 0) result.first = handle;
        result.names.push_back(std::move(outcome));
    }
    return result;
}

void TextureHandleOwner::log_allocation(const GsTextureAllocation& a) {
    log_event({HandleEvent::Kind::Allocate, a.id, a.base_block,
               std::int64_t(a.allocation_class) << 56 | std::int64_t(a.psm) << 48 |
                   std::int64_t(a.width) << 24 | std::int64_t(a.height)});
}

void TextureHandleOwner::log_event(const HandleEvent& e) {
    if (event_limit_ == 0) return;
    if (events_.size() >= event_limit_) {
        events_.pop_front();
        ++events_dropped_;
    }
    events_.push_back(e);
}

void TextureHandleOwner::set_event_log_limit(std::size_t limit) {
    event_limit_ = limit;
    while (events_.size() > event_limit_) {
        events_.pop_front();
        ++events_dropped_;
    }
}

void TextureHandleOwner::retire_all() {
    for (int id : reserved_) free_id(id);
    reserved_.clear();
    for (int h = 0; h < handle_capacity; ++h)
        if (map_[h] >= 0) retire(h);
}

bool TextureHandleOwner::handle_live(int handle) const {
    return handle >= 0 && handle < handle_capacity && map_[handle] >= 0;
}
int TextureHandleOwner::record_of(int handle) const {
    return handle >= 0 && handle < handle_capacity ? map_[handle] : -1;
}
int TextureHandleOwner::live_record_count() const noexcept {
    int n = 0;
    for (const auto& r : records_) n += r.live;
    return n;
}
std::uint16_t TextureHandleOwner::record_handle_count(int record) const { return records_.at(record).handles; }
std::string TextureHandleOwner::record_name(int record) const { return records_.at(record).name.data(); }
int TextureHandleOwner::record_hash_slot(int record) const { return records_.at(record).hash_slot; }
std::vector<int> TextureHandleOwner::reserved_handles() const { return {reserved_.begin(), reserved_.end()}; }
std::vector<std::pair<int, int>> TextureHandleOwner::hash_entries() const {
    std::vector<std::pair<int, int>> out;
    for (int i = 0; i < hash_size; ++i)
        if (hash_[i] != 0) out.emplace_back(i, hash_[i]);
    return out;
}

} // namespace okage
