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
FieldActor::FieldActor(ModelFile model,Bytes motion,TextureAnimation textures):
    model_(std::move(model)),textures_(model_,std::move(textures)),animation_(motion,channel_count(model_,motion)) {
    animation_.set_texture_callback([this](auto group,auto variant){textures_.select(group,variant);});
    animation_.start(0);animation_.tick_channels();evaluate();
    // Original instance construction 0x1b64d8 initializes transform channels.
    animation_.set_channel(0,{0,0,0,1});animation_.set_channel(1,{0,0,0,1});animation_.set_channel(2,{1,1,1,1});
    evaluate();
}
void FieldActor::select_motion(unsigned entry) {
    textures_.restore();animation_.select(entry);motion_=entry;
}
void FieldActor::tick_script(){animation_.tick_script();}
void FieldActor::tick_channels(){animation_.tick_channels();}
void FieldActor::evaluate(){channels_=animation_.values();}
Matrix4 FieldActor::world(const SineTable& sine) const{return instance_matrix(channels_.at(1),channels_.at(0),channels_.at(2),sine);}
}
