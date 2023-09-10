#include "okage/load_queue.hpp"
#include <stdexcept>
#include <utility>

namespace okage {
NativeLoadQueue::NativeLoadQueue():NativeLoadQueue(0) {}
NativeLoadQueue::NativeLoadQueue(std::size_t capacity):observation_capacity_(capacity) {
    events_.reserve(capacity);
    worker_=std::jthread([this](std::stop_token stop){run(stop);});
}
NativeLoadQueue::~NativeLoadQueue() {
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
        resumed_=true;
        record(LoadEventKind::Resume,0,{});
    }
    changed_.notify_one();
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
    for(;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,stop,[this]{return resumed_&&!jobs_.empty();});
            if(stop.stop_requested())return;
            job=std::move(jobs_.front());
            jobs_.pop_front();
            running_job_=job.id;
            record(LoadEventKind::WorkStart,job.id,job.identity);
        }
        try {job.callback();}
        catch(...) {
            std::lock_guard lock(mutex_);
            failure_=std::current_exception();
            running_job_=0;
            record(LoadEventKind::WorkFailure,job.id,job.identity);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            --pending_;
            running_job_=0;
            record(LoadEventKind::WorkComplete,job.id,job.identity);
        }
    }
}
}
