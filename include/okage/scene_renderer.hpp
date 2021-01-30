#pragma once
#include "okage/pose.hpp"
#include "okage/actor.hpp"
#include "okage/effect.hpp"
#include <filesystem>
#include <memory>
namespace okage {
struct SceneActor {const FieldActor* actor;std::filesystem::path model_path;};
class SceneRenderer {
public:
    explicit SceneRenderer(const std::filesystem::path& map,std::span<const SceneActor> actors={});
    ~SceneRenderer();
    void load_effect_textures(const std::filesystem::path&);
    Texture render(const Matrix4& view,const Float4& projection,const AttachedEffects* effects=nullptr,const Float4& camera_rotation={});
    std::size_t placements() const;
    std::size_t omitted_effects() const;
    std::size_t actors() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
