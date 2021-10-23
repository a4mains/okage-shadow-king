#include "okage/render.hpp"
#include "okage/field_raster.hpp"
#include <cfenv>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

void require(bool value) { if (!value) throw std::runtime_error("Render test failed"); }
template<class F> void rejects(F f) {
    bool threw=false;
    try {f();} catch (const std::runtime_error&) {threw=true;}
    require(threw);
}
void rounding_batches() {
    const int previous=std::fegetround();
    const std::array modes{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    const std::array<std::array<unsigned,2>,4> dimensions{{{1,1},{64,32},{320,240},{4095,4095}}};
    const std::array<std::array<float,4>,5> regions{{{0,0,1,1},{.125f,.25f,.75f,1},
        {8,4,24,12},{0,0,0x1.000002p0f,1},{-.5f,-.5f,1,1}}};
    const std::array<std::array<std::int16_t,2>,7> axis_modes{{{1,1},{0,0},{-1,-7},
        {2,3},{1,4},{4,1},{-32768,32767}}};
    const std::array<std::array<float,2>,8> source{{{0,0},{1,1},{-1,2},{.1f,.3f},
        {0x1.fffffep-1f,0x1.000002p0f},{-0.f,0.f},
        {__builtin_bit_cast(float, (1u)),__builtin_bit_cast(float, (0x80000001u))},
        {1e20f,-1e20f}}};
    for(const auto& dims:dimensions)for(const auto& region:regions)for(const auto& axis:axis_modes) {
        const okage::TextureState state{0,0,axis[0],axis[1],region};
        require(std::fesetround(FE_TONEAREST)==0);
        auto expected=source;
        for(auto& value:expected)value=okage::texture_coordinates(state,dims[0],dims[1],value);
        for(int mode:modes) {
            require(std::fesetround(mode)==0);
            // Includes the actual triangle/quad batch sizes and an empty batch.
            for(std::size_t count:{0u,1u,3u,4u,8u}) {
                auto batch=source;
                okage::texture_coordinates_batch(state,dims[0],dims[1],std::span(batch).first(count));
                require(std::fegetround()==mode);
                for(std::size_t i=0;i<count;++i) {
                    const auto scalar=okage::texture_coordinates(state,dims[0],dims[1],source[i]);
                    require(std::fegetround()==mode);
                    for(unsigned c=0;c<2;++c) {
                        const auto bits=__builtin_bit_cast(std::uint32_t, (expected[i][c]));
                        require(__builtin_bit_cast(std::uint32_t, (batch[i][c]))==bits);
                        require(__builtin_bit_cast(std::uint32_t, (scalar[c]))==bits);
                    }
                }
                for(std::size_t i=count;i<source.size();++i)
                    for(unsigned c=0;c<2;++c)
                        require(__builtin_bit_cast(std::uint32_t, (batch[i][c]))==__builtin_bit_cast(std::uint32_t, (source[i][c])));
            }
        }
    }
    const okage::ShadowScreenSettings settings{800,1,.47f,320,112,101,16777115,3,2048,3,2048,0,0,320,112};
    require(std::fesetround(FE_TONEAREST)==0);
    const auto parameters=okage::shadow_screen_parameters(settings);
    const std::array<okage::FieldRasterVertex,3> triangle{{
        {{0,0,10,1},{32,64,128,96.75f},{.25f,.5f,100,1}},
        {{1,0,10,1},{32,64,128,96.75f},{.75f,.5f,100,1}},
        {{0,1,10,1},{32,64,128,96.75f},{.25f,1,100,1}}}};
    const auto raster=okage::field_raster_triangles(triangle,settings);
    require(raster.size()==3);
    for(int mode:modes) {
        require(std::fesetround(mode)==0);
        const auto actual_parameters=okage::shadow_screen_parameters(settings);
        require(std::fegetround()==mode);
        for(unsigned i=0;i<parameters.size();++i)for(unsigned c=0;c<4;++c)
            require(__builtin_bit_cast(std::uint32_t, (parameters[i][c]))==__builtin_bit_cast(std::uint32_t, (actual_parameters[i][c])));
        const auto actual_raster=okage::field_raster_triangles(triangle,settings);
        require(std::fegetround()==mode && actual_raster.size()==raster.size());
        for(unsigned i=0;i<raster.size();++i)for(unsigned c=0;c<4;++c) {
            require(__builtin_bit_cast(std::uint32_t, (actual_raster[i].position[c]))==__builtin_bit_cast(std::uint32_t, (raster[i].position[c])));
            require(__builtin_bit_cast(std::uint32_t, (actual_raster[i].uv[c]))==__builtin_bit_cast(std::uint32_t, (raster[i].uv[c])));
            require(__builtin_bit_cast(std::uint32_t, (actual_raster[i].color[c]))==__builtin_bit_cast(std::uint32_t, (raster[i].color[c])));
        }
        require(okage::field_pack_xyzf({1.f/16.f,15.f/16.f,1.0625f,-1.0625f})==
            std::array<std::uint32_t,4>{1,15,17,std::uint32_t(-17)});
        require(std::fegetround()==mode);
        rejects([]{okage::field_pack_xyzf({0,0,std::numeric_limits<float>::infinity(),0});});
        require(std::fegetround()==mode);
        auto invalid=source;
        invalid.back()[0]=std::numeric_limits<float>::quiet_NaN();
        const okage::TextureState state{0,0,1,1,{0,0,1,1}};
        rejects([&]{okage::texture_coordinates_batch(state,64,32,invalid);});
        require(std::fegetround()==mode);
        rejects([&]{okage::texture_coordinates_batch(state,0,32,invalid);});
        require(std::fegetround()==mode);
    }
    require(std::fesetround(previous)==0);
}
int main() {
    try {
        rounding_batches();
        // Half-texel placement and the distinction between normalized/pixel regions.
        okage::TextureState state{0,0,1,1,{0,0,1,1}};
        auto uv=okage::texture_coordinates(state,64,32,{0,1});
        require(uv[0]==0.5f/1024 && uv[1]==32.5f/1024);
        require(okage::texture_clamp(state,64,32)==(10ULL|(63ULL<<14)|(31ULL<<34)));
        state.u_mode=2;
        require(okage::texture_clamp(state,64,32)==(11ULL|(63ULL<<4)|(31ULL<<34)));
        state.u_mode=1;
        state.region={8,4,24,12};
        uv=okage::texture_coordinates(state,64,32,{0,1});
        require(uv[0]==8.5f/1024 && uv[1]==12.5f/1024);
        require(okage::texture_clamp(state,64,32)==(10ULL|(8ULL<<4)|(23ULL<<14)|(4ULL<<24)|(11ULL<<34)));
        const auto before=std::fegetround();
        std::fesetround(FE_UPWARD);
        okage::texture_coordinates(state,64,32,{0.1f,0.3f});
        require(std::fegetround()==FE_UPWARD);
        rejects([&]{okage::texture_coordinates(state,0,32,{0,0});});
        rejects([&]{okage::texture_coordinates(state,64,32,{std::numeric_limits<float>::infinity(),0});});
        state.region[2]=std::numeric_limits<float>::quiet_NaN();
        rejects([&]{okage::texture_coordinates(state,64,32,{0,0});});
        const auto sky=okage::sky_follow_position({8,-10,20,2},{.75f,.2f,.75f,0});
        require(sky==okage::Float4({6,-2,15,0})&&std::fegetround()==FE_UPWARD);
        rejects([]{okage::sky_follow_position({std::numeric_limits<float>::infinity(),0,0,1},{.5f,0,.5f,0});});
        require(std::fegetround()==FE_UPWARD); std::fesetround(before);
        require(okage::triangle_corners(3)==std::vector<std::array<unsigned,3>>{{0,1,2}});
        require(okage::triangle_corners(4)==std::vector<std::array<unsigned,3>>{{0,1,2},{0,2,3}});
        require(okage::material_instance_alpha(77,.5f)==38);
        require(okage::material_instance_alpha(76,.6f)==45);
        require(okage::material_instance_alpha(128,2)==255);
        require(okage::material_instance_alpha(77,-1)==0);
        rejects([]{okage::material_instance_alpha(256,1);});
        rejects([]{okage::material_instance_alpha(77,std::numeric_limits<float>::quiet_NaN());});
        std::fesetround(FE_UPWARD);
        require(okage::material_instance_alpha(77,.5f)==38 && std::fegetround()==FE_UPWARD);
        std::fesetround(before);
        okage::Material material{};
        material.words[5]=__builtin_bit_cast(unsigned, (.4f));
        require(okage::material_source_color(material,{1,.5f,0,1})==okage::Float4({255,127.5f,0,77}));
        material.words[5]=__builtin_bit_cast(unsigned, (.41f));
        require(okage::material_source_color(material,{.5f,.5f,.5f,1})==okage::Float4({127.5f,127.5f,127.5f,76}));
        require(okage::material_blend_register(material)==0x44);
        material.words[6]=1;require(okage::material_blend_register(material)==0x48);
        material.words[6]=2;require(okage::material_blend_register(material)==0x42);
        material.words[6]=3;rejects([&]{okage::material_blend_register(material);});
        material.words[5]=__builtin_bit_cast(unsigned, (1.f));
        require(okage::material_source_color(material,{-1,2,0,1})==okage::Float4({0,255,0,0}));
        material.words[5]=0;
        std::fesetround(FE_UPWARD);
        require(okage::material_source_color(material,{1,1,1,.5f})[3]==64);
        require(std::fegetround()==FE_UPWARD);std::fesetround(before);
        material.words[5]=__builtin_bit_cast(unsigned, (-1.f));
        rejects([&]{okage::material_source_color(material,{1,1,1,1});});
        material.words[5]=0;
        rejects([&]{okage::material_source_color(material,{1,1,1,std::numeric_limits<float>::quiet_NaN()});});
        rejects([]{okage::triangle_corners(5);});
        std::cout<<"Native render topology, UV boundaries and rounding checks passed\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
