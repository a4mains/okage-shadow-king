#include "okage/ui_matrix.hpp"
#include <bit>
#include <cfenv>
#include <cstdint>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
float normal(float f){
    const auto bits=__builtin_bit_cast(std::uint32_t, (f)),exp=bits&0x7f800000u;
    require(exp!=0x7f800000u&&(exp!=0||(bits&0x7fffffu)==0),"UI matrix exceptional float");return f;
}
template<class T> void validate(const T& a){for(float f:a)normal(f);}
void validate_matrix(const Matrix4& m){for(const auto& c:m)validate(c);}
struct Rounding {
    std::fenv_t saved;
    Rounding(){require(std::fegetenv(&saved)==0,"UI matrix save fenv");
        if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI matrix rounding");}}
    ~Rounding(){std::fesetenv(&saved);}
};
float op(float a,float b,char kind){
    volatile float x=normal(a),y=normal(b),out;
    require(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI matrix clear fenv");
    if(kind=='+')out=x+y;else if(kind=='-')out=x-y;else out=x*y;
    require(!std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW),"UI matrix overflow/underflow");return normal(out);
}
float mul(float a,float b){return op(a,b,'*');}
float add(float a,float b){return op(a,b,'+');}
float sub(float a,float b){return op(a,b,'-');}
unsigned index(float angle){
    normal(angle);
    const auto fixed=angle>=65536.f?0x7fffffffu:angle<=-65536.f?0x80000000u:
        std::uint32_t(static_cast<std::int32_t>(mul(angle,32768.f)));
    return (fixed>>1)&0x3fffu;
}
void rotate(Matrix4& matrix,unsigned first,unsigned second,unsigned at,std::span<const float> sine){
    const auto c=normal(sine[(at+4096)&0x3fff]),s=normal(sine[at]);
    const auto a=matrix[first],b=matrix[second];
    Float4 saved;for(unsigned r=0;r<4;++r)saved[r]=mul(a[r],1.f);
    for(unsigned r=0;r<4;++r)matrix[first][r]=add(mul(a[r],c),mul(b[r],s));
    for(unsigned r=0;r<4;++r)matrix[second][r]=sub(mul(b[r],c),mul(saved[r],s));
}
Float4 transform(const Matrix4& m,const Float4& v){
    Float4 result;
    for(unsigned r=0;r<4;++r){
        auto a=mul(m[0][r],v[0]);a=add(a,mul(m[1][r],v[1]));
        a=add(a,mul(m[2][r],v[2]));result[r]=add(a,mul(m[3][r],v[3]));
    }
    return result;
}
}
Matrix4 ui_matrix_identity(){
    // 01a8 subtracts VF0 from itself in all four columns, then adds 1 to the diagonal.
    return {{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
}
Matrix4 ui_matrix_rotate_zxy(const Matrix4& source,const UiMatrixVector& angles,std::span<const float> sine){
    validate_matrix(source);validate(angles);require(sine.size()==16384,"UI matrix sine table extent");
    Rounding rounding;auto result=source;
    rotate(result,0,1,index(angles[2]),sine); //Z0420
    rotate(result,1,2,index(angles[0]),sine); //X0490
    rotate(result,2,0,index(angles[1]),sine); //Y0458
    return result;
}
Matrix4 ui_matrix_translate(const Matrix4& source,const UiMatrixVector& position){
    validate_matrix(source);validate(position);Rounding rounding;auto result=source;
    result[3]=transform(source,{position[0],position[1],position[2],1.f});return result;
}
Matrix4 ui_matrix_left_multiply(const Matrix4& parent,const Matrix4& local){
    validate_matrix(parent);validate_matrix(local);Rounding rounding;Matrix4 result;
    for(unsigned c=0;c<4;++c)result[c]=transform(parent,local[c]);return result;
}
Matrix4 ui_sprite_parent_matrix(const UiMatrixVector& position,const UiMatrixVector& rotation,
                               bool rotations,std::span<const float> sine){
    auto m=ui_matrix_identity();if(rotations)m=ui_matrix_rotate_zxy(m,rotation,sine);
    return ui_matrix_translate(m,position);
}
Matrix4 ui_sprite_element_matrix(const Matrix4& parent,const UiMatrixVector& translation,
                                std::span<const float> sine){
    auto m=ui_matrix_rotate_zxy(ui_matrix_identity(),{0,0,0},sine);
    m=ui_matrix_translate(m,translation);return ui_matrix_left_multiply(parent,m);
}
Matrix4 ui_glyph_parent_matrix(const UiMatrixVector& position,const UiMatrixVector& rotation,
                              bool rotations,std::span<const float> sine){
    auto m=ui_matrix_translate(ui_matrix_identity(),position);
    if(rotations)m=ui_matrix_rotate_zxy(m,rotation,sine);return m;
}
}
