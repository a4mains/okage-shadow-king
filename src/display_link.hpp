#pragma once
#include <chrono>
#include <cstdint>
#include <memory>

namespace okage {
class DisplayLink {
public:
    struct Timestamp {
        std::uint64_t host_time=0,flags=0;
        std::int64_t video_time=0,video_refresh_period=0;
        std::int32_t video_time_scale=0;
        double rate_scalar=0;
        bool host_time_valid=false,video_time_valid=false,video_period_valid=false;
    };
    struct Snapshot {
        std::uint64_t sequence=0;
        std::uint32_t display_id=0;
        Timestamp now,output;
        std::int64_t nominal_time_value=0;
        std::int32_t nominal_time_scale=0,nominal_flags=0;
        double host_clock_frequency=0;
    };
    explicit DisplayLink(std::chrono::milliseconds watchdog=std::chrono::seconds(10));
    ~DisplayLink();
    DisplayLink(const DisplayLink&)=delete;
    DisplayLink& operator=(const DisplayLink&)=delete;
    DisplayLink(DisplayLink&&)=delete;
    DisplayLink& operator=(DisplayLink&&)=delete;
    std::uint64_t sequence() const;
    bool ready_after(std::uint64_t baseline) const;
    Snapshot wait_after(std::uint64_t baseline) const;
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
