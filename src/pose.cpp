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
    Rounding rounding(FE_TONEAREST);
    SineTable table;
    for (unsigned i=0;i<table.size();++i) {
        float angle=static_cast<float>((double(i)/8192.0)*double(3.1415927410125732421875f));
        table[i]=std::sin(angle);
    }
    table[0]=0; table[4096]=1; table[8192]=0; table[12288]=-1;
    return table;
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
