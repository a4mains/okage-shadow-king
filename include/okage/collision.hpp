#pragma once
#include "okage/model.hpp"

namespace okage {
struct CollisionPolygon {
    std::array<std::uint16_t,10> words{};
    unsigned vertex_count() const {return 3+(words[0]&1);}
};
struct CollisionChunk {
    std::uint32_t source_offset=0;
    std::vector<Float4> vertices;
    std::vector<CollisionPolygon> polygons;
};
struct SurfaceContact {
    bool hit=false;
    std::uint16_t material=0;
    std::uint32_t chunk=0,polygon=0,candidate=0;
    Float4 correction{},normal{};
};
struct CollisionCapsule {Float4 first{},second{};float radius=0;};
struct CollisionShape {std::uint32_t flags=0;CollisionCapsule geometry;};
struct MapCollisionSettings {
    float floor_reach=4,step=0.5f;
    std::int32_t normal_threshold=8190;
    std::uint32_t disabled_mask=0,wall_iterations=2;
};
struct MapContact {
    bool hit=false;
    std::uint32_t flags=0;
    std::array<std::uint16_t,3> materials{}; // Floor, ceiling, wall: first nonzero ID.
    Float4 correction{};
    std::array<Float4,3> normals{};
};
struct CollisionMap {
    std::array<std::int32_t,15> header{};
    std::vector<std::uint8_t> payload;
    std::vector<std::uint32_t> pointer_locations;
    std::vector<CollisionChunk> chunks;
    std::vector<std::int32_t> cells;
    // Original 0x189258 order; mode 1 reverses Y, modes 0/2 ascend.
    // Returns chunk indices, preserving repeated cells and original ordering.
    std::vector<std::uint32_t> query(const Float4& first,const Float4& second,unsigned mode=0) const;
    SurfaceContact vertical_contact(const Float4& previous,const Float4& desired,
                                    bool ceiling,float floor_reach,
                                    std::int32_t normal_threshold,std::uint32_t disabled_mask=0) const;
    SurfaceContact capsule_wall_contact(const Float4& previous,const Float4& desired,
                                        const CollisionCapsule& capsule,
                                        std::int32_t normal_threshold,std::uint32_t disabled_mask=0) const;
    // Original 0x187270/0x187498 with fresh records: subdivide, then apply walls,
    // ceilings and floors. Does not include dynamic actors or 0x186fa0's retries.
    MapContact move_contact(const Float4& previous,const Float4& desired,
                            std::span<const CollisionShape> shapes,const MapCollisionSettings& settings) const;
};
CollisionMap read_xcl(Bytes bytes);
}
