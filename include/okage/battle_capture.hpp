#pragma once
#include "okage/pose.hpp"
#include <array>
#include <cstdint>

namespace okage {
struct BattleCaptureVertex {
    std::uint64_t uv=0,xyz=0;
};
struct BattleCaptureDraw {
    std::uint32_t mode=0,rgba=0,scale=0;
    unsigned count=0;
    std::array<BattleCaptureVertex,4> vertices{};
};
// Layer1's captured-frame transform (0x192b10/0x192368). The caller owns
// enable/opacity and framebuffer capture; this is not a battle lifecycle.
struct BattleCaptureLayer {
    std::uint32_t mode=2,rgba=0x80808080,progress=0;
    BattleCaptureDraw draw(bool field_nonzero,const SineTable&);
};
}
