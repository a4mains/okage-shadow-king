#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace okage {
class NativeLoadQueue {
public:
    static constexpr std::size_t capacity=31;
    NativeLoadQueue();
    NativeLoadQueue(const NativeLoadQueue&)=delete;
    NativeLoadQueue& operator=(const NativeLoadQueue&)=delete;
    ~NativeLoadQueue();
    void enqueue(std::function<void()>);
    void resume();
    std::size_t pending() const;
    void check() const;
private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> jobs_;
    std::size_t pending_=0;
    bool resumed_=false;
    bool stop_requested_=false;
    std::exception_ptr failure_;
    // Last member: the worker is joined before all state it accesses dies.
    std::thread worker_;
};
}
