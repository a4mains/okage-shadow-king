#include "okage/ui_resource.hpp"

#include "okage/font.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool source_reaches_layout(const UiWindow& window) {
    auto flags = window.word(0x1900);
    if (!(flags & 0x8000u)) return false;
    if (flags & 0x1000u) flags = (flags & ~0x22u) | 0x201u;
    const auto display = window.word(0x25c) & 0xffu;
    if (!((display >> 4) & 3u) || (flags & 0x20u) || !(flags & 1u)) return false;
    const auto delta = __builtin_bit_cast(float, (window.word(0x27c)));
    const auto current = __builtin_bit_cast(float, (window.word(0x270)));
    return delta != 0.0f || current != 0.0f;
}
}

std::optional<UiDrawList> UiResourceCoordinator::advance_resource(
    const Registry& registry, std::span<const UiResourceSlotBinding> bindings,
    std::span<const std::uint32_t, 16> resource_accumulators,
    std::shared_ptr<const std::vector<Font>> fonts, std::span<const float> sine,
    GameRandom& random, const ShadowScreenParameters& projection) {
    const bool any_window = std::any_of(registry.begin(), registry.end(),
                                       [](const auto& window) { return bool(window); });
    if (!any_window) return std::nullopt; // 198390 does not tail-call 1a8150.

    check(bool(fonts) && !fonts->empty(), "UI resource pass requires the live shared font owner");
    check(sine.size() == SineTable{}.size(), "UI resource pass requires the full source sine table");

    std::array<std::optional<unsigned>, 8> resource_slot_by_window{};
    for (const auto& binding : bindings) {
        check(binding.window_slot < registry.size(), "UI resource binding window slot bounds");
        check(binding.resource_slot < resource_accumulators.size(), "UI resource accumulator slot bounds");
        check(!resource_slot_by_window[binding.window_slot], "Duplicate UI resource binding for window");
        resource_slot_by_window[binding.window_slot] = binding.resource_slot;
    }

    // FUN_001980f8 walks registry 200688 from slot0 through slot7.
    for (const auto& window : registry)
        if (window) window->advance_clock();

    auto random_value = [&random] { return random.next(); };
    UiPacketBuildContext packets{fonts, draw_state_.background_alpha_top,
                                 draw_state_.background_alpha_bottom};
    const std::span<const Font> font_view(*fonts);

    for (unsigned slot = 0; slot < registry.size(); ++slot) {
        const auto& window = registry[slot];
        if (!window) continue;
        std::int32_t y_offset = 0;
        const bool layout = source_reaches_layout(*window);
        if (layout && window->word(0x3bec) == 2)
            check(resource_slot_by_window[slot].has_value(),
                  "Active mode2 UI window has no source resource-slot binding");
        if (resource_slot_by_window[slot])
            y_offset = __builtin_bit_cast(std::int32_t, (resource_accumulators[*resource_slot_by_window[slot]]));
        UiLayoutContext context{layout_shared_, font_view, sine, random_value,
                                y_offset, packets};
        window->prepare_mode2(context);
    }

    return build_ui_draw_list(registry, draw_state_, projection, sine);
}

} // namespace okage
