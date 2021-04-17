#pragma once
#include "okage/model.hpp"
#include <functional>
#include <utility>

namespace okage {
class Animation {
public:
    Animation(Bytes bytes, std::size_t channel_count);
    void start(std::size_t entry);
    void select(std::size_t entry); // Select for the next script dispatch (0x1b4da8).
    void set_channel(std::size_t index,const Float4& value);
    // 0x1b4700 changes the mode used by subsequent channel transitions.
    void set_channel_mode(std::size_t index,unsigned mode);
    // Collision records point to evaluated output (retail channel +0x20).
    // A direct push does not cancel the channel's queued interpolation.
    void write_channel_output(std::size_t index,const Float4& value);
    void set_component(std::size_t index,unsigned component,float value);
    void turn_component(std::size_t index,unsigned component,float target,int duration);
    bool moving(std::size_t index,unsigned component) const;
    void set_channel_target(Animation* target){channel_target_=target;}
    // Start a recovered script segment. This is not a whole-scene initializer.
    void start_at(std::uint32_t offset);
    struct ServiceResult {std::uint32_t next_pc;bool yield=false;};
    using Service=std::function<ServiceResult(unsigned,unsigned,std::int32_t,std::uint32_t)>;
    void set_service_callback(Service service){service_=std::move(service);}
    void tick();
    void tick_script();
    void tick_channels();
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
    Service service_;
    Animation* channel_target_=nullptr;
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
