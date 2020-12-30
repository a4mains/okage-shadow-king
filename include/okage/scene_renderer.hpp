#pragma once
#include "okage/pose.hpp"
#include <filesystem>
#include <memory>
namespace okage {
class SceneRenderer {
public:
    explicit SceneRenderer(const std::filesystem::path& map);
    ~SceneRenderer();
    Texture render(const Matrix4& view,const Float4& projection);
    std::size_t placements() const;
    std::size_t omitted_effects() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
