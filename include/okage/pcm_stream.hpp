#pragma once

#include "okage/assets.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace okage {

struct TitlePcmGains {
    int stream_left = 128;
    int stream_right = 128;
    int global_left = 128;
    int global_right = 128;
};

class TitlePcmStream {
public:
    static constexpr unsigned sample_rate = 48000;
    static constexpr std::size_t gain_table_size = 257;

    TitlePcmStream(Bytes xwh, Bytes xwb,
                   std::span<const std::int16_t, gain_table_size> q12_gain_table);

    void mix(std::span<std::int16_t> interleaved_stereo,
             TitlePcmGains gains = {});

    std::uint64_t frame_count() const { return frame_count_; }
    std::uint64_t loop_start_frame() const { return loop_start_frame_; }
    std::uint64_t cursor_frame() const { return cursor_frame_; }
    int authored_left_gain() const { return authored_left_gain_; }
    int authored_right_gain() const { return authored_right_gain_; }

private:
    std::vector<std::uint8_t> samples_;
    std::array<std::int16_t, gain_table_size> q12_gain_table_{};
    std::uint64_t frame_count_ = 0;
    std::uint64_t loop_start_frame_ = 0;
    std::uint64_t cursor_frame_ = 0;
    int authored_left_gain_ = 0;
    int authored_right_gain_ = 0;

    int q12_gain(int authored, int stream, int global) const;
};

} // namespace okage
