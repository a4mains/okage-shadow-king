#pragma once
#include "okage/assets.hpp"
#include <array>

namespace okage {
using Float4 = std::array<float, 4>;
struct Bone {
    Float4 scale, rotation, translation; // Source radians; padding retained.
};
struct HierarchyNode { std::uint16_t bone; std::int16_t parent; };
struct TextureState {
    std::int16_t texture, palette, u_mode, v_mode;
    Float4 region;
};
struct Material {
    // Preserve all source words until lighting/blending semantics are validated.
    std::array<std::uint32_t, 8> words;
};
struct AttributeRange {
    std::uint8_t kind;
    std::uint16_t flags, first;
    std::int32_t hierarchy_node;
    std::vector<Float4> values; // UV occupies the first two components.
};
struct Polygon {
    std::uint32_t flags;
    std::int32_t texture_state, material;
    std::vector<std::array<std::int16_t, 4>> corners; // Three or four, in source order.
};
struct ModelCommand { std::size_t offset, size; std::uint8_t opcode; };
struct RenderBlock {
    std::size_t offset, end, draw_offset;
    Float4 bounds;
    std::vector<TextureState> textures;
    std::vector<Material> materials;
    std::vector<AttributeRange> attributes;
    std::vector<Polygon> polygons;
    std::vector<ModelCommand> commands;
    std::array<std::uint32_t, 4> attribute_extents{};
    std::size_t texture_command = 0, material_command = 0;
};
struct Model {
    std::size_t offset, end;
    std::vector<Bone> bones;
    std::vector<HierarchyNode> hierarchy;
    std::vector<std::string> names;
    std::vector<RenderBlock> blocks;
};
struct ModelFile {
    std::vector<std::string> textures;
    std::vector<Model> models;
};
ModelFile read_xmd(Bytes bytes);
}
