#pragma once
#include "okage/audio_output.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace okage {
struct PerformanceDistribution {
    static constexpr std::array<double,19> upper_ms{
        .25,.5,1,2,4,8,16.666667,25,33.333334,50,100,250,500,1000,2000,5000,10000,30000,60000};
    std::array<std::uint64_t,upper_ms.size()+1> buckets{}; // Last bucket is >60000ms.
    std::uint64_t count=0;
    double sum_seconds=0,min_seconds=0,max_seconds=0;
    void add(double seconds);
    std::string json() const;
};

enum class PerformanceEnd {pause,window_close,application_terminate,runtime_error,test_complete};
struct PerformanceTestRange {std::uint64_t start_frame,end_frame;};

class PerformanceInterval {
public:
    bool active() const {return active_;}
    void begin(double now,std::uint64_t frame);
    void update_begin(double now,double cap_discarded_seconds);
    void discard_elapsed(double seconds);
    void scheduler_admit(double admitted_seconds,double pending_seconds);
    void scheduler_pending(double pending_seconds);
    void audio_sample(const AudioOutputStats& stats);
    void update_end(double now);
    void tick(double start,double end,bool rendered);
    void refresh(double start,double end);
    void draw(double now);
    std::string finish(double now,std::uint64_t frame,PerformanceEnd reason,std::uint64_t index);
private:
    void observe(double now);
    bool active_=false,update_open_=false;
    double start_=0,last_observation_=0,update_start_=0,discarded_=0;
    std::optional<double> last_update_,last_draw_;
    std::uint64_t first_frame_=0,updates_=0,ticks_=0,rendered_=0,draws_=0;
    double admitted_=0,pending_=0,max_pending_=0;
    bool scheduler_observed_=false,audio_observed_=false;
    std::uint64_t update_ticks_=0,max_update_ticks_=0,backlogged_updates_=0;
    AudioOutputStats audio_{};
    PerformanceDistribution pending_samples_,audio_ring_samples_;
    PerformanceDistribution update_gaps_,draw_gaps_,update_wall_,tick_wall_,refresh_wall_;
};

class LivePerformance {
public:
    explicit LivePerformance(const std::filesystem::path& path,
                             std::optional<PerformanceTestRange> test_range={});
    ~LivePerformance();
    LivePerformance(const LivePerformance&)=delete;
    LivePerformance& operator=(const LivePerformance&)=delete;
    PerformanceInterval interval;
    void finish(double now,std::uint64_t frame,PerformanceEnd reason);
private:
    void append(const std::string& line);
    int descriptor_=-1;
    std::uint64_t intervals_=0;
    std::optional<PerformanceTestRange> test_range_;
};
}
