#pragma once
#include "okage/model.hpp"
#include "okage/texture_animation.hpp"
#include "okage/resource_textures.hpp"

namespace okage {
struct SceneModelAsset {
    ModelFile model;
    ImmutableTextureBytes motion;
    TextureAnimation animation;
    std::vector<ResourceTextureSlot> textures;
};
using SceneModelAssets = std::vector<std::shared_ptr<const SceneModelAsset>>;
}
