#pragma once
#include "okage/pose.hpp"
#include <span>

namespace okage {
using UiMatrixVector = std::array<float,3>;
Matrix4 ui_matrix_identity();
Matrix4 ui_matrix_rotate_zxy(const Matrix4&,const UiMatrixVector& angles,
                            std::span<const float> sine);
// 0280: basis-weighted XYZ plus existing translation*1; no input W is consumed.
Matrix4 ui_matrix_translate(const Matrix4&,const UiMatrixVector& position);
// 0390: parent*local, all four terms including zero products, in source order.
Matrix4 ui_matrix_left_multiply(const Matrix4& parent,const Matrix4& local);
Matrix4 ui_sprite_parent_matrix(const UiMatrixVector& position,
    const UiMatrixVector& rotation,bool rotate,std::span<const float> sine);
Matrix4 ui_sprite_element_matrix(const Matrix4& parent,
    const UiMatrixVector& translation,std::span<const float> sine);
Matrix4 ui_glyph_parent_matrix(const UiMatrixVector& position,
    const UiMatrixVector& rotation,bool rotate,std::span<const float> sine);
}
