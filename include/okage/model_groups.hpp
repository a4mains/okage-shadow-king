#pragma once
#include "okage/model_ranges.hpp"
#include "okage/resource_textures.hpp"
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace okage {
struct ModelResourceGroup {
    std::string name;
    ModelRangeLease range;
    ResourceTextures textures;
    bool reported{}, retired{};
};
struct ActorModelRegistration {
    ModelRangeLease main;
    std::optional<ModelRangeLease> shadow;
};
class ModelGroupOwner {
public:
    explicit ModelGroupOwner(ResourceTextureOwner& textures) : textures_(textures) {}
    const ModelResourceGroup& insert(std::string name, unsigned model_count,
                                    ResourceTextures textures, bool reported = true);
    bool retire(const ModelRangeLease& range);
    const ModelResourceGroup* find(const ModelRangeLease& range) const noexcept;
    ModelResourceGroup* find(const ModelRangeLease& range) noexcept;
    void bind_instance(unsigned instance, ModelRangeLease main,
                       std::optional<ModelRangeLease> shadow = {});
    unsigned allocate_instance(ModelRangeLease main, std::optional<ModelRangeLease> shadow, unsigned flags);
    void retire_instance(unsigned instance); // 1b8448: associated then main
    void retire_associated();                // 1b84b0: instance order
    void retire_main();                      // 1b6f50: instance order, then XTA
    void reset_instances_and_registrations(); // 1010f4 -> 1b8028, no pool reset
    const ActorModelRegistration* actor(const std::string& name) const;
    void register_actor(const std::string& name, ActorModelRegistration registration);
    const auto& groups() const noexcept { return groups_; }
    const auto& ranges() const noexcept { return ranges_; }
    const auto& instances() const noexcept { return instances_; }
    const auto& registrations() const noexcept { return actors_; }
private:
    ResourceTextureOwner& textures_;
    ModelRangeAllocator ranges_;
    std::vector<std::unique_ptr<ModelResourceGroup>> groups_;
    std::array<std::optional<ActorModelRegistration>, 640> instances_;
    std::map<std::string, ActorModelRegistration> actors_;
    unsigned front_{}, back_{640}; // 337b6c/337b70, distinct from model IDs
    static std::string normalize(std::string name);
    void retire_source_id(unsigned model);
};
}
