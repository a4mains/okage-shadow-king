#include "okage/startup.hpp"
#include <bit>
#include <stdexcept>
namespace okage {
namespace {
template<class T> const T& required(const T& callback,const char* why) {
    if(!callback)throw std::runtime_error(why);return callback;
}
}
void startup_logo_update(StartupTaskState& state,GameOverInputState& input,GameOverPadSample pad,
                         const StartupUpdateServices& services) {
    auto& task=state.common;
    if(task.submode!=0)throw std::runtime_error("Logo updater requires mode8 selector0");
    mode8_update_prefix(task,input,pad,services.common);
    ++task.tick_counter;
    if(task.tick_counter==1) {
        state.image_index=0;task.generic_alpha_rate_bits=0x3ccccccd;
    }
    if(task.tick_counter==180)task.generic_alpha_rate_bits=0xbccccccd;
    if(task.tick_counter==270) {
        required(services.initialize_shared,"Missing logo shared initialization service")();
        state.image_index=1;task.generic_alpha_rate_bits=0x3ccccccd;
    }
    //12966c rereads counter after the shared service; do not cache it above.
    if(task.tick_counter==480)task.generic_alpha_rate_bits=0xbccccccd;
    if(task.tick_counter==600)
        required(services.common.request_mode,"Missing logo mode request service")({8,1,0});
    //1296a8 reloads after request. Counts with the sign bit set still submit.
    if(std::bit_cast<std::int32_t>(task.tick_counter)<600) {
        const GameOverImageCall image{state.image_index,{0,0,0x3f800000,0x3f800000,0,0x3f800000}};
        required(services.common.submit_image,"Missing logo image service")(image);
    }
}
}
