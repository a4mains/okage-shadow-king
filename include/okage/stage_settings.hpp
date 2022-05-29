#pragma once
#include "okage/field.hpp"
#include "okage/field_shadows.hpp"
#include "okage/stage_data.hpp"
#include <optional>

namespace okage {
struct StageSettings {
    explicit StageSettings(SceneClearColor clear_color={});
    FieldOrdinaryLighting lighting;
    FieldShadowSettings shadows;
    Float4 sky{.5f,0,.5f,0};
    SceneClearColor clear;
    std::uint32_t clear_word=0,fog_rgb=0x404040;
    std::array<std::optional<std::array<std::uint32_t,4>>,9> filters;
    std::uint32_t bucket_shift=9,bucket_base=19,selector=0xffffffff,signed_setting=0;
    float cached_depth_min=101,cached_depth_max=16777115;
    void apply(const StageSettingRecord&,FieldProjection&);
private:
    void initialize_filter(unsigned);
    void disable_filter(unsigned);
};
}
