#pragma once
#include "okage/actor.hpp"
#include <filesystem>
#include <memory>
namespace okage {
class MapObjects {
public:
    explicit MapObjects(const std::filesystem::path& map);
    std::span<const ActorBinding> bindings() const{return bindings_;}
    const FieldActor* placement(std::size_t index) const;
private:
    std::vector<std::unique_ptr<FieldActor>> objects_;
    std::vector<ActorBinding> bindings_;
    std::vector<FieldActor*> placements_;
};
}
