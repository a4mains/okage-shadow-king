#include "okage/render.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int previous=std::fegetround();
    Rounding() { if (std::fesetround(FE_TOWARDZERO)) throw std::runtime_error("Cannot set UV rounding mode"); }
    ~Rounding() { std::fesetround(previous); }
};
std::array<float,2> region_dimensions(const TextureState& state,std::uint32_t width,std::uint32_t height) {
    if (!width || !height || width>4095 || height>4095)
        throw std::runtime_error("Texture dimensions exceed original descriptor fields");
    for (float value:state.region)
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite texture region");
    if (state.region[2]>1.0f || state.region[3]>1.0f) return {1,1};
    return {float(width),float(height)};
}
}
std::array<float,2> texture_coordinates(const TextureState& state,
    std::uint32_t width,std::uint32_t height,std::array<float,2> source) {
    const auto dimensions=region_dimensions(state,width,height);
    for (float value:source)
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite texture coordinate");
    Rounding rounding;
    const std::int16_t modes[]={state.u_mode,state.v_mode};
    for (unsigned axis=0;axis<2;++axis) {
        float scale=(state.region[axis+2]-state.region[axis])*dimensions[axis];
        scale=scale*float(std::max<int>(1,modes[axis]));
        float value=source[axis]*scale;
        if (modes[axis]==1) value=value+state.region[axis]*dimensions[axis];
        source[axis]=(value+0.5f)*0.0009765625f;
        if (!std::isfinite(source[axis])) throw std::runtime_error("Texture coordinate overflow");
    }
    return source;
}
std::uint64_t texture_clamp(const TextureState& state,std::uint32_t width,std::uint32_t height) {
    const auto dimensions=region_dimensions(state,width,height);
    Rounding rounding;
    const std::int16_t modes[]={state.u_mode,state.v_mode};
    std::uint64_t result=0;
    auto integer=[](float value) -> std::uint64_t {
        if (!std::isfinite(value) || value<-2147483648.0f || value>=2147483648.0f)
            throw std::runtime_error("Texture region exceeds signed integer conversion");
        return std::uint64_t(std::int64_t(std::int32_t(value)));
    };
    for (unsigned axis=0;axis<2;++axis) {
        const float origin=state.region[axis], end=state.region[axis+2], dim=dimensions[axis];
        float first,second;
        if (modes[axis]==1) {
            result|=std::uint64_t(2)<<(axis*2);
            first=origin*dim; second=end*dim-1.0f;
        } else {
            result|=std::uint64_t(3)<<(axis*2);
            first=(end-origin)*dim-1.0f; second=origin*dim;
        }
        result|=integer(first)<<(4+axis*20);
        result|=integer(second)<<(14+axis*20);
    }
    return result;
}
std::vector<std::array<unsigned,3>> triangle_corners(std::size_t count) {
    if (count==3) return {{0,1,2}};
    if (count==4) return {{0,1,2},{0,2,3}};
    throw std::runtime_error("Unvalidated source polygon size");
}
}
