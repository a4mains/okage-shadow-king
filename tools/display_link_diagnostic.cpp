#include "display_link.hpp"
#include <iostream>
#include <bit>
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace {
void timestamp(const okage::DisplayLink::Timestamp& t) {
    std::cout<<"{\"host_time\":"<<t.host_time<<",\"video_time\":"<<t.video_time
        <<",\"video_time_scale\":"<<t.video_time_scale<<",\"video_refresh_period\":"<<t.video_refresh_period
        <<",\"rate_scalar_bits\":"<<std::bit_cast<std::uint64_t>(t.rate_scalar)<<",\"flags\":"<<t.flags
        <<",\"host_time_valid\":"<<(t.host_time_valid?"true":"false")
        <<",\"video_time_valid\":"<<(t.video_time_valid?"true":"false")
        <<",\"video_period_valid\":"<<(t.video_period_valid?"true":"false")<<'}';
}
}
int main() {
    try {
        {
            okage::DisplayLink link;
            auto previous=link.sequence();
            const auto metadata=link.snapshot();
            if(!std::isfinite(metadata.host_clock_frequency)||metadata.host_clock_frequency<=0)
                throw std::runtime_error("Invalid host clock frequency metadata");
            std::cout<<std::setprecision(17)<<"{\"event\":\"header\",\"provider\":\"CVDisplayLink\",\"scanout_proof\":false,\"display_id\":"
                <<metadata.display_id<<",\"nominal_time_value\":"<<metadata.nominal_time_value
                <<",\"nominal_time_scale\":"<<metadata.nominal_time_scale<<",\"nominal_flags\":"<<metadata.nominal_flags
                <<",\"host_clock_frequency\":"<<metadata.host_clock_frequency<<"}\n";
            for(unsigned i=0;i<3;++i) {
                const auto s=link.wait_after(previous);
                if(s.sequence<=previous||!link.ready_after(previous))throw std::runtime_error("Display generation failed to advance");
                std::cout<<"{\"event\":\"opportunity\",\"sequence\":"<<s.sequence<<",\"baseline\":"<<previous<<",\"now\":";
                timestamp(s.now);std::cout<<",\"output\":";timestamp(s.output);std::cout<<"}\n";
                previous=s.sequence;
            }
        }
        std::cout<<"{\"event\":\"finish\",\"observations\":3,\"owner_destroyed\":true}\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
