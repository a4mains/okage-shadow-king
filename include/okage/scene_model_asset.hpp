#pragma once
#include "okage/model.hpp"
#include "okage/texture_animation.hpp"
#include "okage/resource_textures.hpp"
#include "okage/model_ranges.hpp"

namespace okage {
struct SceneModelAsset {
    ModelFile model;
    ImmutableTextureBytes motion;
    TextureAnimation animation;
    std::vector<ResourceTextureSlot> textures;
    std::optional<ModelRangeLease> resource_group, shadow_group;
    std::shared_ptr<const ModelFile> shadow;
    std::shared_ptr<TextureBindings> packet_textures;
};
using SceneModelAssets = std::vector<std::shared_ptr<const SceneModelAsset>>;
}
