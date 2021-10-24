#pragma once
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

enum class PerformanceEnd {pause,window_close,application_terminate,runtime_error};

class PerformanceInterval {
public:
    bool active() const {return active_;}
    void begin(double now,std::uint64_t frame);
    void update_begin(double now,double cap_discarded_seconds);
    void discard_elapsed(double seconds);
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
    PerformanceDistribution update_gaps_,draw_gaps_,update_wall_,tick_wall_,refresh_wall_;
};

class LivePerformance {
public:
    explicit LivePerformance(const std::filesystem::path& path);
    ~LivePerformance();
    LivePerformance(const LivePerformance&)=delete;
    LivePerformance& operator=(const LivePerformance&)=delete;
    PerformanceInterval interval;
    void finish(double now,std::uint64_t frame,PerformanceEnd reason);
private:
    void append(const std::string& line);
    int descriptor_=-1;
    std::uint64_t intervals_=0;
};
}
