#include "okage/load_queue.hpp"
#include <stdexcept>
#include <utility>

namespace okage {
NativeLoadQueue::NativeLoadQueue():worker_([this]{run();}) {}
NativeLoadQueue::~NativeLoadQueue() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_=true;
    }
    changed_.notify_all();
    worker_.join();
}
void NativeLoadQueue::enqueue(std::function<void()> job) {
    if(!job)throw std::invalid_argument("Empty native load callback");
    {
        std::lock_guard lock(mutex_);
        if(failure_)std::rethrow_exception(failure_);
        if(pending_==capacity)throw std::runtime_error("Native load queue is full");
        jobs_.push_back(std::move(job));
        ++pending_;
    }
    changed_.notify_one();
}
void NativeLoadQueue::resume() {
    {
        std::lock_guard lock(mutex_);
        if(failure_)std::rethrow_exception(failure_);
        resumed_=true;
    }
    changed_.notify_one();
}
std::size_t NativeLoadQueue::pending() const {
    std::lock_guard lock(mutex_);
    return pending_;
}
void NativeLoadQueue::check() const {
    std::lock_guard lock(mutex_);
    if(failure_)std::rethrow_exception(failure_);
}
void NativeLoadQueue::run() {
    for(;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,[this]{return stop_requested_||(resumed_&&!jobs_.empty());});
            if(stop_requested_)return;
            job=std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {job();}
        catch(...) {
            std::lock_guard lock(mutex_);
            failure_=std::current_exception();
            return;
        }
        {
            std::lock_guard lock(mutex_);
            --pending_;
        }
    }
}
}
