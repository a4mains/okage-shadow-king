#pragma once
#include "okage/assets.hpp"
#include "okage/shadow.hpp"
#include <array>
#include <optional>

namespace okage {
// Native ownership for the data loaded by 0x1a9190. This initializes resource
// records, not the separate live UI controllers or their rendering callbacks.
struct RuwWidgetRuntime {
    std::array<std::uint32_t,10> words{}; // 0x1a9138, excluding its text pointer.
    std::optional<std::array<char,128>> text;
};
struct RuwWidget {
    std::array<std::uint32_t,17> words{};
    std::optional<RuwWidgetRuntime> runtime;
};
class RuwResource {
public:
    explicit RuwResource(Bytes);
    const std::vector<std::array<std::uint32_t,4>>& styles() const {return styles_;}
    const std::vector<std::vector<RuwWidget>>& templates() const {return templates_;}
    const std::vector<std::vector<RuwWidget>>& widgets() const {return widgets_;}
private:
    std::vector<std::array<std::uint32_t,4>> styles_;
    std::vector<std::vector<RuwWidget>> templates_,widgets_;
};
// 0x197fc0's separate UI projection; it must not replace scene projection.
ShadowScreenParameters initial_ui_projection();
}
