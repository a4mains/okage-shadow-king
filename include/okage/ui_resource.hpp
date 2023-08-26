#pragma once

#include "okage/enemy.hpp"
#include "okage/font.hpp"
#include "okage/ui_draw.hpp"
#include "okage/ui_layout.hpp"

#include <memory>
#include <optional>

namespace okage {

struct UiResourceSlotBinding {
    unsigned window_slot;
    unsigned resource_slot;
};

class UiResourceCoordinator {
public:
    using Registry = std::array<std::shared_ptr<UiWindow>, 8>;
    void initialize_scene() {layout_shared_.sprite_groups=0;layout_shared_.layer=0;}

    std::optional<UiDrawList> advance_resource(
        const Registry&, std::span<const UiResourceSlotBinding>,
        std::span<const std::uint32_t, 16> resource_accumulators,
        std::shared_ptr<const std::vector<Font>>, std::span<const float> sine,
        GameRandom&, const ShadowScreenParameters& projection);

    UiLayoutShared& layout_shared() { return layout_shared_; }
    const UiLayoutShared& layout_shared() const { return layout_shared_; }
    UiDrawState& draw_state() { return draw_state_; }
    const UiDrawState& draw_state() const { return draw_state_; }
private:
    UiLayoutShared layout_shared_;
    UiDrawState draw_state_;
};

} // namespace okage
