#include "okage/pose.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int previous=std::fegetround();
    explicit Rounding(int mode) { if (std::fesetround(mode)) throw std::runtime_error("Unable to set pose rounding mode"); }
    ~Rounding() { std::fesetround(previous); }
};
float constant(std::uint32_t word) {return __builtin_bit_cast(float, (word));}
std::uint32_t bits(float value) {return __builtin_bit_cast(std::uint32_t, (value));}
float sine_kernel(float x,float tail,bool reduced) {
    if((bits(x)&0x7fffffffu)<0x32000000u)return x;
    const float square=x*x,cube=square*x;
    float polynomial=square*constant(0x2f2ec9d3u);
    polynomial=polynomial+constant(0xb2d72f34u);
    polynomial=square*polynomial;polynomial=polynomial+constant(0x3638ef1bu);
    polynomial=square*polynomial;polynomial=polynomial+constant(0xb9500d01u);
    polynomial=square*polynomial;polynomial=polynomial+constant(0x3c088889u);
    if(!reduced)return x+cube*(square*polynomial+constant(0xbe2aaaabu));
    return x-((square*(tail*.5f-cube*polynomial)-tail)-cube*constant(0xbe2aaaabu));
}
float cosine_kernel(float x,float tail) {
    const auto magnitude=bits(x)&0x7fffffffu;
    if(magnitude<0x32000000u)return 1;
    const float square=x*x;
    float polynomial=square*constant(0xad47d74eu);
    polynomial=polynomial+constant(0x310f74f6u);
    polynomial=square*polynomial;polynomial=polynomial+constant(0xb493f27cu);
    polynomial=square*polynomial;polynomial=polynomial+constant(0x37d00d01u);
    polynomial=square*polynomial;polynomial=polynomial+constant(0xbab60b61u);
    polynomial=square*polynomial;polynomial=polynomial+constant(0x3d2aaaabu);
    polynomial=square*polynomial;
    if(magnitude<=0x3e999999u)return 1.f-(square*.5f-(square*polynomial-x*tail));
    const float split=magnitude>0x3f480000u?constant(0x3e900000u):constant(magnitude-0x1000000u);
    return (1.f-split)-((square*.5f-split)-(square*polynomial-x*tail));
}
float table_sine(float angle) {
    if(!(angle>=0&&angle<=constant(0x40c90fdbu)))throw std::runtime_error("Sine initializer angle outside reviewed domain");
    const auto word=bits(angle);
    if(word<=0x3f490fd8u)return sine_kernel(angle,0,false);
    float head,tail;unsigned quadrant;
    if(word<=0x4016cbe3u) {
        quadrant=1;
        float remainder=angle-constant(0x3fc90f80u),correction=constant(0x37354443u);
        if((word&0x7ffffff0u)==0x3fc90fd0u) {
            remainder=remainder-constant(0x37354400u);correction=constant(0x2e85a308u);
        }
        head=remainder-correction;tail=(remainder-head)-correction;
    } else {
        quadrant=unsigned(angle*constant(0x3f22f984u)+.5f);
        if(quadrant<2||quadrant>4)throw std::runtime_error("Sine initializer quadrant outside reviewed domain");
        const float count=float(quadrant);
        float remainder=angle-count*constant(0x3fc90f80u),correction=count*constant(0x37354443u);
        head=remainder-correction;
        // First four cancellation masks of the reduction table at0x24d9c0.
        constexpr std::uint32_t masks[]={0x3fc90f00u,0x40490f00u,0x4096cb00u,0x40c90f00u};
        auto lost_bits=[&]{return int(word>>23)-int((bits(head)>>23)&255u);};
        if((word&0x7fffff00u)==masks[quadrant-1]&&lost_bits()>=9) {
            float previous=remainder,part=count*constant(0x37354400u);
            remainder=previous-part;
            correction=count*constant(0x2e85a308u)-((previous-remainder)-part);head=remainder-correction;
            if(lost_bits()>=26) {
                previous=remainder;part=count*constant(0x2e85a300u);remainder=previous-part;
                correction=count*constant(0x248d3132u)-((previous-remainder)-part);head=remainder-correction;
            }
        }
        tail=(remainder-head)-correction;
    }
    switch(quadrant&3u) {
        case 0:return sine_kernel(head,tail,true);
        case 1:return cosine_kernel(head,tail);
        case 2:return -sine_kernel(head,tail,true);
        default:return -cosine_kernel(head,tail);
    }
}
Float4 transform(const Matrix4& matrix,const Float4& vector) {
    Float4 result{};
    for (unsigned row=0;row<4;++row) {
        float value=matrix[0][row]*vector[0];
        value=value+matrix[1][row]*vector[1];
        value=value+matrix[2][row]*vector[2];
        result[row]=value+matrix[3][row]*vector[3];
    }
    return result;
}
unsigned angle_index(float radians,bool already_turns=false) {
    // Retail NaN handling is in 0x1829c0; source infinities are not supported.
    if (std::isnan(radians)) radians=0;
    if (!std::isfinite(radians)) throw std::runtime_error("Non-finite bone rotation");
    const float turns=already_turns ? radians : (radians*0.5f)/3.1415927410125732421875f;
    const float fixed=turns*32768.0f;
    if (fixed<=-2147483648.0f || fixed>=2147483648.0f)
        throw std::runtime_error("Bone rotation exceeds quantization range");
    return (static_cast<std::uint32_t>(static_cast<std::int32_t>(fixed))>>1)&0x3fff;
}
void rotate(Matrix4& matrix,unsigned first,unsigned second,unsigned index,const SineTable& sine) {
    if (!index) return;
    const auto c=sine[(index+4096)&0x3fff], s=sine[index];
    const auto a=matrix[first], b=matrix[second];
    for (unsigned row=0;row<4;++row) {
        matrix[first][row]=a[row]*c+b[row]*s;
        matrix[second][row]=b[row]*c-a[row]*s;
    }
}
}
SineTable make_sine_table() {
    // 0x1b1d88 constructs each argument in soft double, rounds it to float,
    // then evaluates the single-precision kernels under the game FPU mode.
    Rounding rounding(FE_TONEAREST);
    SineTable table;
    for(unsigned i=0;i<table.size();++i)
        table[i]=static_cast<float>((double(i)/8192.0)*double(3.1415927410125732421875f));
    {
        Rounding kernel_rounding(FE_TOWARDZERO);
        for(auto& value:table)value=table_sine(value);
    }
    table[0]=0;table[4096]=1;table[8192]=0;table[12288]=-1;
    return table;
}

