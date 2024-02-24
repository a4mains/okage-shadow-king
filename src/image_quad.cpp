#include "okage/image_quad.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace okage {
ImageQuad axis_aligned_image_quad(std::uint32_t width,std::uint32_t height,
                                 const std::array<std::uint32_t,6>& words) {
    const auto check=[](bool v,const char* why){if(!v)throw std::runtime_error(why);};
    check(width>0&&width<4096&&height>0&&height<4096,"Image source descriptor dimensions");
    check(words[2]==0x3f800000&&words[3]==0x3f800000&&words[4]==0,
          "Unrecovered image scale/rotation");
    const float x=std::bit_cast<float>(words[0]),y=std::bit_cast<float>(words[1]);
    check(std::isfinite(x)&&std::isfinite(y)&&x==std::trunc(x)&&y==std::trunc(y)&&
          x>=-65535&&x<=65535&&y>=-65535&&y<=65535,"Unrecovered fractional/out-of-range image position");
    const auto cx=32768+std::int32_t(x),cy=32768+std::int32_t(y);
    const auto dx=std::int32_t(width*8),dy=std::int32_t(height*4);
    const auto left=cx-dx,right=cx+dx,top=cy-dy,bottom=cy+dy;
    check(left>=0&&right<=65535&&top>=0&&bottom<=65535,"Image XY outside native GS16 support");
    const float alpha=std::bit_cast<float>(words[5]);
    check(std::isfinite(alpha)&&alpha>=-1.f&&alpha<=1.f,"Image alpha outside recovered title range");
    const auto packed_alpha=std::uint32_t(std::int32_t(alpha*128.f))<<24;
    return {{{{std::uint32_t(left),std::uint32_t(top)},
              {std::uint32_t(right),std::uint32_t(top)},
              {std::uint32_t(right),std::uint32_t(bottom)},
              {std::uint32_t(left),std::uint32_t(bottom)}}},
        {.5f/1024,.5f/1024,(float(width)-.5f)/1024,(float(height)-.5f)/1024},
        packed_alpha+0x808080,0xffff};
}
}
