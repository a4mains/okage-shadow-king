#pragma once
#include "okage/pose.hpp"
#include "okage/actor.hpp"
#include <filesystem>
#include <memory>
namespace okage {
class SceneRenderer {
public:
    explicit SceneRenderer(const std::filesystem::path& map,const FieldActor* actor=nullptr,
                           const std::filesystem::path& actor_directory={});
    ~SceneRenderer();
    Texture render(const Matrix4& view,const Float4& projection);
    std::size_t placements() const;
    std::size_t omitted_effects() const;
    std::size_t actors() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
