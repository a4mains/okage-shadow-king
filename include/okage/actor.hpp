#pragma once
#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include "okage/pose.hpp"
namespace okage {
class FieldActor {
public:
    FieldActor(ModelFile model,Bytes motion,TextureAnimation textures);
    FieldActor(const FieldActor&)=delete;
    FieldActor& operator=(const FieldActor&)=delete;
    void select_motion(unsigned entry);
    void tick_script();
    void tick_channels();
    void evaluate();
    Matrix4 world(const SineTable& sine) const;
    const ModelFile& model() const{return model_;}
    const TextureBindings& textures() const{return textures_;}
    const std::vector<Float4>& channels() const{return channels_;}
    Animation& animation(){return animation_;}
    const Animation& animation() const{return animation_;}
    unsigned motion() const{return motion_;}
private:
    ModelFile model_;
    TextureBindings textures_;
    Animation animation_;
    std::vector<Float4> channels_;
    unsigned motion_=0;
};
}
