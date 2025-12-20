#pragma once
#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include "okage/pose.hpp"
#include <memory>
#include <optional>
namespace okage {
class FieldActor {
public:
    //1b64d8 without XMT owns four world/color channels and no motion context.
    explicit FieldActor(ModelFile model,std::shared_ptr<TextureBindings> packet_textures={});
    FieldActor(ModelFile model,Bytes motion,TextureAnimation textures,bool defer_transform_evaluation=false,
               bool bone_only_initial_evaluation=false,
               std::shared_ptr<TextureBindings> packet_textures={},
               bool defer_initial_motion_evaluation=false);
    FieldActor(const FieldActor&)=delete;
    FieldActor& operator=(const FieldActor&)=delete;
    void initialize_motion();
    void select_motion(unsigned entry);
    void tick_script();
    void tick_channels();
    void evaluate();
    void evaluate_bones();
    void evaluate_world();
    Matrix4 world(const SineTable& sine) const;
    const ModelFile& model() const{return model_;}
    const TextureBindings& textures() const{return *textures_;}
    void configure_texture_source(const void* owner, TextureBindings::SourceRegisterResolver resolver) const {
        textures_->configure_source_registers(owner,std::move(resolver));
    }
    const std::vector<Float4>& channels() const{return channels_;}
    Animation& animation(){return animation_;}
    const Animation& animation() const{return animation_;}
    unsigned motion() const{return motion_;}
    bool has_motion() const{return has_motion_;}
    bool visible=true; // Instance flag0x800; scene gates channel ticks, scripts keep running.
    bool encounter_model=false;
    std::uint32_t render_flags=0;
    // 0x1baae8 overrides instance+0x2c. Empty retains model construction's
    // default. Shadow submission is still an explicit scene-renderer gap.
    std::optional<unsigned> shadow_mode_override;
private:
    ModelFile model_;
    std::shared_ptr<TextureBindings> textures_;
    Animation animation_;
    std::vector<Float4> channels_;
    unsigned motion_=0;
    bool has_motion_=true;
    bool motion_initialized_=false;
    bool defer_transform_evaluation_=false;
    bool bone_only_initial_evaluation_=false;
};
// Field actor construction may omit a collision body (STUN).
struct ActorBinding {int alias;FieldActor* actor;bool collidable=true;int instance=-1;};
}
