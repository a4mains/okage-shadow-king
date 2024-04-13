#include "okage/chapter.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
template<class T> const T& required(const T& callback, const char* message) {
    if(!callback)throw std::runtime_error(message);
    return callback;
}
}
void chapter_update(ChapterState& state, const ChapterUpdateServices& services) {
    if(state.phase!=19)throw std::runtime_error("Unrecovered chapter manager phase");
    //1344bc..1344e0: descriptor0; XY0, scale1/1, rotation0, alpha1.
    static constexpr GameOverImageCall image{0,{0,0,0x3f800000,0x3f800000,0,0x3f800000}};
    required(services.submit_image,"Missing chapter image service")(image);
    ++state.counter;
    if(std::bit_cast<std::int32_t>(state.counter)<240)return;
    if(state.counter==240) {
        required(services.request_fade,"Missing chapter fade request service")(0.f);
        return;
    }
    if(required(services.fade_pending,"Missing chapter fade status service")())return;
    required(services.clear_command,"Missing chapter command clear service")();
    //Read saved destinations after clear, as134534/44 do after1248d8 returns.
    required(services.request_mode,"Missing chapter mode request service")(
        {2,state.destination_scene,state.destination_entrance});
}
} // namespace okage
