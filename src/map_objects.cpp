#include "okage/input_log.hpp"
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
    record_input_read(path,bytes);return bytes;
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
        auto value=[](std::uint32_t word){const float f=std::bit_cast<float>(word);
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
MapObjects::MapObjects(const std::filesystem::path& path, std::span<const std::shared_ptr<const SceneModelAsset>> assets) {
    const auto map=read_xmp(read(path));const auto ids=map_instance_ids(map);
    if(!assets.empty() && assets.size()!=map.definitions.size())throw std::runtime_error("Owned map definition count");
    collisions_=map_collision_bodies(map);
    placements_.resize(map.placements.size());
    static_channels_.resize(map.placements.size());
    states_.resize(map.placements.size());
    for(std::size_t i=0;i<map.placements.size();++i) {
        const auto& p=map.placements[i];const auto& d=map.definitions.at(p.definition());
        const auto transform=map_transform(p,d);
        if(ids[i]>=0) {
            if(transform.flags&0x100000)back_=std::min(back_,ids[i]);
            else front_=std::max(front_,ids[i]+1);
        }
        states_[i]={ids[i],(transform.flags&0x10000)==0,transform};
        if(p.handler())continue;
        if(!(d.flags()&8)) {
            auto channels=std::make_unique<Animation>(4);
            channels->set_channel(0,transform.rotation);
            channels->set_channel(1,transform.position);
            channels->set_channel(2,transform.scale);
            //1bba88 evaluates placement rotation and scale at construction.
            channels->value(0);channels->value(2);
            static_channels_[i]=std::move(channels);
            continue;
        }
        auto model=path.parent_path()/d.name,motion=model,xta=model;
        motion.replace_extension(".xmt");xta.replace_extension(".xta");
        const auto asset=assets.empty()?nullptr:assets[p.definition()];
        if(!assets.empty() && (!asset || !asset->motion))throw std::runtime_error("Owned animated map asset unavailable");
        TextureAnimation textures;
        // 0x1b6a18 attempts the XTA sidecar; missing files leave no replacement
        // table. A present malformed file is not silently discarded.
        if(asset)textures=asset->animation;
        else if(std::filesystem::exists(xta)) {
            const auto bytes=read(xta);
            textures=read_xta({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
        }
        auto object=std::make_unique<FieldActor>(asset?asset->model:read_xmd(read(model)),
            asset?Bytes(*asset->motion):Bytes(read(motion)),std::move(textures),false,false,
            asset?asset->packet_textures:std::shared_ptr<TextureBindings>{});
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
bool MapObjects::select_motion(int instance,unsigned entry) {
    if(instance<0)return false;
    for(std::size_t i=0;i<states_.size();++i)if(states_[i].instance==instance) {
        if(auto* object=placements_[i])object->select_motion(entry);
        //1b6fb8 with context=-1 enables channel advancement without a script.
        else states_[i].advance=true;
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
    if(object)return object->animation().value(1);
    const auto& channels=static_channels_.at(index);
    return channels?channels->value(1):states_.at(index).transform.position;
}
void MapObjects::write_position(std::size_t index,const Float4& position) {
    if(auto* object=placements_.at(index))object->animation().write_channel_output(1,position);
    else if(auto& channels=static_channels_.at(index))channels->write_channel_output(1,position);
    states_.at(index).transform.position=position;
}
MapTransform MapObjects::transform(std::size_t index) const {
    auto result=states_.at(index).transform;
    auto* channels=placements_.at(index)?&placements_[index]->animation():static_channels_.at(index).get();
    if(channels) {
        result.rotation=channels->value(0);result.position=channels->value(1);result.scale=channels->value(2);
    }
    return result;
}
Animation* MapObjects::channel_group(int instance) const {
    if(instance<0)return nullptr;
    for(std::size_t i=0;i<states_.size();++i)if(states_[i].instance==instance)
        return placements_[i]?&placements_[i]->animation():static_channels_[i].get();
    return nullptr;
}
void MapObjects::bind_channels(const std::shared_ptr<AnimationChannelPool>& pool) {
    for(std::size_t i=0;i<states_.size();++i) {
        auto* channels=placements_[i]?&placements_[i]->animation():static_channels_[i].get();
        if(channels)channels->bind_channels(pool);
    }
}
void MapObjects::tick_static_channels() {
    for(std::size_t i=0;i<states_.size();++i)
        if(static_channels_[i]&&states_[i].visible&&states_[i].advance)static_channels_[i]->tick_channels();
}
}
