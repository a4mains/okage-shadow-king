#pragma once
#include "okage/effect.hpp"
#include "okage/assets.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace effect_harness {
using Data=std::vector<std::uint8_t>;
inline Data read_file(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read "+path.string());
    return Data(std::istreambuf_iterator<char>(in),{});
}
struct Owned {
    std::vector<okage::EffectDefinition> raw,resolved;
    okage::EffectTextureSizes sizes{};
};
inline std::optional<Owned> load_owned(const std::filesystem::path& root) {
    if(!std::filesystem::exists(root/"TES.EFD")||!std::filesystem::exists(root/"ENV_TEX.XPF"))return std::nullopt;
    Owned owned;
    owned.raw=okage::read_efd(read_file(root/"TES.EFD"));
    owned.resolved=okage::resolve_effect_references(owned.raw);
    for(unsigned i=0;i<owned.sizes.size();++i) {
        const auto image=okage::decode_xim(read_file(root/"ENV_TEX.XPF"/okage::effect_texture_names[i]));
        owned.sizes[i]={image.width,image.height};
    }
    return owned;
}
//Deterministic, well-mixed RNG words (xorshift32); the game RNG is not modelled.
struct Random {
    std::uint32_t state=0x1234567u;unsigned calls=0;
    std::uint32_t operator()(){++calls;state^=state<<13;state^=state>>17;state^=state<<5;return state&0x7fffffffu;}
};
struct Inputs {
    okage::Matrix4 view;okage::Float4 camera;okage::Float4 eye;okage::Float4 tint;
    okage::EffectProjectionContext projection;
};
inline Inputs make_inputs() {
    const auto sine=okage::make_sine_table();
    Inputs in;
    in.camera={.05f,.3f,0,1};in.eye={4,-20,-60,1};in.tint={1,.9f,.8f,1};
    in.view=okage::camera_matrix({-4,20,60,1},{-.05f,-.3f,0,0},{1,1,1,0},sine);
    in.projection.view=in.view;in.projection.focal=512;
    in.projection.screen={};
    in.projection.screen[0]={1,1,1,0};in.projection.screen[1]={0,0,0,1};
    in.projection.screen[4]={2048,2048,0,0};in.projection.screen[5]={1,1,1,0};
    return in;
}
//Template ids whose lifetime the harness must not exceed; looping jobs
//(flag0x8000) are sampled for a bounded number of updates.
inline unsigned update_count(const okage::EffectDefinition& d,unsigned cap) {
    return std::min<unsigned>(d.duration+2,cap);
}
}
