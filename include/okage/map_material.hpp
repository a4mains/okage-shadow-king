#pragma once
#include "okage/render.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace okage {
struct MapMaterialClass {
    bool translucent;
    bool base_lighting;
};
// Source draw command bits are FGE0x20 and optional TME0x10 in the
// reviewed ordinary map corpus. A retained texture selection does not enable it.
inline bool map_material_textured(const Polygon& polygon) {
    if(polygon.flags!=0x20&&polygon.flags!=0x30)throw std::runtime_error("Unrecovered map polygon flags");
    const bool textured=(polygon.flags&0x10)!=0;
    if(textured&&polygon.texture_state<0)throw std::runtime_error("Textured map polygon has no texture state");
    return textured;
}
// Source compiler 0x179ed0/0x17b298. A positive source transparency belongs
// to the translucent list even when its compiled alpha still rounds to128.
inline MapMaterialClass map_material_class(const Material& material,int normal) {
    const auto transparency=__builtin_bit_cast(float, (material.words[5]));
    if(!std::isfinite(transparency)||transparency<0||transparency>1)
        throw std::runtime_error("Map material transparency outside recovered range");
    if(material.words[7]&~1u)throw std::runtime_error("Unrecovered map material flags");
    if(material.words[6]!=0)throw std::runtime_error("Unrecovered map material blend class");
    return {transparency>0,normal<0||(material.words[7]&1)!=0};
}
inline bool map_material_block_uses_normals(const RenderBlock& block) {
    for(const auto& polygon:block.polygons) {
        if(polygon.material<0)throw std::runtime_error("Map polygon has no material");
        const auto& material=block.materials.at(polygon.material);
        if(!(material.words[7]&1))for(const auto& corner:polygon.corners)if(corner[1]>=0)return true;
    }
    return false;
}
inline void validate_map_material_extra_pass(const Material& material,bool normal_block) {
    if(!normal_block)return;
    const auto exponent=__builtin_bit_cast(float, (material.words[4]));
    if(!std::isfinite(exponent)||exponent>70)
        throw std::runtime_error("Unrecovered map material extra lighting pass");
}
template<class Draw,class Submit>
void map_translucent_passes(std::span<const Draw> draws,Submit submit) {
    for(unsigned alpha_test:{1u,2u})for(const auto& draw:draws)submit(draw,alpha_test);
}
}
