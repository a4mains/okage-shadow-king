#include "okage/actor.hpp"
#include <stdexcept>
namespace okage {
namespace {
std::size_t channel_count(const ModelFile& model,Bytes motion) {
    if(model.models.size()!=1 || motion.size()<32)throw std::runtime_error("Actor requires one model and extended motion header");
    const bool scale=motion[20]||motion[21]||motion[22]||motion[23];
    return 4+model.models[0].bones.size()*(scale ? 3 : 2);
}
}
FieldActor::FieldActor(ModelFile model):model_(std::move(model)),textures_(model_,{}),animation_(4),has_motion_(false) {
    if(model_.models.size()!=1)throw std::runtime_error("Static actor requires one model");
    animation_.set_channel(0,{0,0,0,1});animation_.set_channel(1,{0,0,0,1});animation_.set_channel(2,{1,1,1,1});
    channels_=animation_.channel_outputs(); //Dirty world values remain deferred.
}
FieldActor::FieldActor(ModelFile model,Bytes motion,TextureAnimation textures,bool defer_transform_evaluation,
                       bool bone_only_initial_evaluation):
    model_(std::move(model)),textures_(model_,std::move(textures)),animation_(motion,channel_count(model_,motion)) {
    animation_.set_texture_callback([this](auto group,auto variant){textures_.select(group,variant);});
    animation_.start(0);animation_.tick_channels();
    if(bone_only_initial_evaluation)evaluate_bones();else evaluate();
    // Original instance construction 0x1b64d8 initializes transform channels.
    animation_.set_channel(0,{0,0,0,1});animation_.set_channel(1,{0,0,0,1});animation_.set_channel(2,{1,1,1,1});
    if(!defer_transform_evaluation)evaluate();
}
void FieldActor::select_motion(unsigned entry) {
    if(!has_motion_)throw std::runtime_error("Cannot select motion on a static actor");
    textures_.restore();animation_.select(entry);motion_=entry;
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
