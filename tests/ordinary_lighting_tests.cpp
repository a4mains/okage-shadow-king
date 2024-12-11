#include "okage/ordinary_lighting.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

using namespace okage;
namespace {
void require(bool value) { if (!value) throw std::runtime_error("Ordinary lighting test failed"); }
template<class F> void rejects(F f) {
    bool threw = false;
    try { f(); } catch (const std::runtime_error&) { threw = true; }
    require(threw);
}
bool same(const Float4& a, const Float4& b) {
    return std::bit_cast<std::array<std::uint32_t, 4>>(a) ==
           std::bit_cast<std::array<std::uint32_t, 4>>(b);
}
constexpr Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
constexpr std::array<Float4, 3> directions{{{-1,0,0,17},{0,-2,0,19},{0,0,-4,23}}};

void arithmetic_contract() {
    Matrix4 matrix = identity;
    matrix[0][3] = 4; matrix[1][3] = 5; matrix[2][3] = 6;
    matrix[3] = {100,200,300,400};
    const Float4 normal{1,2,3,17};
    require(ordinary_transform_normal(matrix, normal, OrdinaryNormalOperation::direct) == normal);
    require(ordinary_transform_normal(matrix, normal, OrdinaryNormalOperation::overwrite) == Float4{1,2,3,32});
    require(ordinary_transform_normal(matrix, normal, OrdinaryNormalOperation::accumulate, {4,5,6,7}) == Float4{5,7,9,39});
    // A pre-summed contribution would incorrectly return 2 in lane X.
    Matrix4 cancellation{{{16777216,0,0,0},{-16777216,0,0,0},{1,0,0,0},{}}};
    require(ordinary_transform_normal(cancellation, {1,1,1,0}, OrdinaryNormalOperation::accumulate,
                                      {1,0,0,0})[0] == 1);
    Matrix4 direction{{{1,1,1,0},{},{},{}}};
    Matrix4 color{{{16777216,0,300,-2},{-16777216,0,0,0},{1,0,0,0},{1,0,0,0}}};
    const auto direct = ordinary_light_normal({1,0,0,6}, direction, color, OrdinaryLightingPath::direct);
    const auto deferred = ordinary_light_normal({1,0,0,6}, direction, color, OrdinaryLightingPath::deferred);
    require(direct.lighting_cache == Float4{1,0,300,0});
    require(deferred.lighting_cache == Float4{2,0,300,0});
    require(direct.normalized_normal == Float4{1,0,0,6});
    const auto scaled = ordinary_light_normal({0,0,2,6}, identity, identity, OrdinaryLightingPath::deferred);
    require(scaled.normalized_normal == Float4{0,0,1,3});
    const auto different_w = ordinary_light_normal({0,0,2,-9}, identity, identity, OrdinaryLightingPath::deferred);
    require(same(scaled.lighting_cache, different_w.lighting_cache));
    Matrix4 negative_zero{};
    for (auto& c : negative_zero) c.fill(-0.f);
    require(same(ordinary_light_normal({1,0,0,0}, identity, negative_zero,
        OrdinaryLightingPath::deferred).lighting_cache, Float4{}));
    require(ordinary_material_modulation({1,3.5f,255,256}, {0,1,2,1}) == Float4{0,3.5f,255,255});
    require(ordinary_material_color({1,3.5f,255,256}, {0,1,2,1}) == Float4{0,3,255,255});
    require(ordinary_material_color({0x1.fffffep-1f,1,1,1}, {1,1,1,1}) == Float4{0,1,1,1});

    Matrix4 view = identity;
    view[3] = {7,8,9,1};
    const auto dm = ordinary_direction_matrix(directions, view);
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r) require(dm[c][r] == view[r][c]);
}

RenderBlock block() {
    RenderBlock b{};
    b.attributes.push_back({6,1,1,0,{{0,0,2,7}}});
    b.commands = {{0,8,4,0,-1},{8,28,6,-1,0}};
    b.attribute_extents[1] = 3;
    return b;
}
void command_contract() {
    const std::array<Matrix4, 1> pose{identity};
    auto b = block();
    auto cache = ordinary_normal_cache(b, pose);
    require(cache.path == OrdinaryLightingPath::direct);
    require(cache.defined == std::vector<bool>{false,true,false});
    require(cache.normals[1] == Float4{0,0,2,7});
    // Even a repeated node with no following attribute changes the block path.
    b.commands.push_back({36,8,4,0,-1});
    cache = ordinary_normal_cache(b, pose);
    require(cache.path == OrdinaryLightingPath::deferred);
    require(cache.normals[1] == Float4{0,0,2,0});
    b.attributes.push_back({6,1,1,0,{{2,0,0,11}}});
    b.commands.push_back({44,28,6,-1,1});
    cache = ordinary_normal_cache(b, pose);
    require(cache.normals[1] == Float4{2,0,2,0}); // No per-contribution normalization.
    b.attributes[0].flags = 0;
    rejects([&] { ordinary_normal_cache(b, pose); });
    b.attributes[0].flags = 1;
    b.commands[1].size += 16;
    rejects([&] { ordinary_normal_cache(b, pose); });
    b = block(); b.commands.clear();
    rejects([&] { ordinary_normal_cache(b, pose); });
    b = block(); b.attribute_extents[1] = 1;
    rejects([&] { ordinary_normal_cache(b, pose); });
    b = block(); b.attributes[0].hierarchy_node = 1;
    rejects([&] { ordinary_normal_cache(b, pose); });
    b = block(); b.commands.push_back(b.commands.back());
    rejects([&] { ordinary_normal_cache(b, pose); });
}

