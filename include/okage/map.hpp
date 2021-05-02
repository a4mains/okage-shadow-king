#pragma once
#include "okage/pose.hpp"

namespace okage {
struct MapDefinition {
    std::string name;
    std::array<std::uint32_t,37> words; // 0x94-byte source record, unknowns retained.
    std::uint32_t flags() const { return words[7]; }
};
struct MapPlacement {
    std::array<std::uint32_t,20> words; // 0x50-byte source record.
    std::uint32_t definition() const { return words[2]; }
    std::uint32_t flags() const { return words[3]; }
    unsigned handler() const { return (flags()>>20)&15; }
    std::uint32_t handler_argument() const { return ((flags()>>24)&127)|(flags()&0xff0000); }
};
struct MapFile {
    std::array<std::uint32_t,16> header;
    std::vector<MapDefinition> definitions;
    std::vector<MapPlacement> placements;
};
struct MapTransform {
    Float4 position, rotation, scale;
    std::uint32_t flags;
};
// Inline XMP form loaded by retail 0x1bc0b8. LPF's split form is separate.
MapFile read_xmp(Bytes bytes);
// Recovered 0x1bbd28..0x1bbe40 conversion. Does not execute effect handlers,
// attach animation, perform model instantiation or apply these transforms.
MapTransform map_transform(const MapPlacement& placement,const MapDefinition& definition);
// 0x1b9f88 replaces flag1 instance rotation with camera pitch/yaw, zero roll.
// Position and scale remain the placement's values. Camera rotation is in turns.
Matrix4 map_instance_matrix(const MapTransform&,const Float4& camera_rotation,const SineTable&);
// Retail 0x1b8a80 allocates ordinary instances upward and optimized ones
// downward. Handler1 uses the separate effect pool and receives -1 here.
std::vector<int> map_instance_ids(const MapFile& map);
}
