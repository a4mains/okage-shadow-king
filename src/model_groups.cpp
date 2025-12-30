#include "okage/model_groups.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace okage {
const ModelResourceGroup& ModelGroupOwner::insert(std::string name, unsigned count,
                                                ResourceTextures textures, bool reported) {
    auto lease = ranges_.allocate(count);
    if (!lease) throw std::runtime_error("179780 model range exhausted after texture load");
    auto group = std::make_unique<ModelResourceGroup>(ModelResourceGroup{
        std::move(name), *lease, std::move(textures), reported, false});
    groups_.push_back(std::move(group));
    return *groups_.back();
}
const ModelResourceGroup* ModelGroupOwner::find(const ModelRangeLease& range) const noexcept {
    if (!ranges_.live(range)) return nullptr;
    for (const auto& group : groups_)
        if (!group->retired && group->range == range) return group.get();
    return nullptr;
}
ModelResourceGroup* ModelGroupOwner::find(const ModelRangeLease& range) noexcept {
    return const_cast<ModelResourceGroup*>(std::as_const(*this).find(range));
}
bool ModelGroupOwner::retire(const ModelRangeLease& range) {
    if (!find(range)) return false;
    for (auto& group : groups_) if (!group->retired && group->range == range) {
        textures_.retire_xmd(group->textures);
        if (!ranges_.release(range)) throw std::logic_error("model range lost during group retirement");
        group->retired = true;
        return true;
    }
    throw std::logic_error("live range without material group");
}
void ModelGroupOwner::retire_source_id(unsigned model) {
    for (const auto& group : groups_)
        if (!group->retired && model >= group->range.first && model - group->range.first < group->range.count) {
            retire(group->range);
            return;
        }
}
void ModelGroupOwner::bind_instance(unsigned instance, ModelRangeLease main,
                                   std::optional<ModelRangeLease> shadow) {
    if (instance >= instances_.size() || instances_[instance])
        throw std::logic_error("1b8a80 invalid or occupied instance");
    if (!find(main) || (shadow && !find(*shadow)))
        throw std::logic_error("1b8a80 instance references a stale model group");
    instances_[instance] = ActorModelRegistration{main, shadow};
}
unsigned ModelGroupOwner::allocate_instance(ModelRangeLease main, std::optional<ModelRangeLease> shadow, unsigned flags) {
    if (front_ >= back_ || ((flags & 0x100000) && front_ + 1 >= back_))
        throw std::runtime_error("1b8a80 instance pool exhausted");
    const auto instance = flags & 0x100000 ? back_ - 1 : front_;
    bind_instance(instance, main, ((flags>>5)&3)>=2?shadow:std::nullopt);
    if (flags & 0x100000) --back_; else ++front_;
    return instance;
}
void ModelGroupOwner::retire_instance(unsigned instance) {
    if (instance >= instances_.size()) throw std::out_of_range("model instance");
    if (!instances_[instance]) return;
    const auto binding = *instances_[instance];
    if (binding.shadow) retire_source_id(binding.shadow->first);
    retire_source_id(binding.main.first);
    instances_[instance].reset();
}
void ModelGroupOwner::retire_associated() {
    for (const auto& instance : instances_)
        if (instance && instance->shadow) retire_source_id(instance->shadow->first);
}
void ModelGroupOwner::retire_main() {
    for (auto& instance : instances_) if (instance) {
        retire_source_id(instance->main.first);
        instance.reset();
    }
    textures_.retire_scene_images();
}
void ModelGroupOwner::reset_instances_and_registrations() {
    for (auto& instance : instances_) instance.reset();
    actors_.clear();
    front_ = 0; back_ = 640;
}
std::string ModelGroupOwner::normalize(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return name;
}
const ActorModelRegistration* ModelGroupOwner::actor(const std::string& name) const {
    const auto found = actors_.find(normalize(name));
    return found == actors_.end() ? nullptr : &found->second;
}
void ModelGroupOwner::register_actor(const std::string& name, ActorModelRegistration registration) {
    if (!find(registration.main) || (registration.shadow && !find(*registration.shadow)))
        throw std::logic_error("1b79b8 stale model registration");
    if (!actors_.emplace(normalize(name), registration).second)
        throw std::logic_error("1b79b8 duplicate registration after a cache hit");
}
}
