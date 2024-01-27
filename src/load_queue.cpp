#include "okage/load_queue.hpp"
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace okage {
namespace {
const char* event_name(LoadEventKind kind) {
    switch(kind) {
        case LoadEventKind::Enqueue:return "enqueue";
        case LoadEventKind::Resume:return "resume";
        case LoadEventKind::WorkStart:return "work_start";
        case LoadEventKind::WorkComplete:return "work_complete";
        case LoadEventKind::WorkFailure:return "work_failure";
        case LoadEventKind::Poll:return "poll";
        case LoadEventKind::Prepared:return "prepared";
        case LoadEventKind::MainContinuation:return "main_continuation";
        case LoadEventKind::PauseRequest:return "pause_request";
        case LoadEventKind::PauseAcknowledged:return "pause_acknowledged";
        case LoadEventKind::Checkpoint:return "checkpoint";
    }
    throw std::runtime_error("Unknown loader observation event");
}

std::string json_string(const std::string& value) {
    static constexpr char hex[]="0123456789abcdef";
    std::string result;
    result.reserve(value.size()+2);
    result.push_back('"');
    for(const unsigned char c:value) {
        switch(c) {
            case '"':result+="\\\"";break;
            case '\\':result+="\\\\";break;
            case '\b':result+="\\b";break;
            case '\f':result+="\\f";break;
            case '\n':result+="\\n";break;
            case '\r':result+="\\r";break;
            case '\t':result+="\\t";break;
            default:
                if(c<0x20) {
                    result+="\\u00";
                    result.push_back(hex[c>>4]);
                    result.push_back(hex[c&0x0f]);
                } else result.push_back(static_cast<char>(c));
        }
    }
    result.push_back('"');
    return result;
}
}

void write_load_observation_fields(std::ostream& destination,const LoadObservation& snapshot) {
    std::ostringstream out;
    out<<"\"dropped\":"<<snapshot.dropped<<",\"events\":[";
    bool comma=false;
    for(const auto& event:snapshot.events) {
        if(comma)out<<',';
        comma=true;
        std::ostringstream thread;
        thread<<event.thread;
        out<<"{\"seq\":"<<event.sequence<<",\"kind\":"<<json_string(event_name(event.kind))
           <<",\"job\":"<<event.job<<",\"source\":"<<event.identity.source
           <<",\"task\":"<<event.identity.task<<",\"pending\":"<<event.pending
           <<",\"running\":"<<event.running_job<<",\"resumed\":"<<(event.resumed?"true":"false")
           <<",\"failed\":"<<(event.failed?"true":"false")<<",\"tick\":";
        if(event.logical_tick)out<<*event.logical_tick;else out<<"null";
        out<<",\"host_monotonic_ns\":"
           <<std::chrono::duration_cast<std::chrono::nanoseconds>(event.host_time.time_since_epoch()).count()
           <<",\"thread\":"<<json_string(thread.str())<<'}';
    }
    out<<']';
    destination<<out.str();
    if(!destination)throw std::runtime_error("Cannot write loader observation");
}

void write_load_observation(std::ostream& destination,const LoadObservation& snapshot) {
    std::ostringstream out;
    out<<'{';
    write_load_observation_fields(out,snapshot);
    out<<'}';
    destination<<out.str();
    if(!destination)throw std::runtime_error("Cannot write loader observation");
}

NativeLoadQueue::NativeLoadQueue():NativeLoadQueue(0) {}
NativeLoadQueue::NativeLoadQueue(std::size_t capacity):observation_capacity_(capacity) {
    events_.reserve(capacity);
    worker_=std::jthread([this](std::stop_token stop){run(stop);});
}
NativeLoadQueue::~NativeLoadQueue() {
    {
        std::lock_guard lock(mutex_);
        stopping_=true;
    }
    worker_.request_stop();
    changed_.notify_all();
    worker_.join();
}
void NativeLoadQueue::enqueue(std::function<void()> job) {
    (void)enqueue(std::move(job),{});
}
std::uint64_t NativeLoadQueue::enqueue(std::function<void()> job,LoadJobIdentity identity) {
    if(!job)throw std::invalid_argument("Empty native load callback");
    std::uint64_t id;
    {
        std::lock_guard lock(mutex_);
        if(failure_)std::rethrow_exception(failure_);
        if(pending_==capacity)throw std::runtime_error("Native load queue is full");
        id=next_job_;
        jobs_.push_back({std::move(job),id,identity});
        ++next_job_;
        ++pending_;
        record(LoadEventKind::Enqueue,id,identity);
    }
    changed_.notify_one();
    return id;
}
void NativeLoadQueue::resume() {
    {
        std::lock_guard lock(mutex_);
        if(failure_)std::rethrow_exception(failure_);
        if(stopping_)throw std::logic_error("Native load queue is stopping");
        if(pause_requested_&&pause_controller_!=std::this_thread::get_id())
            throw std::logic_error("Only the pause controller may resume the loader");
        if(pause_requested_&&acknowledged_pause_generation_<pause_generation_)
            throw std::logic_error("Cannot resume before the loader pause is acknowledged");
        if(pause_requested_)pause_controller_={};
        pause_requested_=false;
        ++resume_generation_;
        resumed_=true;
        record(LoadEventKind::Resume,0,{});
    }
    changed_.notify_one();
}
void NativeLoadQueue::pause_and_wait() {
    std::unique_lock lock(mutex_);
    if(std::this_thread::get_id()==worker_thread_)
        throw std::logic_error("Native load worker cannot wait for its own pause");
    if(failure_)std::rethrow_exception(failure_);
    if(stopping_)throw std::logic_error("Native load queue is stopping");
    if(pause_requested_&&pause_controller_!=std::this_thread::get_id())
        throw std::logic_error("Only the pause controller may join the loader pause");
    if(!pause_requested_) {
        pause_requested_=true;
        ++pause_generation_;
        pause_controller_=std::this_thread::get_id();
        resumed_=false;
    }
    const auto requested_generation=pause_generation_;
    record(LoadEventKind::PauseRequest,running_job_,running_identity_);
    changed_.notify_all();
    changed_.wait(lock,[this,requested_generation]{
        return acknowledged_pause_generation_>=requested_generation||failure_||stopping_;
    });
    if(failure_)std::rethrow_exception(failure_);
    if(stopping_)throw std::logic_error("Native load queue stopped before acknowledging pause");
}
bool NativeLoadQueue::checkpoint() {
    std::unique_lock lock(mutex_);
    if(std::this_thread::get_id()!=worker_thread_||!running_job_)
        throw std::logic_error("Loader checkpoint must run inside the active worker callback");
    record(LoadEventKind::Checkpoint,running_job_,running_identity_);
    if(stopping_||worker_.get_stop_token().stop_requested())return false;
    while(pause_requested_) {
        if(acknowledged_pause_generation_<pause_generation_) {
            acknowledged_pause_generation_=pause_generation_;
            record(LoadEventKind::PauseAcknowledged,running_job_,running_identity_);
            changed_.notify_all();
        }
        const auto resumed_generation=resume_generation_;
        changed_.wait(lock,[this,resumed_generation]{
            return stopping_||worker_.get_stop_token().stop_requested()||
                   resume_generation_!=resumed_generation;
        });
        if(stopping_||worker_.get_stop_token().stop_requested()) {
            changed_.notify_all();
            return false;
        }
    }
    return true;
}
std::size_t NativeLoadQueue::pending() const {
    std::lock_guard lock(mutex_);
    record(LoadEventKind::Poll,0,{});
    return pending_;
}
void NativeLoadQueue::check() const {
    std::lock_guard lock(mutex_);
    if(failure_)std::rethrow_exception(failure_);
}
void NativeLoadQueue::record(LoadEventKind kind,std::uint64_t job,LoadJobIdentity identity,
                           std::optional<std::uint64_t> tick) const noexcept {
    // All callers hold mutex_. Saturation is explicit and never changes jobs.
    if(!observation_capacity_)return;
    const auto sequence=next_event_++;
    if(events_.size()==observation_capacity_){++dropped_;return;}
    events_.push_back({sequence,job,kind,identity,pending_,running_job_,resumed_,
        bool(failure_),tick,std::chrono::steady_clock::now(),std::this_thread::get_id()});
}
LoadObservation NativeLoadQueue::observation() const {
    std::lock_guard lock(mutex_);
    return {events_,dropped_};
}
void NativeLoadQueue::observe(LoadEventKind kind,std::uint64_t job,LoadJobIdentity identity,
                            std::optional<std::uint64_t> tick) {
    if(kind!=LoadEventKind::Prepared&&kind!=LoadEventKind::MainContinuation&&kind!=LoadEventKind::Poll)
        throw std::invalid_argument("Loader marker cannot impersonate a queue event");
    std::lock_guard lock(mutex_);
    if(job>=next_job_)throw std::invalid_argument("Loader marker references an unknown job");
    record(kind,job,identity,tick);
}
void NativeLoadQueue::run(std::stop_token stop) {
    {
        std::lock_guard lock(mutex_);
        worker_thread_=std::this_thread::get_id();
        changed_.notify_all();
    }
    for(;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            for(;;) {
                if(stop.stop_requested()||stopping_||failure_)return;
                if(pause_requested_) {
                    if(acknowledged_pause_generation_<pause_generation_) {
                        acknowledged_pause_generation_=pause_generation_;
                        record(LoadEventKind::PauseAcknowledged,0,{});
                        changed_.notify_all();
                    }
                    const auto resumed_generation=resume_generation_;
                    changed_.wait(lock,stop,[this,&stop,resumed_generation]{
                        return stop.stop_requested()||stopping_||
                               resume_generation_!=resumed_generation;
                    });
                    if(stop.stop_requested()||stopping_)return;
                    continue;
                }
                if(resumed_&&!jobs_.empty())break;
                changed_.wait(lock,stop,[this,&stop]{
                    return stop.stop_requested()||stopping_||pause_requested_||(resumed_&&!jobs_.empty());
                });
            }
            if(stop.stop_requested()||stopping_)return;
            job=std::move(jobs_.front());
            jobs_.pop_front();
            running_job_=job.id;
            running_identity_=job.identity;
            record(LoadEventKind::WorkStart,job.id,job.identity);
        }
        try {job.callback();}
        catch(...) {
            std::lock_guard lock(mutex_);
            failure_=std::current_exception();
            running_job_=0;
            running_identity_={};
            record(LoadEventKind::WorkFailure,job.id,job.identity);
            changed_.notify_all();
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if(stop.stop_requested()||stopping_) {
                running_job_=0;
                running_identity_={};
                changed_.notify_all();
                return;
            }
            --pending_;
            running_job_=0;
            running_identity_={};
            record(LoadEventKind::WorkComplete,job.id,job.identity);
        }
        changed_.notify_all();
    }
}
}
