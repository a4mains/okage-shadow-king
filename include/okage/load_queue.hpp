#pragma once
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace okage {
struct LoadJobIdentity {
    std::uint64_t source=0,task=0;
    bool operator==(const LoadJobIdentity&) const = default;
};
enum class LoadEventKind {Enqueue, Resume, WorkStart, WorkComplete, WorkFailure,
                         Poll, Prepared, MainContinuation};
struct LoadEvent {
    std::uint64_t sequence=0,job=0;
    LoadEventKind kind{};
    LoadJobIdentity identity;
    std::size_t pending=0;
    std::uint64_t running_job=0;
    bool resumed=false,failed=false;
    std::optional<std::uint64_t> logical_tick;
    std::chrono::steady_clock::time_point host_time;
    std::thread::id thread;
};
struct LoadObservation {
    // Detached copies: reading/mutating this snapshot cannot affect the queue.
    std::vector<LoadEvent> events;
    std::uint64_t dropped=0;
};
class NativeLoadQueue {
public:
    static constexpr std::size_t capacity=31;
    NativeLoadQueue();
    // Opt-in, preallocated bounded log. Zero is disabled (the default).
    explicit NativeLoadQueue(std::size_t observation_capacity);
    NativeLoadQueue(const NativeLoadQueue&)=delete;
    NativeLoadQueue& operator=(const NativeLoadQueue&)=delete;
    ~NativeLoadQueue();
    void enqueue(std::function<void()>);
    // Same queue operation, with provenance and a stable job ID for markers.
    std::uint64_t enqueue(std::function<void()>,LoadJobIdentity);
    void resume();
    std::size_t pending() const;
    void check() const;
    LoadObservation observation() const;
    void observe(LoadEventKind,std::uint64_t job,LoadJobIdentity,
                 std::optional<std::uint64_t> logical_tick=std::nullopt);
private:
    struct Job {std::function<void()> callback;std::uint64_t id;LoadJobIdentity identity;};
    void record(LoadEventKind,std::uint64_t,LoadJobIdentity,
                std::optional<std::uint64_t> tick=std::nullopt) const noexcept;
    void run(std::stop_token);
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Job> jobs_;
    std::size_t pending_=0;
    bool resumed_=false;
    std::exception_ptr failure_;
    std::uint64_t next_job_=1,running_job_=0;
    std::size_t observation_capacity_=0;
    mutable std::vector<LoadEvent> events_;
    mutable std::uint64_t next_event_=1,dropped_=0;
    // Last member: the worker is joined before all state it accesses dies.
    std::jthread worker_;
};
}
