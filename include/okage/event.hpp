#pragma once
#include "okage/assets.hpp"
#include <span>
#include <vector>

namespace okage {
void initialize_game_variables(std::span<std::uint8_t> variables);
// Recovered field-event variable banks in the runtime structure, not a save format.
// Banks 8..15 expose signed 16-bit records, bounded to their 0x50-byte stride.
std::int32_t event_variable(Bytes variables, std::uint32_t selector);
void set_event_variable(std::span<std::uint8_t> variables,unsigned index,unsigned bank,std::int32_t value);

struct DialogueWaitState {
    std::uint32_t mode=0, active=0, countdown=0, observed=0, accumulated=0;
};
struct DialogueObservation {
    bool present=false;
    std::int32_t status=0;
    // Native dialogue must supply these; current comparisons use observations.
    std::span<const std::uint32_t> layer_flags;
    std::int32_t text_progress=-1;
};
// Original callback 0x130458: zero releases the event, +/-1 keeps waiting.
// Integer counters retain original 32-bit wraparound. This does not render text.
std::int32_t poll_dialogue_wait(DialogueWaitState& state, std::int32_t request,
                               const DialogueObservation& dialogue);

std::vector<std::uint32_t> run_event_setup(Bytes script, Bytes variables,
                                         std::span<std::uint8_t> field_state);
}
