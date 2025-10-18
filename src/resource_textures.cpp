#include "okage/resource_textures.hpp"

#include <limits>
#include <stdexcept>

namespace okage {
namespace {
std::string xta_name(std::string_view row) {
    std::string result(row);
    if (result.empty()) throw std::invalid_argument("XTA '=' image name is empty");
    for (auto& c : result) {
        if (static_cast<unsigned char>(c) >= 128)
            throw std::invalid_argument("XTA non-ASCII name is unrecovered");
        if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    }
    return result;
}

std::int32_t xta_group(std::string_view row) {
    std::size_t at = 0;
    while (at < row.size() && row[at] == ' ') ++at;
    bool negative = false;
    if (at < row.size() && (row[at] == '-' || row[at] == '+')) negative = row[at++] == '-';
    std::uint64_t value = 0;
    const std::uint64_t limit = std::uint64_t(std::numeric_limits<std::int32_t>::max()) + negative;
    while (at < row.size() && row[at] >= '0' && row[at] <= '9') {
        const auto digit = unsigned(row[at++] - '0');
        if (value > (limit - digit) / 10) throw std::invalid_argument("XTA group integer overflow is unrecovered");
        value = value * 10 + digit;
    }
    return static_cast<std::int32_t>(negative ? -std::int64_t(value) : std::int64_t(value));
}
} // namespace

ResourceTextureSlot ResourceTextureOwner::describe(int handle) const {
    ResourceTextureSlot slot;
    slot.handle = handle;
    slot.handle_serial = handles_.handle_serial(handle);
    slot.record = handles_.record_of(handle);
    slot.image = handles_.image(handle);
    if (slot.image) {
        const auto found = payloads_.find({slot.record, slot.image->id, slot.image->serial});
        if (found != payloads_.end()) slot.bytes = found->second.lock();
    }
    return slot;
}

void ResourceTextureOwner::remember(int handle, const ImmutableTextureBytes& bytes) {
    if (const auto image = handles_.image(handle))
        payloads_[{handles_.record_of(handle), image->id, image->serial}] = bytes;
}

ResourceTextures ResourceTextureOwner::load_xmd(std::span<const std::string> names,
                                              const TextureHandleOwner::FileReader& reader) {
    ResourceTextures resource;
    const TextureHandleOwner::FileReader capture = [&](std::string& name) {
        auto bytes = reader(name);
        resource.xmd_reads.push_back(bytes ? std::make_shared<const std::vector<std::uint8_t>>(*bytes) : nullptr);
        return bytes;
    };
    resource.xmd = handles_.load_xmd_names(names, capture); // 186c48 -> 186aa8
    std::size_t read = 0;
    // Remember even a prefix of an aborted load; 186aa8 does not roll it back.
    for (const auto& outcome : resource.xmd.names)
        if (outcome.kind != XmdNameOutcome::Kind::CacheHit) remember(outcome.handle, resource.xmd_reads.at(read++));
    if (resource.xmd.first >= 0) {
        int handle = resource.xmd.first;
        for (std::size_t i = 0; i < names.size(); ++i) {
            auto slot = describe(handle);
            resource.slots.push_back(std::move(slot));
            handle = handles_.live_successor(handle);
        }
    }
    return resource;
}

bool ResourceTextureOwner::load_xta(ResourceTextures& resource, ImmutableTextureBytes sidecar,
                                   const TextureHandleOwner::FileReader& reader) {
    if (resource.xta_attempted) throw std::logic_error("XTA sidecar already attempted for this resource");
    resource.xta_attempted = true;
    if (!sidecar || sidecar->empty()) return false; // 1b6898 read <= 0 precedes reservation
    resource.xta_present = true;
    resource.xta_bytes = std::move(sidecar);
    // 1b68c4's return is not tested. Continue even if reservation failed.
    resource.xta_reservation = handles_.reserve_handles(16);
    resource.xta_first = -1;
    const auto& bytes = *resource.xta_bytes;
    std::size_t at = 0;
    while (at < bytes.size() && bytes[at]) {
        while (at < bytes.size() && bytes[at] && bytes[at] < 32) ++at;
        if (at == bytes.size() || !bytes[at]) break;
        const auto begin = at;
        while (at < bytes.size() && bytes[at] >= 32) {
            if (bytes[at] >= 128) throw std::invalid_argument("XTA non-ASCII row is unrecovered");
            ++at;
        }
        const std::string_view row(reinterpret_cast<const char*>(bytes.data() + begin), at - begin);
        if (row.size() > 255) throw std::invalid_argument("XTA row exceeds source 256-byte buffer");
        if (row.front() == '=') {
            auto name = xta_name(row.substr(1));
            auto read = reader(name);
            if (!read) throw std::runtime_error("XTA '=' XIM read failed: " + name);
            auto payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(*read));
            const int handle = handles_.load_xim(OwnedRawXim(*payload)).handle;
            remember(handle, payload);
            auto slot = describe(handle);
            slot.bytes = std::move(payload);
            scene_images_.push_back(slot);
            resource.xta_variants.push_back(std::move(slot));
            if (resource.xta_first == -1) resource.xta_first = handle; // 1b6908..1b6914
        } else {
            if (resource.xta_groups.size() == 8) throw std::invalid_argument("XTA exceeds eight source group pointers");
            resource.xta_groups.push_back(xta_group(row)); // 1b6928 -> 1b7430
        }
    }
    return true;
}

void ResourceTextureOwner::retire(const ResourceTextureSlot& slot) {
    if (handles_.handle_live(slot.handle) &&
        (handles_.handle_serial(slot.handle) != slot.handle_serial ||
         handles_.record_of(slot.handle) != slot.record || handles_.image(slot.handle) != slot.image))
        throw std::logic_error("Resource texture handle was reused before retirement");
    handles_.retire(slot.handle);
}

void ResourceTextureOwner::retire_xmd(ResourceTextures& resource) {
    if (resource.xmd_retired) return;
    for (const auto& slot : resource.slots) retire(slot); // 1796e4..179704
    resource.xmd_retired = true;
}

void ResourceTextureOwner::retire_scene_images() {
    for (const auto& slot : scene_images_) retire(slot); // 1b60ec..1b6110
    scene_images_.clear();
}
} // namespace okage
