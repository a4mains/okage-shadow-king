#pragma once
#include "okage/assets.hpp"
#include <optional>
namespace okage {
// Recovered animated field setting 0x49 / filter5. Other settings remain guarded.
struct ScreenFilter {
    bool updating=false;
    unsigned setting=0,alpha=0;
    void select(unsigned id,int mode);
    void tick(float value);
};
class FrameFeedback {
    unsigned frame_=0;
    std::optional<Texture> previous_;
public:
    void apply(Texture& current,unsigned frame,unsigned alpha) const;
    void retain(const Texture& completed,unsigned frame,unsigned game_height);
};
}