void rounding_and_failures() {
    const int original = std::fegetround();
    require(std::fesetround(FE_TONEAREST) == 0);
    const Float4 n{.123f, .234f, .345f, .456f};
    const auto expected_direction = ordinary_direction_matrix({n,n,n}, identity);
    const auto expected = ordinary_light_normal(n, expected_direction, identity, OrdinaryLightingPath::direct);
    const auto expected_transform = ordinary_transform_normal(expected_direction, n, OrdinaryNormalOperation::accumulate, n);
    const auto expected_color = ordinary_material_color({123.1f,43.2f,65.3f,76.4f}, expected.lighting_cache);
    for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        require(std::fesetround(mode) == 0);
        const auto direction = ordinary_direction_matrix({n,n,n}, identity);
        for (unsigned c = 0; c < 4; ++c) require(same(direction[c], expected_direction[c]));
        const auto actual = ordinary_light_normal(n, direction, identity, OrdinaryLightingPath::direct);
        require(same(actual.normalized_normal, expected.normalized_normal));
        require(same(actual.lighting_cache, expected.lighting_cache));
        require(same(ordinary_transform_normal(direction, n, OrdinaryNormalOperation::accumulate, n), expected_transform));
        require(same(ordinary_material_color({123.1f,43.2f,65.3f,76.4f}, actual.lighting_cache), expected_color));
        require(std::fegetround() == mode);
        require(ordinary_environment_uv({0,0,1,0},{.5f/1024,.5f/1024,64.5f/1024,64.5f/1024})==
            std::array<float,2>{32.75f/1024,32.75f/1024});
        require(ordinary_environment_uv({-1,1,0,0},{.5f/1024,.5f/1024,64.5f/1024,64.5f/1024})==
            std::array<float,2>{.5f/1024,65.f/1024});
        require(ordinary_environment_source(.25f)==Float4{128,128,128,32});
        require(ordinary_environment_source(-1)==Float4{128,128,128,0});
        require(ordinary_environment_source(2)==Float4{128,128,128,255});
        require(std::fegetround()==mode);
        Matrix4 zero_colors=identity;zero_colors[3]={.5f,.25f,.75f,1};
        for(const auto path:{OrdinaryLightingPath::direct,OrdinaryLightingPath::deferred}) {
            const auto zero=ordinary_light_normal({-0.f,0,-0.f,1},identity,zero_colors,path);
            require(same(zero.normalized_normal,{-0.f,0,-0.f,std::numeric_limits<float>::max()}));
            require(same(zero.lighting_cache,zero_colors[3]));
            require(std::fegetround()==mode);
        }
        require(std::fegetround() == mode);
        Matrix4 zero_directions{};zero_directions[3][3]=1;
        for(unsigned mask=0;mask<8;++mask) {
            std::array<Float4,3> zero{};
            for(auto& direction:zero)for(unsigned c=0;c<3;++c)direction[c]=(mask&(1u<<c))?-0.f:0.f;
            const auto actual=ordinary_direction_matrix(zero,identity);
            for(unsigned c=0;c<4;++c)require(same(actual[c],zero_directions[c]));
            require(std::fegetround()==mode);
        }
        rejects([&] { ordinary_direction_matrix({Float4{0x1p-100f,0,0,0},Float4{0,1,0,0},Float4{0,0,1,0}}, identity); });
        require(std::fegetround() == mode);
        rejects([&] { ordinary_material_color({-1,0,0,0}, {}); });
        require(std::fegetround() == mode);
        for (float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::denorm_min()}) {
            rejects([&] { ordinary_transform_normal(identity, {bad,1,1,1}, OrdinaryNormalOperation::direct); });
            require(std::fegetround() == mode);
        }
        // Nonzero operands whose squared length underflows to zero are guarded.
        rejects([&] { ordinary_light_normal({0x1p-100f,0,0,0}, identity, identity, OrdinaryLightingPath::direct); });
        require(std::fegetround() == mode);
    }
    require(std::fesetround(original) == 0);
}
}
int main() {
    try {
        arithmetic_contract(); command_contract(); rounding_and_failures();
        std::cout << "Ordinary lighting tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
