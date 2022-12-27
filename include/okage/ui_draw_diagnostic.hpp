#pragma once

#include "okage/ui_draw.hpp"
#include <iosfwd>
#include <string_view>

namespace okage {
void write_ui_draw_diagnostic(std::ostream&, const UiDrawList&,
                              std::uint32_t frame, std::uint32_t input_anchor,
                              std::uint32_t reference_vblank,
                              std::string_view execution);
}
