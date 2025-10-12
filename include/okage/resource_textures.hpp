#pragma once

#include "okage/texture_handles.hpp"

#include <map>
#include <memory>
#include <tuple>

namespace okage {

using ImmutableTextureBytes = std::shared_ptr<const std::vector<std::uint8_t>>;

struct ResourceTextureSlot {
    int handle{-1};
    int record{-1};
    OptDescriptor image;
    ImmutableTextureBytes bytes;
};

struct ResourceTextures {
    ResourceTextures() = default;
    ResourceTextures(const ResourceTextures&) = delete;
    ResourceTextures& operator=(const ResourceTextures&) = delete;
    ResourceTextures(ResourceTextures&&) = default;
    ResourceTextures& operator=(ResourceTextures&&) = delete;

    XmdLoadResult xmd;
    std::vector<ImmutableTextureBytes> xmd_reads;
    std::vector<ResourceTextureSlot> slots;
    bool xmd_retired{};

    bool xta_attempted{}, xta_present{};
    int xta_reservation{-1};
    std::vector<std::int32_t> xta_groups;
    std::vector<ResourceTextureSlot> xta_variants;
    int xta_first{-1};
    ImmutableTextureBytes xta_bytes;
};

class ResourceTextureOwner {
public:
    explicit ResourceTextureOwner(TextureHandleOwner& handles) : handles_(handles) {}
    ResourceTextureOwner(const ResourceTextureOwner&) = delete;
    ResourceTextureOwner& operator=(const ResourceTextureOwner&) = delete;

    ResourceTextures load_xmd(std::span<const std::string> names,
                              const TextureHandleOwner::FileReader& reader);
    bool load_xta(ResourceTextures& resource, ImmutableTextureBytes sidecar,
                  const TextureHandleOwner::FileReader& reader);
    void retire_xmd(ResourceTextures& resource);
    void retire_scene_images();
    const std::vector<ResourceTextureSlot>& scene_images() const noexcept { return scene_images_; }
    ResourceTextureSlot describe(int handle) const;

private:
    TextureHandleOwner& handles_;
    using PayloadKey = std::tuple<int, TextureAllocationId, std::uint64_t>;
    std::map<PayloadKey, std::weak_ptr<const std::vector<std::uint8_t>>> payloads_;
    std::vector<ResourceTextureSlot> scene_images_;
    void remember(int handle, const ImmutableTextureBytes& bytes);
    void retire(const ResourceTextureSlot& slot);
};

} // namespace okage
