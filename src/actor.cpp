#include "okage/actor.hpp"
#include <stdexcept>
namespace okage {
namespace {
std::size_t channel_count(const ModelFile& model,Bytes motion) {
    if(model.models.size()!=1 || motion.size()<32)throw std::runtime_error("Actor requires one model and extended motion header");
    const bool scale=motion[20]||motion[21]||motion[22]||motion[23];
    return 4+model.models[0].bones.size()*(scale ? 3 : 2);
}
std::shared_ptr<TextureBindings> texture_bindings(const ModelFile& model,TextureAnimation animation,
                                                 std::shared_ptr<TextureBindings> shared) {
    auto expected=std::make_shared<TextureBindings>(model,std::move(animation));
    if(!shared)return expected;
    auto original=*shared;original.restore();
    if(shared->names()!=expected->names() ||
       shared->animation().groups!=expected->animation().groups ||
       shared->animation().variants!=expected->animation().variants ||
       original.images()!=expected->images() || shared->images().size()!=expected->images().size())
        throw std::runtime_error("Shared actor texture bindings do not match model/XTA state");
    return shared;
}
}
FieldActor::FieldActor(ModelFile model,std::shared_ptr<TextureBindings> packet_textures):
    model_(std::move(model)),textures_(texture_bindings(model_,{},std::move(packet_textures))),animation_(4),has_motion_(false) {
    if(model_.models.size()!=1)throw std::runtime_error("Static actor requires one model");
    animation_.set_channel(0,{0,0,0,1});animation_.set_channel(1,{0,0,0,1});animation_.set_channel(2,{1,1,1,1});
    channels_=animation_.channel_outputs(); //Dirty world values remain deferred.
}
FieldActor::FieldActor(ModelFile model,Bytes motion,TextureAnimation textures,bool defer_transform_evaluation,
                       bool bone_only_initial_evaluation,std::shared_ptr<TextureBindings> packet_textures):
    model_(std::move(model)),textures_(texture_bindings(model_,std::move(textures),std::move(packet_textures))),
    animation_(motion,channel_count(model_,motion)) {
    animation_.set_texture_callback([this](auto group,auto variant){textures_->select(group,variant);});
    // 0x1b6654 -> 0x1b5bd8 -> 0x1b4da8(entry0) -> 0x1b5830.
    // No 0x1b7528 restore on this path: another instance's packet selection
    // survives until this initial script actually issues an opcode34 callback.
    animation_.start(0);animation_.tick_channels();
    if(bone_only_initial_evaluation)evaluate_bones();else evaluate();
    // Original instance construction 0x1b64d8 initializes transform channels.
    animation_.set_channel(0,{0,0,0,1});animation_.set_channel(1,{0,0,0,1});animation_.set_channel(2,{1,1,1,1});
    if(!defer_transform_evaluation)evaluate();
}
void FieldActor::select_motion(unsigned entry) {
    if(!has_motion_)throw std::runtime_error("Cannot select motion on a static actor");
    // 0x1b6ff8 restores all model packet registrations before 0x1b7004
    // selects the motion context. Later opcode34 -> 0x1b7b28 -> 0x1b7610
    // changes only its group; variant0 restores that group's saved state.
    textures_->restore();animation_.select(entry);motion_=entry;
}
void FieldActor::tick_script(){if(has_motion_)animation_.tick_script();}
void FieldActor::tick_channels(){if(has_motion_)animation_.tick_channels();}
void FieldActor::evaluate(){channels_=animation_.values();}
void FieldActor::evaluate_bones(){
    channels_=animation_.channel_outputs();
    for(std::size_t i=4;i<channels_.size();++i)channels_[i]=animation_.value(i);
}
void FieldActor::evaluate_world(){
    for(std::size_t i=0;i<3;++i)channels_.at(i)=animation_.value(i);
}
Matrix4 FieldActor::world(const SineTable& sine) const{return instance_matrix(channels_.at(1),channels_.at(0),channels_.at(2),sine);}
}
