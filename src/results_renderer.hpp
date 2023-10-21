#pragma once
#include "okage/font.hpp"
#include "okage/dialogue.hpp"
#include "okage/ui_draw.hpp"
#include <memory>
#include <optional>

namespace okage {
class ResultsRenderer {
public:
    ResultsRenderer(std::shared_ptr<const std::vector<Font>> fonts,
                    std::shared_ptr<const Texture> window_atlas);
    ~ResultsRenderer();
    ResultsRenderer(const ResultsRenderer&)=delete;
    ResultsRenderer& operator=(const ResultsRenderer&)=delete;
    const Texture& render(const std::optional<UiDrawList>&,const SceneFade&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
