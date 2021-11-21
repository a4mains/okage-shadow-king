#include "okage/field_raster.hpp"
#include "okage/ordinary_lighting.hpp"
#include "okage/field_shadows.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

void check(bool value,unsigned where=__builtin_LINE()) {
    if (!value) throw std::runtime_error("Field raster test failed at line "+std::to_string(where));
}

template<class F> void rejects(F function) {
    try { function(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid field raster input accepted");
}

void check_rounding_restored(const auto& function) {
    check(std::fegetround() == FE_UPWARD);
    function();
    check(std::fegetround() == FE_UPWARD);
}

void raster_storage_paths(const okage::ShadowScreenSettings& settings) {
    using P=okage::Float4;using V=okage::FieldRasterVertex;
    const std::array<std::array<P,3>,9> positions{{
        {P{0,0,10,1},P{1,0,10,1},P{0,1,10,1}}, // no clipping
        {P{100,0,10,1},P{101,1,10,1},P{100,1,10,1}}, // common reject
        {P{0,0,2,1},P{1,0,5,1},P{0,1,5,1}}, // near
        {P{0,0,3000,1},P{10,0,100,1},P{0,10,100,1}}, // far
        {P{100,0,10,1},P{0,0,10,1},P{0,1,10,1}}, // X
        {P{0,0,2,1},P{100,0,10,1},P{0,100,10,1}}, // clipped to empty
        {P{100,0,10,1},P{0,100,10,1},P{100,100,10,1}}, // XY to empty
        {P{0,0,2,1},P{100,0,10,1},P{0,1,10,1}}, // near then X
        {P{100,100,10,1},P{0,0,10,1},P{0,1,10,1}}, // X then Y
    }};
    const std::array<unsigned,9> counts{3,0,6,6,6,0,0,6,6};
    const std::array<std::array<std::uint64_t,2>,9> digests{{
        {8475731917585238904ull,12977841801580022648ull},
        {14695981039346656037ull,14695981039346656037ull},
        {1993230469601300914ull,9300896185111820834ull},
        {2573573708025604317ull,8315156688444081396ull},
        {12637808025694590848ull,17888922779904911334ull},
        {14695981039346656037ull,14695981039346656037ull},
        {14695981039346656037ull,14695981039346656037ull},
        {10102940894449499366ull,14746301037873709333ull},
        {10853666249077178761ull,17734705878456284879ull},
    }};
    const auto digest=[](const auto& vertices) {
        std::uint64_t result=14695981039346656037ull;
        for(const auto& v:vertices)for(const auto& a:{v.position,v.color,v.uv})for(float f:a) {
            result^=__builtin_bit_cast(std::uint32_t, (f));result*=1099511628211ull;
        }
        return result;
    };
    const int previous=std::fegetround();
    for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO})for(bool gs:{false,true}) {
        check(std::fesetround(mode)==0);
        std::vector<V> mixed,expected;
        for(unsigned i=0;i<positions.size();++i) {
            const std::array<V,3> input{{
                {positions[i][0],{.75f,64.5f,300.25f,96.75f},{.25f,.5f,100,1}},
                {positions[i][1],{1.75f,32.25f,128.75f,64.5f},{.75f,.5f,100,1}},
                {positions[i][2],{2.75f,128.125f,16.5f,32.25f},{.25f,1,100,1}},
            }};
            const auto actual=okage::field_raster_triangles(input,settings,gs);
            check(actual.size()==counts[i] && digest(actual)==digests[i][gs]);
            check(std::fegetround()==mode);
            mixed.insert(mixed.end(),input.begin(),input.end());
            expected.insert(expected.end(),actual.begin(),actual.end());
        }
        const auto actual=okage::field_raster_triangles(mixed,settings,gs);
        check(actual.size()==expected.size());
        check(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(V))==0);
        mixed.back().uv[3]=std::numeric_limits<float>::quiet_NaN();
        rejects([&]{okage::field_raster_triangles(mixed,settings,gs);});
        check(std::fegetround()==mode);
        check(okage::field_raster_triangles({},settings,gs).empty());
        auto invalid=settings;invalid.focal=std::numeric_limits<float>::infinity();
        rejects([&]{okage::field_raster_triangles({},invalid,gs);});
        // Count validation precedes settings and the nonfinite vertex guard.
        try {okage::field_raster_triangles(std::span(mixed).first(2),invalid,gs);check(false);}
        catch(const std::runtime_error& e){check(std::string(e.what())=="Field raster requires triangles");}
        check(std::fegetround()==mode);
    }
    check(std::fesetround(previous)==0);
}

