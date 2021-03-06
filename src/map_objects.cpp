#include "okage/map_objects.hpp"
#include "okage/map.hpp"
#include <fstream>
#include <stdexcept>
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
MapObjects::MapObjects(const std::filesystem::path& path) {
    const auto map=read_xmp(read(path));const auto ids=map_instance_ids(map);
    placements_.resize(map.placements.size());
    for(std::size_t i=0;i<map.placements.size();++i) {
        const auto& p=map.placements[i];const auto& d=map.definitions.at(p.definition());
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
        const auto transform=map_transform(p,d);
        object->animation().set_channel(0,transform.rotation);
        object->animation().set_channel(1,transform.position);
        object->animation().set_channel(2,transform.scale);
        object->visible=(transform.flags&0x10000)==0;object->evaluate();
        placements_[i]=object.get();bindings_.push_back({ids[i],object.get()});objects_.push_back(std::move(object));
    }
}
const FieldActor* MapObjects::placement(std::size_t index) const {return placements_.at(index);}
}
