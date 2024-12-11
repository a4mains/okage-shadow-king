#include "okage/ordinary_lighting.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
struct Rounding {
    int old = std::fegetround();
    Rounding() {
        if (old < 0 || (old != FE_TOWARDZERO && std::fesetround(FE_TOWARDZERO)))
            throw std::runtime_error("Cannot set ordinary lighting rounding");
    }
    ~Rounding() { if (old != FE_TOWARDZERO) std::fesetround(old); }
};
float checked(float f) {
    if (!std::isfinite(f) || std::fpclassify(f) == FP_SUBNORMAL)
        throw std::runtime_error("Unproved ordinary lighting exceptional arithmetic");
    return f;
}
void validate(const Float4& v) { for (float f : v) checked(f); }
void validate(const Matrix4& m) { for (const auto& v : m) validate(v); }
float mul(float a, float b) { return checked(a * b); }
float add(float a, float b) { return checked(a + b); }
float inverse_length(const Float4& n, bool vu_esadd) {
    const float x = mul(n[0], n[0]), y = mul(n[1], n[1]), z = mul(n[2], n[2]);
    // Play's ESADD stack order is x²+(y²+z²). EE 0x1e22d0 uses (x²+y²)+z².
    const float length = vu_esadd ? add(x, add(y, z)) : add(add(x, y), z);
    if (!(length > 0)) throw std::runtime_error("Zero ordinary lighting normal/direction");
    const float root = checked(std::sqrt(length));
    return checked(1.f / root);
}
Float4 transform(const Matrix4& m, const Float4& n, OrdinaryNormalOperation op,
                 const Float4& initial) {
    Float4 result = n;
    const unsigned count = op == OrdinaryNormalOperation::direct ? 3 : 4;
    for (unsigned r = 0; r < count; ++r) {
        float v = mul(m[0][r], n[0]);
        if (op == OrdinaryNormalOperation::accumulate) v = add(mul(1.f, initial[r]), v);
        v = add(v, mul(m[1][r], n[1]));
        result[r] = add(v, mul(m[2][r], n[2]));
    }
    return result;
}
Matrix4 transpose(const Matrix4& m) {
    Matrix4 result{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r) result[c][r] = m[r][c];
    return result;
}
}

Float4 ordinary_transform_normal(const Matrix4& matrix, const Float4& source,
                                OrdinaryNormalOperation operation, const Float4& initial_cache) {
    if (operation != OrdinaryNormalOperation::direct && operation != OrdinaryNormalOperation::overwrite &&
        operation != OrdinaryNormalOperation::accumulate)
        throw std::runtime_error("Unknown ordinary normal operation");
    validate(matrix); validate(source); validate(initial_cache);
    Rounding rounding;
    return transform(matrix, source, operation, initial_cache);
}

std::array<float,2> ordinary_environment_uv(const Float4& normal,const Float4& region) {
    validate(normal);validate(region);Rounding rounding;
    std::array<float,2> result{};
    for(unsigned i=0;i<2;++i) {
        const float scale=mul(region[i+2],.5f);
        const float bias=add(region[i],scale);
        result[i]=add(mul(1.f,bias),mul(normal[i],scale));
    }
    return result;
}
Float4 ordinary_environment_source(float coefficient) {
    checked(coefficient);Rounding rounding;
    //0x17f480 defaults RGB to128, then clamps every component through17dfe8.
    return {128,128,128,std::clamp(mul(coefficient,128.f),0.f,255.f)};
}

OrdinaryLightingResult ordinary_light_normal(const Float4& normal_cache,
    const Matrix4& direction_matrix, const Matrix4& light_colors, OrdinaryLightingPath path) {
    if (path != OrdinaryLightingPath::direct && path != OrdinaryLightingPath::deferred)
        throw std::runtime_error("Unknown ordinary lighting path");
    validate(normal_cache); validate(direction_matrix); validate(light_colors);
    Rounding rounding;
    const bool zero = normal_cache[0]==0 && normal_cache[1]==0 && normal_cache[2]==0;
    const float q = zero ? std::numeric_limits<float>::max() : inverse_length(normal_cache, true);
    Float4 weights = transform(direction_matrix, normal_cache, OrdinaryNormalOperation::direct, {});
    // VU MAX's second operand is +0, including when the first is -0.
    for (unsigned i = 0; i < 3; ++i) weights[i] = mul(weights[i] > 0 ? weights[i] : 0.f, q);
    OrdinaryLightingResult result{};
    for (unsigned r = 0; r < 4; ++r) {
        result.normalized_normal[r] = mul(normal_cache[r], q);
        float v;
        if (path == OrdinaryLightingPath::direct) {
            v = mul(light_colors[3][r], 1.f);
            for (unsigned i = 0; i < 3; ++i) v = add(v, mul(light_colors[i][r], weights[i]));
        } else {
            v = mul(light_colors[0][r], weights[0]);
            v = add(v, mul(light_colors[1][r], weights[1]));
            v = add(v, mul(light_colors[2][r], weights[2]));
            v = add(v, mul(light_colors[3][r], 1.f));
        }
        result.lighting_cache[r] = v > 0 ? v : 0.f;
    }
    return result;
}

