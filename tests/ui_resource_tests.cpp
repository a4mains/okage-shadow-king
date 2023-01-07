#include "okage/ui_resource.hpp"
#include "okage/ruw.hpp"

#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace okage;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void put(std::vector<std::uint8_t>& bytes, unsigned at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = std::uint8_t(value >> (i * 8));
}
std::shared_ptr<RuwResource> resource(bool with_sprite) {
    const unsigned count = with_sprite ? 2 : 1;
    std::vector<std::uint8_t> bytes(28 + 16 + 5 * 68 + count * 68);
    put(bytes, 8, 1); put(bytes, 12, 1); put(bytes, 16, 1); put(bytes, 20, 5); put(bytes, 24, count);
    put(bytes, 28, 8); put(bytes, 32, 16); put(bytes, 36, 12); put(bytes, 40, 8);
    for (unsigned i = 0; i < count; ++i) {
        const auto at = 384 + i * 68;
        put(bytes, at, __builtin_bit_cast(std::uint32_t, (1.0f)));
        put(bytes, at + 4, __builtin_bit_cast(std::uint32_t, (1.0f)));
        put(bytes, at + 8, 0x80808080); put(bytes, at + 12, 0x80808080);
        put(bytes, at + 16, 1); put(bytes, at + 20, i == 0 ? 1 : 0);
        put(bytes, at + 44, 28); put(bytes, at + 48, 51);
        put(bytes, at + 52, 12); put(bytes, at + 56, 8); put(bytes, at + 60, i);
    }
    auto result = std::make_shared<RuwResource>(bytes);
    result->set_text(0, 0, "A");
    return result;
}
std::shared_ptr<const std::vector<Font>> fonts() {
    auto result = std::make_shared<std::vector<Font>>(3);
    for (auto& font : *result) {
        font.first = 32; font.last = 126; font.glyphs.resize(95);
        for (unsigned i = 0; i < 95; ++i)
            font.glyphs[i].metrics = {std::uint16_t(i + 32), 0, 0, 192, 8, 8, 200, 392, 192, 384};
    }
    return result;
}
void configure(UiWindow& window, unsigned glyph_capacity, unsigned sprite_capacity,
               std::shared_ptr<RuwResource> ruw) {
    std::array<std::uint32_t, 28> settings{};
    settings[0] = window.stream().binding_id; settings[1] = 40; settings[2] = 6;
    settings[20] = 1; settings[21] = 1;
    settings[25] = glyph_capacity; settings[26] = sprite_capacity;
    const std::array<UiWindow::AtlasTuple, 2> atlas{{{0, 0, 16, 16}, {}}};
    window.configure(settings, atlas, std::make_shared<int>(1));
    const std::array<std::size_t, 1> groups{0};
    window.bind_ruw(std::move(ruw), groups);
}
void coordinator_order_and_slot_selection() {
    UiResourceCoordinator coordinator;
    UiResourceCoordinator::Registry registry{};
    auto left_resource = resource(false);
    auto right_resource = resource(true);
    registry[0] = std::make_shared<UiWindow>(4, 4, 0,
        std::make_shared<UiWindowStream>(UiWindowStream{1, {}}));
    registry[1] = std::make_shared<UiWindow>(4, 4, 1,
        std::make_shared<UiWindowStream>(UiWindowStream{2, {}}));
    registry[2] = std::make_shared<UiWindow>(4, 4, 2,
        std::make_shared<UiWindowStream>(UiWindowStream{1, {}}));
    configure(*registry[0], 4, 4, left_resource);
    configure(*registry[1], 4, 4, right_resource);
    configure(*registry[2], 4, 4, resource(false));
    registry[0]->set_opacity_tween(0.0f, 128.0f, 0, 128);
    registry[1]->set_opacity_tween(0.0f, 128.0f, 0, 128);
    registry[2]->set_opacity_tween(0.2f, 0.0f, 0, 0);

    std::array<std::uint32_t, 16> accumulators{};
    accumulators[3] = 0xfffffff9u;
    accumulators[9] = 37;
    accumulators[11] = 12;
    const std::array<UiResourceSlotBinding, 3> bindings{{{0, 3}, {1, 9}, {2, 11}}};
    const auto sine = make_sine_table();
    GameRandom random{1};
    const auto result = coordinator.advance_resource(registry, bindings, accumulators,
        fonts(), sine, random, initial_ui_projection());
    require(result.has_value(), "Registered windows require the 1a8150 draw pass");
    require(registry[0]->word(0x4f0) == 2 && registry[1]->word(0x4f0) == 2 &&
            registry[2]->word(0x4f0) == 2,
            "Registry-wide clock runs before window preparation");
    require(registry[0]->word(0xcdc) == 1 && registry[1]->word(0xcdc) == 1,
            "Both mode2 windows prepared their actual text records");
    require(registry[2]->word(0x270) == 0 && registry[2]->word(0x27c) == 0 &&
            registry[2]->word(0xcdc) == 0,
            "Clock tween completes before mode2 eligibility is tested");
    require(registry[1]->sprite_buffer(0).word(4) == __builtin_bit_cast(std::uint32_t, (14.0f)),
            "Each window consumes the accumulator selected by its explicit resource slot");
    require(coordinator.layout_shared().sprite_dirty,
            "Ascending preparation retains final shared state from slot1");
    require(accumulators[3] == 0xfffffff9u && accumulators[9] == 37,
            "Coordinator reads the caller's resource accumulators without shadow copies");
    require(random.state != 1 && !result->commands().empty(),
            "Layout consumes the live GameRandom owner and draw IR follows preparation");
}
void missing_slot_is_rejected_only_at_reached_mode2() {
    UiResourceCoordinator coordinator;
    UiResourceCoordinator::Registry registry{};
    registry[2] = std::make_shared<UiWindow>(1, 1, 2,
        std::make_shared<UiWindowStream>(UiWindowStream{1, {}}));
    configure(*registry[2], 1, 1, resource(false));
    registry[2]->set_opacity_tween(0.0f, 128.0f, 0, 128);
    std::array<std::uint32_t, 16> accumulators{};
    const auto sine = make_sine_table();
    GameRandom random{7};
    bool rejected = false;
    try {
        coordinator.advance_resource(registry, {}, accumulators, fonts(), sine,
                                     random, initial_ui_projection());
    } catch (const std::runtime_error&) { rejected = true; }
    require(rejected && registry[2]->word(0x4f0) == 2 && random.state == 7,
            "Missing binding rejects only when post-clock mode2 layout is reached");
}
void no_registry_skips_shared_draw() {
    UiResourceCoordinator coordinator;
    UiResourceCoordinator::Registry registry{};
    std::array<std::uint32_t, 16> accumulators{};
    const auto sine = make_sine_table();
    GameRandom random{11};
    require(!coordinator.advance_resource(registry, {}, accumulators, nullptr, {},
                    random, initial_ui_projection()),
            "Source only calls 1a8150 when registry count is nonzero");
    require(random.state == 11, "Empty registry consumes no random values");
}
}
int main() {
    try {
        coordinator_order_and_slot_selection();
        missing_slot_is_rejected_only_at_reached_mode2();
        no_registry_skips_shared_draw();
        std::cout << "UI resource coordinator tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
