#pragma once

#include "okage/pcm_stream.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace okage {

struct TitleStreamState {
    bool registered = false;
    bool stop_pending = false;
    unsigned slot = 0;
    std::uint32_t stop_countdown = 0;
    bool service_ready = false;
    int requested_volume = 0;
    int stream_volume = 0;
    std::uint64_t cursor_frame = 0;
};

class TitleStreamOwner {
public:
    static constexpr unsigned title_slot = 1;
    static constexpr std::uint32_t deferred_stop_updates = 128;

    TitleStreamOwner() = default;
    TitleStreamOwner(const TitleStreamOwner&) = delete;
    TitleStreamOwner& operator=(const TitleStreamOwner&) = delete;

    void start(unsigned slot, Bytes xwh, Bytes xwb,
               std::span<const std::int16_t, TitlePcmStream::gain_table_size> q12_gain_table);

    void request_stop(unsigned slot);

    // Call once per source main-loop update (12ce20), never once per PCM block.
    void advance_main_loop();

    void mix(std::span<std::int16_t> interleaved_stereo);

    TitleStreamState state() const;

private:
    void release_locked();

    mutable std::mutex mutex_;
    std::unique_ptr<TitlePcmStream> stream_;
    bool stop_pending_ = false;
    std::uint32_t stop_countdown_ = 0;
    bool service_ready_ = false;
    bool gain_update_pending_ = false;
    int requested_volume_ = 0;
    int stream_volume_ = 0;
};

} // namespace okage