Float4 ordinary_material_modulation(const Float4& source_cache, const Float4& lighting_cache) {
    validate(source_cache); validate(lighting_cache);
    Rounding rounding;
    Float4 result{};
    for (unsigned i = 0; i < 4; ++i) {
        if (source_cache[i] < 0 || lighting_cache[i] < 0)
            throw std::runtime_error("Unproved negative ordinary material color");
        result[i] = std::min(mul(lighting_cache[i], source_cache[i]), 255.f);
    }
    return result;
}

Float4 ordinary_material_color(const Float4& source_cache, const Float4& lighting_cache) {
    validate(source_cache); validate(lighting_cache);
    Rounding rounding;
    Float4 result{};
    for (unsigned i = 0; i < 4; ++i) {
        if (source_cache[i] < 0 || lighting_cache[i] < 0)
            throw std::runtime_error("Unproved negative ordinary material color");
        const float biased = add(8388608.f, mul(lighting_cache[i], source_cache[i]));
        result[i] = std::min(biased, 8388863.f) - 8388608.f;
    }
    return result;
}

Matrix4 ordinary_direction_matrix(const std::array<Float4, 3>& directions, const Matrix4& view) {
    for (const auto& direction : directions) validate(direction);
    validate(view);
    Rounding rounding;
    Matrix4 light{};
    for (unsigned c = 0; c < 3; ++c) {
        Float4 negative{};
        for (unsigned r = 0; r < 4; ++r) negative[r] = mul(directions[c][r], -1.f);
        const bool zero = negative[0] == 0 && negative[1] == 0 && negative[2] == 0;
        const float q = zero ? std::numeric_limits<float>::max() : inverse_length(negative, false);
        for (unsigned r = 0; r < 3; ++r) light[c][r] = mul(negative[r], q);
    }
    light[3][3] = 1.f;
    const auto a = transpose(light), b = transpose(view);
    Matrix4 result{};
    for (unsigned c = 0; c < 4; ++c) {
        result[c] = transform(a, b[c], OrdinaryNormalOperation::overwrite, {});
        for (unsigned r = 0; r < 4; ++r) result[c][r] = add(result[c][r], mul(a[3][r], b[c][3]));
    }
    return result;
}

OrdinaryNormalCache ordinary_normal_cache(const RenderBlock& block, std::span<const Matrix4> pose) {
    const auto count = std::count_if(block.commands.begin(), block.commands.end(),
        [](const ModelCommand& command) { return command.opcode == 4; });
    OrdinaryNormalCache result{count > 1 ? OrdinaryLightingPath::deferred : OrdinaryLightingPath::direct, {}, {}};
    const auto extent = block.attribute_extents[1];
    if (extent > 32768) throw std::runtime_error("Ordinary normal cache extent out of range");
    result.normals.resize(extent);
    result.defined.resize(extent);
    std::vector<bool> seen(block.attributes.size());
    std::int32_t node = -1;
    Rounding rounding;
    for (const auto& command : block.commands) {
        if (command.opcode == 4) {
            if (command.size != 8 || command.hierarchy_node < 0 ||
                static_cast<std::size_t>(command.hierarchy_node) >= pose.size())
                throw std::runtime_error("Invalid ordinary normal hierarchy command");
            node = command.hierarchy_node;
        } else if (command.opcode == 5 || command.opcode == 6) {
            if (command.attribute < 0 || static_cast<std::size_t>(command.attribute) >= block.attributes.size())
                throw std::runtime_error("Missing ordinary normal attribute command");
            const auto index = static_cast<std::size_t>(command.attribute);
            const auto& attribute = block.attributes[index];
            if (seen[index] || attribute.kind != command.opcode || attribute.flags != 1 ||
                command.size != 12 + attribute.values.size() * 16 || node < 0 || attribute.hierarchy_node != node)
                throw std::runtime_error("Unsupported ordinary normal attribute flags/animation/order");
            seen[index] = true;
            if (attribute.kind != 6) continue;
            if (attribute.first > extent || attribute.values.size() > extent - attribute.first)
                throw std::runtime_error("Ordinary normal attribute exceeds cache extent");
            validate(pose[node]);
            for (std::size_t i = 0; i < attribute.values.size(); ++i) {
                const auto destination = attribute.first + i;
                validate(attribute.values[i]);
                const auto operation = result.path == OrdinaryLightingPath::direct ?
                    OrdinaryNormalOperation::direct : OrdinaryNormalOperation::accumulate;
                result.normals[destination] = transform(pose[node], attribute.values[i], operation, result.normals[destination]);
                result.defined[destination] = true;
            }
        }
    }
    for (std::size_t i = 0; i < block.attributes.size(); ++i)
        if ((block.attributes[i].kind == 5 || block.attributes[i].kind == 6) && !seen[i])
            throw std::runtime_error("Missing ordinary normal command ordering");
    return result;
}
}
