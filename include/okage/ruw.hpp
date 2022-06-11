#pragma once
#include "okage/assets.hpp"
#include "okage/shadow.hpp"
#include <array>
#include <optional>
#include <string_view>

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
    static constexpr std::int32_t preserve_component=-3000;
    std::size_t widget_index(std::size_t group,std::int32_t authored_id) const;
    const RuwWidget& widget(std::size_t group,std::int32_t authored_id) const;
    void set_class_rgba(std::size_t group,std::int32_t class_id,std::array<std::int32_t,4>);
    std::uint32_t add_class_rgba(std::size_t group,std::int32_t class_id,std::array<std::int32_t,4>);
    std::uint32_t add_rgba(std::size_t group,std::int32_t authored_id,std::array<std::int32_t,4>);
    void reset_class(std::size_t group,std::int32_t class_id);
    void reset_widget(std::size_t group,std::int32_t authored_id);
    void set_text(std::size_t group,std::int32_t authored_id,std::string_view);
    void set_position(std::size_t group,std::int32_t authored_id,std::int32_t x,std::int32_t y);
    void set_runtime_word0(std::size_t group,std::int32_t authored_id,std::uint32_t);

private:
    std::vector<RuwWidget>& mutable_group(std::size_t);
    RuwWidget& mutable_widget(std::size_t,std::int32_t);
    std::vector<std::array<std::uint32_t,4>> styles_;
    std::vector<std::vector<RuwWidget>> templates_,widgets_;
};
// 0x197fc0's separate UI projection; it must not replace scene projection.
ShadowScreenParameters initial_ui_projection();
}