int main() {
    try {
        const int previous_rounding = std::fegetround();
        check(previous_rounding != -1 && std::fesetround(FE_UPWARD) == 0);

        const okage::ShadowScreenSettings settings{
            800, 1, .47f, 320, 112, 101, 16777115,
            3, 2048, 3, 2048, 0, 0, 320, 112};
        const auto parameters = okage::shadow_screen_parameters(settings);
        raster_storage_paths(settings);

        const std::array<okage::Float4, 1> screen_input{{{0, 0, 10, 1}}};
        check_rounding_restored([&] {
            check(okage::field_screen_vertices(screen_input, parameters).size() == 1);
        });
        check_rounding_restored([&] {
            const std::array<okage::Float4, 1> invalid{{{
                std::numeric_limits<float>::quiet_NaN(), 0, 10, 1}}};
            rejects([&] { okage::field_screen_vertices(invalid, parameters); });
        });

        check_rounding_restored([&] {
            const auto packed = okage::field_pack_xyzf({1.f / 16.f, 15.f / 16.f,
                                                         1.0625f, -1.0625f});
            check(packed[0] == 1 && packed[1] == 15 && packed[2] == 17);
            check(packed[3] == static_cast<std::uint32_t>(-17));
        });
        check_rounding_restored([&] {
            const auto packed = okage::field_pack_xyzf({1.0e30f, -1.0e30f, 0, 0});
            check(packed[0] == 0x7fffffffu && packed[1] == 0x80000000u);
        });
        check_rounding_restored([&] {
            rejects([&] {
                okage::field_pack_xyzf({std::numeric_limits<float>::infinity(), 0, 0, 0});
            });
        });

        std::array<okage::FieldRasterVertex, 3> outside{{
            {{100, 0, 10, 1}, {32, 64, 128, 96.75f}, {0, 0, 100, 1}},
            {{101, 1, 10, 1}, {32, 64, 128, 96.75f}, {1, 0, 100, 1}},
            {{100, 1, 10, 1}, {32, 64, 128, 96.75f}, {0, 1, 100, 1}},
        }};
        check_rounding_restored([&] {
            check(okage::field_raster_triangles(outside, settings).empty());
            check(okage::field_raster_block_rejected(outside, settings));
        });

        std::vector<std::vector<okage::FieldRasterVertex>> rejected_blocks;
        for(unsigned axis=0;axis<3;++axis)for(int side:{-1,1}) {
            auto triangle=outside;
            for(auto& v:triangle) {
                v.position={0,0,10,1};
                v.position[axis]=axis==2?(side<0?2.f:3000.f):float(side*100);
            }
            rejected_blocks.emplace_back();
            for(unsigned i=0;i<8;++i)
                rejected_blocks.back().insert(rejected_blocks.back().end(),triangle.begin(),triangle.end());
        }
        auto behind=outside;
        for(auto& vertex:behind)vertex.position={0,0,-10,1};
        rejected_blocks.emplace_back(behind.begin(),behind.end());
        for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            check(std::fesetround(mode)==0);
            for(const auto& block:rejected_blocks) {
                check(okage::field_raster_block_rejected(block,settings));
                for(bool gs:{false,true})check(okage::field_raster_triangles(block,settings,gs).empty());
                check(std::fegetround()==mode);
            }
            check(!okage::field_raster_block_rejected({},settings));
        }
        check(std::fesetround(FE_UPWARD)==0);
        auto opposite=rejected_blocks[0];
        opposite.insert(opposite.end(),rejected_blocks[1].begin(),rejected_blocks[1].end());
        check(!okage::field_raster_block_rejected(opposite,settings));
        check(okage::field_raster_triangles(opposite,settings).empty());

        for(float exceptional:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
        for(unsigned attribute=0;attribute<3;++attribute)for(unsigned lane=0;lane<4;++lane) {
            auto invalid=rejected_blocks[0];
            auto& v=invalid.back();
            auto& value=attribute==0?v.position:attribute==1?v.color:v.uv;
            value[lane]=exceptional;
            check(!okage::field_raster_block_rejected(invalid,settings));
            rejects([&]{okage::field_raster_triangles(invalid,settings);});
        }
        rejects([&]{okage::field_raster_block_rejected(std::span(outside).first(2),settings);});
        auto invalid_settings=settings;invalid_settings.focal=std::numeric_limits<float>::infinity();
        rejects([&]{okage::field_raster_block_rejected(outside,invalid_settings);});
        rejects([&]{okage::field_raster_triangles(outside,invalid_settings);});
        check(std::fegetround()==FE_UPWARD);

        for(unsigned axis=0;axis<3;++axis)for(int side:{-1,1}) {
            auto straddle=outside;
            straddle[0].position={0,0,10,1};straddle[1].position={.1f,0,10,1};straddle[2].position={0,.1f,10,1};
            straddle[0].position[axis]=axis==2?(side<0?2.f:3000.f):float(side*100);
            for(bool gs:{false,true}) {
                const auto expected=okage::field_raster_triangles(straddle,settings,gs);
                check(!expected.empty());
                check(!okage::field_raster_block_rejected(straddle,settings));
                const auto actual=okage::field_raster_triangles(straddle,settings,gs);
                check(actual.size()==expected.size());
                check(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(actual[0]))==0);
            }
        }

        std::array<okage::FieldRasterVertex, 3> crossing{{
            {{0, 0, 2, 1}, {32, 64, 128, 96.75f}, {.25f, .5f, 100, 1}},
            {{1, 0, 5, 1}, {32, 64, 128, 96.75f}, {.75f, .5f, 100, 1}},
            {{0, 1, 5, 1}, {32, 64, 128, 96.75f}, {.25f, 1, 100, 1}},
        }};
        std::vector<okage::FieldRasterVertex> clipped;
        check_rounding_restored([&] {
            clipped = okage::field_raster_triangles(crossing, settings);
        });
        check(clipped.size() >= 6 && clipped.size() % 3 == 0);
        bool saw_second = false, saw_third = false;
        for (const auto& vertex : clipped) {
            for (const auto& value : {vertex.position, vertex.color, vertex.uv})
                for (float component : value) check(std::isfinite(component));
            check(vertex.uv[2] >= 0 && vertex.uv[3] == 1);
            const auto close=[](float a,float b){return std::abs(a-b)<=2*std::numeric_limits<float>::epsilon();};
            saw_second |= close(vertex.uv[0]/vertex.position[2],crossing[1].uv[0]) &&
                          close(vertex.uv[1]/vertex.position[2],crossing[1].uv[1]);
            saw_third |= close(vertex.uv[0]/vertex.position[2],crossing[2].uv[0]) &&
                         close(vertex.uv[1]/vertex.position[2],crossing[2].uv[1]);
        }
        check(saw_second && saw_third);

        check_rounding_restored([&] {
            auto invalid = crossing;
            invalid[0].color[0] = std::numeric_limits<float>::quiet_NaN();
            rejects([&] { okage::field_raster_triangles(invalid, settings); });
        });
        check_rounding_restored([&] {
            rejects([&] { okage::field_raster_triangles(
                std::span(crossing).first(2), settings); });
        });

        std::vector<okage::FieldRasterVertex> fractional, gs_units;
        check_rounding_restored([&] {
            fractional = okage::field_raster_triangles(crossing, settings, false);
        });
        check_rounding_restored([&] {
            gs_units = okage::field_raster_triangles(crossing, settings, true);
        });
        check(!fractional.empty() && fractional.size() == gs_units.size());
        check(fractional[0].color[3] == 96.75f && gs_units[0].color[3] == 96.f);

        auto lit = crossing, prematurely_packed = crossing;
        for (unsigned i=0;i<3;++i) {
            const okage::Float4 source{i?1.75f:.75f,32,64,128};
            lit[i].color=okage::ordinary_material_modulation(source,{1,1,1,1});
            prematurely_packed[i].color=okage::ordinary_material_color(source,{1,1,1,1});
        }
        const auto correct=okage::field_raster_triangles(lit,settings,true);
        const auto early=okage::field_raster_triangles(prematurely_packed,settings,true);
        check(correct.size()==early.size() && !correct.empty());
        bool differs=false;
        for(unsigned i=0;i<correct.size();++i) {
            check(correct[i].color[0]==1);
            differs|=correct[i].color[0]!=early[i].color[0];
        }
        check(differs);

        check(okage::field_ordinary_bucket(0, 512, true) == 1031); // Ghost 1031.
        check(okage::field_ordinary_bucket(0xe0, 12) == 533); // Ordinary NPC depth.
        check(okage::field_ordinary_bucket(0xc0, 12) == 533); // HERO depth.
        check(okage::field_ordinary_bucket(0x120000, 12) == 15); // Optimized sky/map depth.
        check(okage::field_ordinary_bucket(0x40080, 12) == 15); // Bit80 keeps the base at3.
        check(okage::field_ordinary_bucket(0x80000, 512) == 4); // Front terrain.
        rejects([&] { okage::field_ordinary_bucket(0, 513); });

        {
            std::vector<std::uint8_t> motion(56);
            const auto put=[&](unsigned at,std::uint32_t value) {
                for(unsigned i=0;i<4;++i)motion.at(at+i)=value>>(8*i);
            };
            put(0,0x544d58);put(4,24);put(16,0xffffffff);
            okage::ModelFile model;model.models.emplace_back();
            okage::FieldActor hero(model,motion,{});
            hero.animation().set_channel(1,{0,0,10,1});hero.evaluate();
            std::array<okage::FieldShadowActor,1> actors;
            actors[0].alias=-1;actors[0].actor=&hero;actors[0].flags=0xc0;
            actors[0].selector=4;actors[0].companion.emplace();
            const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
            okage::FieldProjectionFrame screen;screen.ordinary=settings;screen.shadow=settings;
            okage::FieldShadowSettings shadows;shadows.threshold=0;
            const auto sine=okage::make_sine_table();
            const auto frame=[&](float factor) {
                return okage::field_shadow_frame(actors,identity,screen,shadows,factor,sine);
            };
            check_rounding_restored([&] {
                const auto baseline=frame(1);
                check(hero.visible&&baseline.submissions.size()==1&&baseline.draws.empty());
                const auto& submission=baseline.submissions[0];
                check(submission.alias==-1&&submission.selector==4&&submission.bucket>shadows.threshold);
                check(submission.blocks==0&&submission.triangles==0&&submission.emitted==0);
            });
            actors[0].companion->bones.emplace_back();
            check_rounding_restored([&] { rejects([&] { frame(1); }); });
            for(unsigned selector:{0xffffu,0x8000u}) {
                hero.shadow_mode_override=selector;
                for(float factor:{1.f,std::numeric_limits<float>::quiet_NaN()})
                    check_rounding_restored([&] {
                        const auto suppressed=frame(factor);
                        check(hero.visible&&hero.shadow_mode_override==selector);
                        check(suppressed.submissions.empty()&&suppressed.draws.empty());
                    });
            }
        }

        check(std::fegetround() == FE_UPWARD);
        check(std::fesetround(previous_rounding) == 0);
        std::cout << "Field raster clipping, packing, buckets and rounding checks pass\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
