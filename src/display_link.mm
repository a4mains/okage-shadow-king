#include "display_link.hpp"
#include <CoreVideo/CoreVideo.h>
#include <CoreGraphics/CoreGraphics.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

namespace okage {
namespace {
using Clock=std::chrono::steady_clock;
struct LinkState {
    std::mutex mutex;
    std::condition_variable changed;
    DisplayLink::Snapshot sample;
    Clock::time_point last_callback=Clock::now();
    std::chrono::milliseconds watchdog;
    bool stopping=false,overflow=false,timed_out=false;
    std::atomic<bool> callback_failed=false;
    explicit LinkState(std::chrono::milliseconds timeout):watchdog(timeout) {}
    void check() const {
        if(stopping)throw std::runtime_error("Display link stopped");
        if(callback_failed.load())throw std::runtime_error("Display link callback failed");
        if(overflow)throw std::runtime_error("Display link sequence overflow");
        if(timed_out)throw std::runtime_error("Display link callback watchdog expired");
    }
    bool ready(std::uint64_t baseline) {
        check();
        if(baseline>sample.sequence)throw std::runtime_error("Display link baseline is in the future");
        if(sample.sequence>baseline)return true;
        if(Clock::now()-last_callback>=watchdog) {
            timed_out=true;changed.notify_all();check();
        }
        return false;
    }
};
DisplayLink::Timestamp stamp(const CVTimeStamp& value) {
    return {value.hostTime,value.flags,value.videoTime,value.videoRefreshPeriod,
            value.videoTimeScale,value.rateScalar,
            bool(value.flags&kCVTimeStampHostTimeValid),
            bool(value.flags&kCVTimeStampVideoTimeValid),
            bool(value.flags&kCVTimeStampVideoRefreshPeriodValid)};
}
void check_cv(CVReturn status,const char* operation) {
    if(status!=kCVReturnSuccess)
        throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(status));
}
}
struct DisplayLink::Impl {
    CVDisplayLinkRef link=nullptr;
    std::shared_ptr<LinkState> state;
    explicit Impl(std::chrono::milliseconds timeout):state(std::make_shared<LinkState>(timeout)) {}
    ~Impl() {
        {
            std::lock_guard lock(state->mutex);
            state->stopping=true;
        }
        state->changed.notify_all();
        if(link) {
            if(CVDisplayLinkIsRunning(link)) {
                const auto status=CVDisplayLinkStop(link);
                if(status!=kCVReturnSuccess)
                    std::fprintf(stderr,"Display link stop failed: %d\n",int(status));
            }
            CVDisplayLinkRelease(link);
        }
    }
};
DisplayLink::DisplayLink(std::chrono::milliseconds watchdog) {
    if(watchdog<=std::chrono::milliseconds::zero()||watchdog>std::chrono::minutes(1))
        throw std::runtime_error("Display link watchdog must be positive and at most one minute");
    auto impl=std::make_unique<Impl>(watchdog);
    const auto display=CGMainDisplayID();
    if(!display||!CGDisplayIsActive(display))throw std::runtime_error("No active main display for display link");
    check_cv(CVDisplayLinkCreateWithCGDisplay(display,&impl->link),"CVDisplayLinkCreateWithCGDisplay");
    if(!impl->link)throw std::runtime_error("Display link creation returned no owner");
    auto state=impl->state;
    state->sample.display_id=CVDisplayLinkGetCurrentCGDisplay(impl->link);
    const auto nominal=CVDisplayLinkGetNominalOutputVideoRefreshPeriod(impl->link);
    state->sample.nominal_time_value=nominal.timeValue;
    state->sample.nominal_time_scale=nominal.timeScale;
    state->sample.nominal_flags=nominal.flags;
    state->sample.host_clock_frequency=CVGetHostClockFrequency();
    check_cv(CVDisplayLinkSetOutputHandler(impl->link,
        ^CVReturn(CVDisplayLinkRef callback_link,const CVTimeStamp* now,const CVTimeStamp* output,
                  CVOptionFlags flags_in,CVOptionFlags* flags_out) {
            (void)callback_link;(void)flags_in;
            const auto callback_state=state;
            *flags_out=0;
            try {
                {
                    std::lock_guard lock(callback_state->mutex);
                    if(callback_state->stopping)return kCVReturnSuccess;
                    if(callback_state->sample.sequence==std::numeric_limits<std::uint64_t>::max())
                        callback_state->overflow=true;
                    else {
                        callback_state->sample.now=stamp(*now);callback_state->sample.output=stamp(*output);
                        ++callback_state->sample.sequence;callback_state->last_callback=Clock::now();
                    }
                }
                callback_state->changed.notify_all();
                return kCVReturnSuccess;
            }catch(...) {
                callback_state->callback_failed.store(true);callback_state->changed.notify_all();
                return kCVReturnError; // Never unwind through CoreVideo.
            }
        }),"CVDisplayLinkSetOutputHandler");
    state->last_callback=Clock::now();
    check_cv(CVDisplayLinkStart(impl->link),"CVDisplayLinkStart");
    impl_=std::move(impl);
}
DisplayLink::~DisplayLink()=default;
std::uint64_t DisplayLink::sequence() const {return snapshot().sequence;}
DisplayLink::Snapshot DisplayLink::snapshot() const {
    const auto state=impl_->state;
    std::lock_guard lock(state->mutex);state->check();return state->sample;
}
bool DisplayLink::ready_after(std::uint64_t baseline) const {
    const auto state=impl_->state;
    std::lock_guard lock(state->mutex);return state->ready(baseline);
}
DisplayLink::Snapshot DisplayLink::wait_after(std::uint64_t baseline) const {
    const auto state=impl_->state;
    std::unique_lock lock(state->mutex);
    while(!state->ready(baseline))
        state->changed.wait_until(lock,state->last_callback+state->watchdog);
    return state->sample;
}
}