Matrix4 instance_matrix(const Float4& position,const Float4& rotation,const Float4& scale,const SineTable& sine) {
    Rounding rounding(FE_TOWARDZERO);
    for (const auto& values:{position,rotation,scale}) for(float v:values)
        if(!std::isfinite(v)) throw std::runtime_error("Non-finite instance channel");
    Matrix4 result{{{1,0,0,0},{0,1,0,0},{0,0,1,0},position}};
    result[3][3]=1;
    rotate(result,0,1,angle_index(rotation[2],true),sine);
    rotate(result,2,0,angle_index(rotation[1],true),sine);
    rotate(result,1,2,angle_index(rotation[0],true),sine);
    for(unsigned column=0;column<3;++column)for(unsigned row=0;row<4;++row) result[column][row]*=scale[column];
    return result;
}
std::vector<Matrix4> compose_pose(const Matrix4& parent,std::span<const Matrix4> pose) {
    Rounding rounding(FE_TOWARDZERO);
    for(const auto& column:parent)for(float v:column)
        if(!std::isfinite(v))throw std::runtime_error("Non-finite parent matrix");
    std::vector<Matrix4> result(pose.begin(),pose.end());
    for(auto& matrix:result)for(auto& column:matrix)column=transform(parent,column);
    return result;
}
Matrix4 effect_matrix(const Float4& position,const Float4& rotation,const Float4& scale,const SineTable& sine,bool element) {
    Rounding rounding(FE_TOWARDZERO);
    for(const auto& channel:{position,rotation,scale})for(float v:channel)
        if(!std::isfinite(v))throw std::runtime_error("Non-finite effect transform");
    Matrix4 result{{{1,0,0,0},{0,1,0,0},{0,0,1,0},position}};result[3][3]=1;
    const auto x=angle_index(rotation[0],true),y=angle_index(rotation[1],true),z=angle_index(rotation[2],true);
    if(element){rotate(result,2,0,y,sine);rotate(result,1,2,x,sine);}
    else {rotate(result,1,2,x,sine);rotate(result,2,0,y,sine);}
    rotate(result,0,1,z,sine);
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<4;++r)result[c][r]*=scale[c];
    return result;
}
Matrix4 camera_matrix(const Float4& position,const Float4& rotation,const Float4& scale,const SineTable& sine) {
    Rounding rounding(FE_TOWARDZERO);
    for(const auto& values:{position,rotation,scale})for(float v:values)
        if(!std::isfinite(v))throw std::runtime_error("Non-finite camera channel");
    Matrix4 result{{{scale[0],0,0,0},{0,scale[1],0,0},{0,0,scale[2],0},{0,0,0,1}}};
    rotate(result,0,1,angle_index(rotation[2],true),sine);
    rotate(result,1,2,angle_index(rotation[0],true),sine);
    rotate(result,2,0,angle_index(rotation[1],true),sine);
    result[3]=transform(result,{position[0],position[1],position[2],1});
    return result;
}
static std::vector<Matrix4> build_pose(const Model& model,const SineTable& sine,bool turns) {
    Rounding rounding(FE_TOWARDZERO);
    std::vector<Matrix4> matrices;
    std::vector<Float4> scale;
    for (std::size_t i=0;i<model.hierarchy.size();++i) {
        const auto node=model.hierarchy[i];
        if (node.bone>=model.bones.size() || node.parent<-1 || node.parent>=std::int64_t(i))
            throw std::runtime_error("Invalid pose hierarchy");
        const auto& bone=model.bones[node.bone];
        const Float4 parent_scale=node.parent<0 ? Float4{1,1,1,0} : scale[node.parent];
        Float4 cumulative{};
        Matrix4 local{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
        for (unsigned j=0;j<3;++j) {
            if (!std::isfinite(bone.scale[j]) || !std::isfinite(bone.translation[j]))
                throw std::runtime_error("Non-finite bone scale/translation");
            cumulative[j]=bone.scale[j]*parent_scale[j];
            local[3][j]=bone.translation[j]*parent_scale[j];
        }
        scale.push_back(cumulative);
        rotate(local,0,1,angle_index(bone.rotation[2],turns),sine);
        rotate(local,2,0,angle_index(bone.rotation[1],turns),sine);
        rotate(local,1,2,angle_index(bone.rotation[0],turns),sine);
        if (node.parent>=0) {
            const auto& parent=matrices[node.parent];
            for (auto& column:local) column=transform(parent,column);
        }
        matrices.push_back(local);
    }
    // Parent composition uses unscaled bases. Apply the accumulated scale only
    // after all hierarchy matrices exist (retail 0x184a20 initial mode).
    for (std::size_t i=0;i<matrices.size();++i)
        for (unsigned column=0;column<3;++column)
            for (unsigned row=0;row<4;++row) matrices[i][column][row]*=scale[i][column];
    return matrices;
}
std::vector<Matrix4> rest_pose(const Model& model,const SineTable& sine) {
    return build_pose(model,sine,false);
}
std::vector<Matrix4> animated_pose(const Model& model,std::span<const Float4> channels,
                                 const SineTable& sine,bool scale_channels) {
    const std::size_t stride=scale_channels ? 3 : 2;
    if (channels.size()!=4+stride*model.bones.size()) throw std::runtime_error("Animation bone channel count mismatch");
    Model posed; posed.bones=model.bones; posed.hierarchy=model.hierarchy;
    for (std::size_t i=0;i<posed.bones.size();++i) {
        auto at=4+i*stride;
        if (scale_channels) posed.bones[i].scale=channels[at++];
        posed.bones[i].rotation=channels[at++];
        posed.bones[i].translation=channels[at];
    }
    return build_pose(posed,sine,true);
}
std::vector<Float4> posed_positions(const RenderBlock& block,std::span<const Matrix4> pose) {
    Rounding rounding(FE_TOWARDZERO);
    std::vector<Float4> result(block.attribute_extents[0]);
    for (const auto& range:block.attributes) {
        if (range.kind!=5) continue;
        if (range.hierarchy_node<0 || std::size_t(range.hierarchy_node)>=pose.size() ||
            std::size_t(range.first)+range.values.size()>result.size())
            throw std::runtime_error("Invalid skinned position range");
        for (std::size_t i=0;i<range.values.size();++i) {
            const auto value=transform(pose[range.hierarchy_node],range.values[i]);
            for (unsigned j=0;j<4;++j) result[range.first+i][j]+=value[j];
        }
    }
    return result;
}
}
