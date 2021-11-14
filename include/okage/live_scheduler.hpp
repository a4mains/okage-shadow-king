#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace okage {
class LiveTickClock {
public:
    static constexpr double step=1./60,wall_cap=.25;
    double admit(double elapsed) {
        if(!std::isfinite(elapsed)||elapsed<0)throw std::runtime_error("Invalid live elapsed time");
        completed_=false;
        const double admitted=std::min(wall_cap,elapsed);pending_+=admitted;return admitted;
    }
    bool tick_due() const {return !completed_&&pending_>=step;}
    void complete_tick() {
        if(!tick_due())throw std::runtime_error("Live tick completed without budget");
        pending_-=step;completed_=true;
    }
    double pending() const {return pending_;}
    void reset() {pending_=0;completed_=false;}
private:
    double pending_=0;
    bool completed_=false;
};
}
