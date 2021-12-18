#pragma once
#include <charconv>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string_view>

namespace okage {
struct NativeRenderRange { unsigned first,last; };
inline NativeRenderRange native_render_range(std::string_view text) {
    const auto colon=text.find(':');
    if(colon==text.npos)throw std::runtime_error("Render range requires positive FIRST:LAST");
    const auto number=[](std::string_view token) {
        unsigned value=0;
        const auto [end,error]=std::from_chars(token.data(),token.data()+token.size(),value);
        if(error!=std::errc{}||end!=token.data()+token.size()||!value)
            throw std::runtime_error("Render range requires positive FIRST:LAST");
        return value;
    };
    const NativeRenderRange range{number(text.substr(0,colon)),number(text.substr(colon+1))};
    if(range.first>range.last)throw std::runtime_error("Render range requires FIRST <= LAST");
    return range;
}
inline void validate_native_render_ranges(std::span<const NativeRenderRange> ranges,unsigned replay_frames) {
    for(const auto range:ranges)
        if(!replay_frames||!range.first||range.first>range.last||range.last>replay_frames)
            throw std::runtime_error("Render ranges require positive ordered endpoints within the headless replay");
}
inline bool native_render_tick(unsigned frame,unsigned from,unsigned until,
        std::span<const NativeRenderRange> ranges) {
    if(from&&frame>=from&&(!until||frame<=until))return true;
    for(const auto range:ranges)if(frame>=range.first&&frame<=range.last)return true;
    return false;
}

struct NativeRunPlan {
    unsigned replay_frames=0;
    bool starts_paused=false,bounded_handoff=false;
    std::filesystem::path replay_capture,live_capture;
};

inline NativeRunPlan native_run_plan(unsigned frames,unsigned preview,unsigned ui_frames,
        const std::filesystem::path& capture,unsigned limit) {
    if(frames>limit || preview>limit || ui_frames>limit ||
       (frames&&(preview||ui_frames)) || (ui_frames&&capture.empty()) ||
       (preview&&ui_frames&&ui_frames<=preview))
        throw std::runtime_error("Native diagnostic frame/capture bounds");
    const bool handoff=preview&&ui_frames;
    return {frames?frames:preview,bool(preview&&!ui_frames),handoff,
        handoff?std::filesystem::path(capture.string()+".preview.png"):capture,
        ui_frames?capture:std::filesystem::path{}};
}

inline void validate_native_capture_selection(const NativeRunPlan& plan,
        std::span<const unsigned> ticks) {
    if(!ticks.empty()&&(!plan.replay_frames||plan.replay_capture.empty()||ticks.back()>plan.replay_frames))
        throw std::runtime_error("Native selected captures require an output and ticks within a replay");
}

inline void require_fresh_native_capture_family(const std::filesystem::path& capture) {
    const auto name=capture.filename().string();
    if(name.empty()||name=="."||name=="..")throw std::runtime_error("Invalid native capture filename");
    const auto parent=capture.has_parent_path()?capture.parent_path():std::filesystem::path(".");
    if(!std::filesystem::is_directory(parent))
        throw std::runtime_error("Combined native capture requires an existing output directory");
    for(const auto& entry:std::filesystem::directory_iterator(parent)) {
        const auto candidate=entry.path().filename().string();
        if(candidate==name||candidate.starts_with(name+"."))
            throw std::runtime_error("Combined native capture requires a fresh output family");
    }
}

inline bool native_capture_family_contains(const std::filesystem::path& capture,
        const std::filesystem::path& other) {
    const auto absolute=std::filesystem::weakly_canonical(std::filesystem::absolute(capture));
    const auto relative=std::filesystem::weakly_canonical(std::filesystem::absolute(other)).lexically_relative(absolute.parent_path());
    if(relative.empty())return false;
    const auto first=relative.begin()->string(),name=absolute.filename().string();
    return first==name||first.starts_with(name+".");
}
}
