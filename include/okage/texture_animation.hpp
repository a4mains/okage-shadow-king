#pragma once
#include "okage/model.hpp"
#include <string_view>

namespace okage {
struct TextureAnimation {
    std::vector<std::int16_t> groups;
    std::vector<std::string> variants; // One-based XMT choices; zero restores.
};
TextureAnimation read_xta(std::string_view text);

class TextureBindings {
public:
    TextureBindings(const ModelFile& model, TextureAnimation animation);
    void select(std::size_t group, std::size_t variant);
    void restore();
    const std::vector<std::int32_t>& images() const { return current_; }
    const std::vector<std::string>& names() const { return names_; }
    const TextureAnimation& animation() const { return animation_; }
private:
    TextureAnimation animation_;
    std::vector<std::string> names_;
    std::vector<std::int32_t> original_, current_;
    std::size_t base_count_;
};
}
