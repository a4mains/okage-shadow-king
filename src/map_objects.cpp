#include "okage/map_objects.hpp"
#include "okage/map.hpp"
#include <fstream>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    if(size>128*1024*1024)throw std::runtime_error("Map object asset size limit");
    std::vector<std::uint8_t> bytes(size);std::ifstream input(path,std::ios::binary);
    if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))
        throw std::runtime_error("Cannot read map object asset");
    return bytes;
}
}
std::vector<MapCollisionBinding> map_collision_bodies(const MapFile& map) {
    struct Rounding {
        int old=std::fegetround();
        Rounding(){if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Map collision rounding");}
        ~Rounding(){std::fesetround(old);}
    } rounding;
    std::vector<std::optional<CollisionBody>> definitions(map.definitions.size());
    for(std::size_t i=0;i<definitions.size();++i) {
        const auto& d=map.definitions[i];const auto type=d.words[32];
        if(!type)continue;
        if(type>2||i>=map.placements.size())throw std::runtime_error("Map model collision construction is unrecovered");
        const auto& p=map.placements[i];
        auto value=[](std::uint32_t word){const float f=__builtin_bit_cast(float, (word));
            if(!std::isfinite(f))throw std::runtime_error("Nonfinite map collision geometry");return f;};
        auto f=[&](unsigned n){return value(d.words[n]);};
        const float sx=value(p.words[12]),sy=value(p.words[13]),sz=value(p.words[14]);
        const float radius=(f(34)*50.f)*sx,priority=f(36);
        if(radius<0||priority<0||priority>=4294967296.f)throw std::runtime_error("Map collision radius/priority range");
        CollisionBody body;body.priority=std::uint32_t(priority);body.broad_radius=radius+radius;
        CollisionShape floor{8,{{0,f(35)*sy,0,1},{},0}};
        CollisionShape shape;shape.flags=0x70|type;shape.geometry.radius=radius;
        shape.geometry.first={(f(24)*20.f)*sx,((-f(29))*20.f)*sy,(f(26)*20.f)*sz,1};
        if(type==2)shape.geometry.second={(f(28)*20.f)*sx,((-f(25))*20.f)*sy,(f(30)*20.f)*sz,1};
        body.shapes={floor,shape};definitions[i]=std::move(body);
    }
    const auto ids=map_instance_ids(map);std::vector<MapCollisionBinding> result;
    for(std::size_t i=0;i<map.placements.size();++i) {
        const auto& p=map.placements[i];if(p.handler()||!definitions.at(p.definition()))continue;
        auto body=*definitions[p.definition()];body.id=result.size();
        const auto transform=map_transform(p,map.definitions[p.definition()]);
        body.previous=body.position=transform.position;
        if(body.id&&(transform.flags&0x10000))body.flags|=2;
        result.push_back({i,ids[i],std::move(body)});
    }
    return result;
}
MapObjects::MapObjects(const std::filesystem::path& path) {
    const auto map=read_xmp(read(path));const auto ids=map_instance_ids(map);
    collisions_=map_collision_bodies(map);
    placements_.resize(map.placements.size());
    states_.resize(map.placements.size());
    for(std::size_t i=0;i<map.placements.size();++i) {
        const auto& p=map.placements[i];const auto& d=map.definitions.at(p.definition());
        const auto transform=map_transform(p,d);
        states_[i]={ids[i],(transform.flags&0x10000)==0,transform.position};
        if(p.handler()||!(d.flags()&8))continue;
        auto model=path.parent_path()/d.name,motion=model,xta=model;
        motion.replace_extension(".xmt");xta.replace_extension(".xta");
        TextureAnimation textures;
        // 0x1b6a18 attempts the XTA sidecar; missing files leave no replacement
        // table. A present malformed file is not silently discarded.
        if(std::filesystem::exists(xta)) {
            const auto bytes=read(xta);
            textures=read_xta({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
        }
        auto object=std::make_unique<FieldActor>(read_xmd(read(model)),read(motion),std::move(textures));
        object->animation().set_channel(0,transform.rotation);
        object->animation().set_channel(1,transform.position);
        object->animation().set_channel(2,transform.scale);
        object->visible=(transform.flags&0x10000)==0;object->evaluate();
        placements_[i]=object.get();bindings_.push_back({ids[i],object.get()});objects_.push_back(std::move(object));
    }
}
const FieldActor* MapObjects::placement(std::size_t index) const {return placements_.at(index);}
bool MapObjects::visible(std::size_t index) const {
    const auto* object=placements_.at(index);
    return object?object->visible:states_.at(index).visible;
}
bool MapObjects::set_visible(int instance,bool value) {
    if(instance<0)return false;
    for(std::size_t i=0;i<states_.size();++i)if(states_[i].instance==instance) {
        states_[i].visible=value;
        if(auto* object=placements_[i])object->visible=value;
        return true;
    }
    return false;
}
std::optional<unsigned> MapObjects::collision_index(int instance) const {
    if(instance<0)return {};
    for(unsigned i=0;i<collisions_.size();++i)if(collisions_[i].instance==instance)return i;
    for(const auto& state:states_)if(state.instance==instance)return 0;
    return {};
}
Float4 MapObjects::position(std::size_t index) const {
    auto* object=placements_.at(index);
    return object?object->animation().values().at(1):states_.at(index).position;
}
void MapObjects::write_position(std::size_t index,const Float4& position) {
    if(auto* object=placements_.at(index))object->animation().write_channel_output(1,position);
    states_.at(index).position=position;
}
}
