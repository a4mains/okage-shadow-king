#pragma once
#include "okage/actor.hpp"
#include "okage/collision.hpp"
#include "okage/map.hpp"
#include <filesystem>
#include <memory>
namespace okage {
struct MapCollisionBinding {std::size_t placement;int instance;CollisionBody body;};
// 0x1bba88 definition conversion, 0x1b86e8 shapes, 0x1b8a80 registry order.
std::vector<MapCollisionBinding> map_collision_bodies(const MapFile&);
class MapObjects {
public:
    explicit MapObjects(const std::filesystem::path& map);
    std::span<const ActorBinding> bindings() const{return bindings_;}
    const FieldActor* placement(std::size_t index) const;
    bool visible(std::size_t placement) const;
    bool set_visible(int instance,bool visible);
    bool select_motion(int instance,unsigned entry);
    int next_front_instance() const{return front_;}
    int first_back_instance() const{return back_;}
    const std::vector<MapCollisionBinding>& collision_bodies() const {return collisions_;}
    std::optional<unsigned> collision_index(int instance) const;
    Float4 position(std::size_t placement) const;
    void write_position(std::size_t placement,const Float4&);
    MapTransform transform(std::size_t placement) const;
    Animation* channel_group(int instance) const;
    void bind_channels(const std::shared_ptr<AnimationChannelPool>&);
    void tick_static_channels();
private:
    struct State {int instance=-1;bool visible=true;MapTransform transform{};bool advance=false;};
    std::vector<State> states_;
    std::vector<MapCollisionBinding> collisions_;
    std::vector<std::unique_ptr<FieldActor>> objects_;
    std::vector<ActorBinding> bindings_;
    std::vector<FieldActor*> placements_;
    // Unanimated instances still own four real world/color channels (1b64d8).
    std::vector<std::unique_ptr<Animation>> static_channels_;
    int front_=0,back_=640;
};
}
