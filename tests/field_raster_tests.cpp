#include "okage/field_raster.hpp"
#include "okage/field_shadows.hpp"
#include <array>
#include <cfenv>
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

int main() {
    try {
        const int previous_rounding = std::fegetround();
        check(previous_rounding != -1 && std::fesetround(FE_UPWARD) == 0);

        const okage::ShadowScreenSettings settings{
            800, 1, .47f, 320, 112, 101, 16777115,
            3, 2048, 3, 2048, 0, 0, 320, 112};
        const auto parameters = okage::shadow_screen_parameters(settings);

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
        });

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

        check(okage::field_ordinary_bucket(0, 512, true) == 1031); // Ghost 1031.
        check(okage::field_ordinary_bucket(0xe0, 12) == 533); // Ordinary NPC depth.
        check(okage::field_ordinary_bucket(0xc0, 12) == 533); // HERO depth.
        check(okage::field_ordinary_bucket(0x120000, 12) == 15); // Optimized sky/map depth.
        check(okage::field_ordinary_bucket(0x40080, 12) == 15); // Bit80 keeps the base at3.
        check(okage::field_ordinary_bucket(0x80000, 512) == 4); // Front terrain.
        rejects([&] { okage::field_ordinary_bucket(0, 513); });

        check(std::fegetround() == FE_UPWARD);
        check(std::fesetround(previous_rounding) == 0);
        std::cout << "Field raster clipping, packing, buckets and rounding checks pass\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
