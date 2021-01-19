#pragma once
#include "okage/assets.hpp"
#include <optional>
#include <array>

namespace okage {
struct AdpcmHistory {std::int32_t recent=0,older=0;};
std::array<std::int16_t,28> decode_spu_block(Bytes block,AdpcmHistory& history);
struct DecodedSample {
    std::vector<std::int16_t> pcm;
    std::size_t consumed=0;
    std::optional<std::size_t> loop_sample;
    std::uint8_t end_flags=0;
};
DecodedSample decode_spu_sample(Bytes blocks);
}
