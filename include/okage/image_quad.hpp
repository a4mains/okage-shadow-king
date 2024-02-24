#pragma once
#include <array>
#include <cstdint>

namespace okage {
struct ImageQuad {
    std::array<std::array<std::uint32_t,2>,4> xy16;
    std::array<float,4> st;
    std::uint32_t rgba, z;
};
ImageQuad axis_aligned_image_quad(std::uint32_t width,std::uint32_t height,
                                 const std::array<std::uint32_t,6>& fp_words);
}
