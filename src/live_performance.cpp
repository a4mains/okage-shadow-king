#include "okage/live_performance.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace okage {
namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
std::ostringstream json_stream() {
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);return out;
}
const char* reason_name(PerformanceEnd reason) {
    switch(reason) {
        case PerformanceEnd::pause:return "pause";
        case PerformanceEnd::window_close:return "window_close";
        case PerformanceEnd::application_terminate:return "application_terminate";
        case PerformanceEnd::runtime_error:return "runtime_error";
    }
    throw std::runtime_error("Invalid performance interval end reason");
}
std::runtime_error output_error() {
    return std::runtime_error(std::string("Cannot write performance report: ")+std::strerror(errno));
}
}
void PerformanceDistribution::add(double seconds) {
    require(std::isfinite(seconds)&&seconds>=0,"Invalid performance duration");
    if(!count)min_seconds=max_seconds=seconds;
    else {min_seconds=std::min(min_seconds,seconds);max_seconds=std::max(max_seconds,seconds);}
    ++count;sum_seconds+=seconds;
    const auto bucket=std::lower_bound(upper_ms.begin(),upper_ms.end(),seconds*1000)-upper_ms.begin();
    ++buckets[bucket];
}
std::string PerformanceDistribution::json() const {
    auto out=json_stream();out<<"{\"count\":"<<count<<",\"sum_ms\":"<<sum_seconds*1000;
    if(count)out<<",\"min_ms\":"<<min_seconds*1000<<",\"max_ms\":"<<max_seconds*1000<<",\"mean_ms\":"<<sum_seconds*1000/count;
    else out<<",\"min_ms\":null,\"max_ms\":null,\"mean_ms\":null";
    out<<",\"bucket_counts\":[";
    for(std::size_t i=0;i<buckets.size();++i)out<<(i?",":"")<<buckets[i];
    out<<"]}";return out.str();
}
void PerformanceInterval::observe(double now) {
    require(active_,"Performance interval is not active");
    require(std::isfinite(now)&&now>=last_observation_,"Nonmonotonic performance timestamp");
    last_observation_=now;
}
void PerformanceInterval::begin(double now,std::uint64_t frame) {
    require(!active_,"Performance interval already active");
    require(std::isfinite(now)&&now>=0,"Invalid performance start timestamp");
    *this={};active_=true;start_=last_observation_=now;first_frame_=frame;
}
void PerformanceInterval::update_begin(double now,double cap_discarded_seconds) {
    require(!update_open_,"Overlapping performance update");
    require(std::isfinite(cap_discarded_seconds)&&cap_discarded_seconds>=0,"Invalid discarded elapsed time");
    observe(now);
    if(last_update_)update_gaps_.add(now-*last_update_);
    last_update_=now;update_start_=now;update_open_=true;++updates_;discarded_+=cap_discarded_seconds;
}
void PerformanceInterval::discard_elapsed(double seconds) {
    require(active_&&std::isfinite(seconds)&&seconds>=0,"Invalid discarded elapsed time");discarded_+=seconds;
}
void PerformanceInterval::update_end(double now) {
    require(update_open_,"Performance update is not active");observe(now);
    update_wall_.add(now-update_start_);update_open_=false;
}
void PerformanceInterval::tick(double start,double end,bool rendered) {
    require(update_open_,"Performance tick outside update");
    require(std::isfinite(start)&&start>=update_start_&&end>=start,"Invalid performance tick timestamps");observe(end);
    tick_wall_.add(end-start);++ticks_;if(rendered)++rendered_;
}
void PerformanceInterval::refresh(double start,double end) {
    require(std::isfinite(start)&&start>=start_&&end>=start,"Invalid performance refresh timestamps");observe(end);refresh_wall_.add(end-start);
}
void PerformanceInterval::draw(double now) {
    observe(now);if(last_draw_)draw_gaps_.add(now-*last_draw_);
    last_draw_=now;++draws_;
}
std::string PerformanceInterval::finish(double now,std::uint64_t frame,PerformanceEnd reason,std::uint64_t index) {
    require(!update_open_,"Cannot finish performance interval during update");observe(now);
    require(frame>=first_frame_&&frame-first_frame_>=ticks_,"Inconsistent performance frame counter");
    const double elapsed=now-start_;auto out=json_stream();
    out<<"{\"type\":\"interval\",\"schema_version\":1,\"interval\":"<<index
       <<",\"end_reason\":\""<<reason_name(reason)<<"\",\"monotonic_start_s\":"<<start_
       <<",\"monotonic_end_s\":"<<now<<",\"wall_duration_s\":"<<elapsed
       <<",\"start_frame\":"<<first_frame_<<",\"end_frame\":"<<frame
       <<",\"frame_counter_delta\":"<<frame-first_frame_<<",\"completed_simulation_ticks\":"<<ticks_
       <<",\"frame_counter_matches_completed_ticks\":"<<(frame-first_frame_==ticks_?"true":"false")
       <<",\"rendered_ticks\":"<<rendered_<<",\"update_callbacks\":"<<updates_
       <<",\"appkit_draw_callbacks\":"<<draws_<<",\"elapsed_discarded_by_update_cap_s\":"<<discarded_;
    if(elapsed>0)out<<",\"simulation_ticks_per_wall_second\":"<<ticks_/elapsed
                   <<",\"appkit_draw_callbacks_per_wall_second\":"<<draws_/elapsed;
    else out<<",\"simulation_ticks_per_wall_second\":null,\"appkit_draw_callbacks_per_wall_second\":null";
    out<<",\"update_callback_gaps\":"<<update_gaps_.json()<<",\"appkit_draw_callback_gaps\":"<<draw_gaps_.json()
       <<",\"update_wall\":"<<update_wall_.json()<<",\"completed_tick_wall\":"<<tick_wall_.json()
       <<",\"refresh_wall\":"<<refresh_wall_.json()<<"}\n";
    active_=false;return out.str();
}
LivePerformance::LivePerformance(const std::filesystem::path& path) {
    descriptor_=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(descriptor_<0)throw std::runtime_error(std::string("Performance report requires a fresh writable path: ")+std::strerror(errno));
    try {
        auto out=json_stream();
        out<<"{\"type\":\"metadata\",\"schema_version\":1,\"clock\":\"NSProcessInfo.systemUptime monotonic seconds\","
              "\"scope\":\"Interactive running intervals only; wall timings, not CPU or GPU timings. AppKit draw callbacks are not compositor presentations or presented FPS.\","
              "\"boundaries\":\"Resume callback entry through pause callback entry; initial live start is after window activation. Pause callback work and report flush are excluded. Resume audio setup and refresh are included. Cap discard includes the last running gap at pause.\","
              "\"counts\":\"Ticks count completed Session::tick calls. Rendered ticks have a current-frame rendered surface after that call. Failed tick work is included only in update_wall; frame-counter mismatch is reported. Gaps are between callbacks within an interval.\","
              "\"timings\":\"update_wall includes tick and refresh wall times; distributions overlap. Histogram bins have inclusive upper bounds; final bin is overflow. No per-frame samples retained.\","
              "\"persistence\":\"Only finished intervals are written. Abrupt termination can leave the final interval absent.\","
              "\"histogram_upper_ms\":[";
        for(std::size_t i=0;i<PerformanceDistribution::upper_ms.size();++i)
            out<<(i?",":"")<<PerformanceDistribution::upper_ms[i];
        out<<",null]}\n";append(out.str());
    } catch(...) {::close(descriptor_);descriptor_=-1;throw;}
}
LivePerformance::~LivePerformance() {if(descriptor_>=0)::close(descriptor_);}
void LivePerformance::append(const std::string& line) {
    std::size_t offset=0;
    while(offset<line.size()) {
        const auto written=::write(descriptor_,line.data()+offset,line.size()-offset);
        if(written<0&&errno==EINTR)continue;
        if(written<=0)throw output_error();
        offset+=written;
    }
    if(::fsync(descriptor_))throw output_error();
}
void LivePerformance::finish(double now,std::uint64_t frame,PerformanceEnd reason) {
    if(interval.active())append(interval.finish(now,frame,reason,++intervals_));
}
}
