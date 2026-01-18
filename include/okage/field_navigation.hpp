#pragma once
#include "okage/field.hpp"
#include "okage/image_quad.hpp"
#include <bit>

namespace okage {
class FieldNavigation {
public:
    FieldNavigation(const FieldDefinition&,unsigned width,unsigned height,unsigned marker_width,unsigned marker_height);
    std::vector<ImageQuad> queue(const Float4& player_position,const Float4& player_rotation,bool toggle,const SineTable&);
    bool visible() const noexcept{return visible_;}
    float center() const noexcept{return center_;}
    float pulse() const noexcept{return pulse_;}
    float delta() const noexcept{return delta_;}
private:
    unsigned width_,height_,marker_width_,marker_height_,flags_;
    float scale_,offset_x_,offset_z_,center_=-3840.f,pulse_=.5f,delta_=std::bit_cast<float>(0x3ca3d70au);
    bool visible_=true;
};
}
