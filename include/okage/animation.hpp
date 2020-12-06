#pragma once
#include "okage/model.hpp"
#include <functional>
#include <utility>

namespace okage {
class Animation {
public:
    Animation(Bytes bytes, std::size_t channel_count);
    void start(std::size_t entry);
    void tick();
    std::vector<Float4> values();
    std::uint32_t pc() const { return pc_; }
    std::uint32_t script_state() const { return (std::uint32_t(wait_)<<16)|flags_; }
    bool scale_channels() const { return scale_channels_; }
    void set_texture_callback(std::function<void(std::size_t,std::size_t)> callback) {
        texture_callback_=std::move(callback);
    }
private:
    struct Channel {
        std::uint16_t flags=0x201, mode=2;
        std::array<std::uint16_t,4> component{};
        Float4 output{1,1,1,1}, a{1,1,1,1}, b{1,1,1,1}, c{1,1,1,1};
        Float4 t{}, step{}, next_step{};
        std::uint32_t track=0;
    };
    std::vector<std::uint8_t> bytes_;
    std::vector<Channel> channels_;
    std::uint32_t code_=0, samples_=0, sample_end_=0, pc_=0;
    std::uint16_t flags_=1, wait_=0;
    bool wide_tracks_=false, scale_channels_=false;
    std::function<void(std::size_t,std::size_t)> texture_callback_;
    std::uint32_t word(std::size_t offset) const;
    float number(std::size_t offset) const;
    float sample(std::uint64_t index) const;
    std::uint32_t relative(std::uint32_t at) const;
    void run();
    void advance(Channel& channel);
    void read_track(Channel& channel);
    void evaluate(Channel& channel);
};
}
