#pragma once
#include "okage/assets.hpp"
#include <optional>

namespace okage {
struct DecodedSample {
    std::vector<std::int16_t> pcm;
    std::size_t consumed=0;
    std::optional<std::size_t> loop_sample;
    std::uint8_t end_flags=0;
};
DecodedSample decode_spu_sample(Bytes blocks);
}
