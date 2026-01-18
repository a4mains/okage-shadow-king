#include "okage/field_navigation.hpp"
#include <bit>
#include <cmath>
#include <cfenv>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON

namespace okage {
FieldNavigation::FieldNavigation(const FieldDefinition& definition,unsigned width,unsigned height,unsigned marker_width,unsigned marker_height)
    :width_(width),height_(height),marker_width_(marker_width),marker_height_(marker_height),flags_(definition.navigation_flags),
     scale_(definition.navigation_scale),offset_x_(definition.navigation_offset_x),offset_z_(definition.navigation_offset_z) {
    if(definition.type!=0)throw std::runtime_error("Non-type0 navigation draw remains unrecovered");
    if(flags_&~7u||!std::isfinite(scale_)||!std::isfinite(offset_x_)||!std::isfinite(offset_z_))
        throw std::runtime_error("Invalid source navigation mapping");
}
std::vector<ImageQuad> FieldNavigation::queue(const Float4& position,const Float4& rotation,bool toggle,const SineTable& sine) {
    if(toggle)visible_=!visible_;
    struct Rounding {int previous=std::fegetround();Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Navigation rounding mode");}
        ~Rounding(){std::fesetround(previous);}} rounding;
    const auto add=[](float a,float b){volatile float value=a+b;return value;};
    const auto mul=[](float a,float b){volatile float value=a*b;return value;};
    float x=position[0],z=position[2];if(flags_&4u)std::swap(x,z);
    const float sx=(flags_&1u)?-scale_:scale_;
    float sz=mul(scale_,.5f);if(flags_&2u)sz=-sz;
    const float u=add(mul(x,sx),offset_x_),v=add(mul(z,sz),offset_z_);
    const float old_center=center_,adjustment=add(add(old_center,u),4608.f);
    if(!std::isfinite(u)||!std::isfinite(v)||!std::isfinite(rotation[1]))throw std::runtime_error("Non-finite navigation player pose");
    if(adjustment<0)center_=add(center_,-adjustment); //current packets still use old_center
    if(!visible_)return {};
    const auto words=[](float px,float py,float sx,float sy,float angle,float alpha) {
        return std::array<std::uint32_t,6>{std::bit_cast<std::uint32_t>(px),std::bit_cast<std::uint32_t>(py),
            std::bit_cast<std::uint32_t>(sx),std::bit_cast<std::uint32_t>(sy),std::bit_cast<std::uint32_t>(angle),std::bit_cast<std::uint32_t>(alpha)};
    };
    std::vector<ImageQuad> commands;
    commands.push_back(source_image_quad(width_,height_,words(old_center,448,2,2,0,std::bit_cast<float>(0x3f333333u)),sine));
    commands.push_back(source_image_quad(marker_width_,marker_height_,words(add(old_center,u),add(448,v),1,1,add(rotation[1],.5f),pulse_),sine));
    pulse_=add(pulse_,delta_);
    if(pulse_<=std::bit_cast<float>(0x3ecccccdu)||pulse_>=1)delta_=-delta_;
    return commands;
}
}
