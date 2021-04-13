#pragma once
#include "okage/pose.hpp"
#include "okage/actor.hpp"
#include "okage/effect.hpp"
#include <filesystem>
#include <memory>
namespace okage {
class MapObjects;
struct SceneActor {const FieldActor* actor;std::filesystem::path model_path;};
class SceneRenderer {
public:
    explicit SceneRenderer(const std::filesystem::path& map,std::span<const SceneActor> actors={},const MapObjects* objects=nullptr);
    ~SceneRenderer();
    void load_effect_textures(const std::filesystem::path&);
    EffectTextureSizes effect_texture_sizes() const;
    void bind_map_effects(const MapEffects*);
    void set_effect_light_tint(const Float4&);
    Texture render(const Matrix4& view,const Float4& projection,const AttachedEffects* effects=nullptr,const Float4& camera_rotation={},float far_clip=1024,const Float4& camera_position={});
    std::size_t placements() const;
    std::size_t omitted_effects() const;
    std::size_t actors() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
