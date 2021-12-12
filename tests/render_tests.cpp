#include "okage/render.hpp"
#include "okage/field_raster.hpp"
#include "okage/map_material.hpp"
#include "okage/ordinary_lighting.hpp"
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
        const okage::PreparedTextureState prepared(state,dims[0],dims[1]);
        const auto preparedClamp=okage::texture_clamp(state,dims[0],dims[1]);
        for(int mode:modes) {
            require(std::fesetround(mode)==0);
            require(prepared.clamp()==preparedClamp && std::fegetround()==mode);
            for(std::size_t count:{3u,4u}) {
                const auto coordinates=prepared.coordinates(std::span(source).first(count));
                require(std::fegetround()==mode);
                for(std::size_t i=0;i<count;++i)for(unsigned c=0;c<2;++c)
                    require(__builtin_bit_cast(std::uint32_t, (coordinates[i][c]))==__builtin_bit_cast(std::uint32_t, (expected[i][c])));
                if(count==3)require(coordinates[3]==std::array<float,2>{});
            }
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
void prepared_texture_guards() {
    const int previous=std::fegetround();
    for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        require(std::fesetround(mode)==0);
        okage::TextureState state{0,0,1,1,{0,0,1,1}};
        const okage::PreparedTextureState prepared(state,64,32);
        std::array<std::array<float,2>,4> source{{{0,0},{1,0},{1,1},{0,1}}};
        const auto expected=prepared.coordinates(source);
        // The descriptor is a snapshot, not a reference to animated state.
        state.region={8,4,24,12};state.texture=27;
        require(prepared.coordinates(source)==expected);
        rejects([&]{okage::PreparedTextureState invalid(state,0,32);});
        state.region[2]=std::numeric_limits<float>::quiet_NaN();
        rejects([&]{okage::PreparedTextureState invalid(state,64,32);});
        state.region={0,0,3e20f,3e20f};
        rejects([&]{okage::PreparedTextureState invalid(state,64,32);});
        rejects([&]{prepared.coordinates(std::span(source).first(2));});
        source[3][0]=std::numeric_limits<float>::infinity();
        rejects([&]{prepared.coordinates(source);});
        source[3][0]=0;
        require(prepared.coordinates(source)==expected); // Failed preparation does not corrupt it.
        require(std::fegetround()==mode);
    }
    require(std::fesetround(previous)==0);
}
int main() {
    try {
        rounding_batches();
        prepared_texture_guards();
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
        okage::Material mapMaterial{};
        mapMaterial.words[0]=__builtin_bit_cast(unsigned, (.125f)); // Not a source RGB multiplier.
        for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            std::fesetround(rounding);
            for(const auto& sample:std::array{
                    okage::Float4{.75f,.75f,.75f,1},okage::Float4{.59375f,.625f,.65625f,1}}) {
                const auto cache=okage::material_source_color(mapMaterial,sample);
                const auto expected=sample[0]==.75f?okage::Float4{191.25f,191.25f,191.25f,128}:
                    okage::Float4{151.40625f,159.375f,167.34375f,128};
                require(cache==expected);
                require(okage::ordinary_material_modulation(cache,{1,1,1,1})==expected);
                require(okage::ordinary_material_color(cache,{1,1,1,1})==
                    (sample[0]==.75f?okage::Float4{191,191,191,128}:okage::Float4{151,159,167,128}));
                require(std::fegetround()==rounding);
            }
        }
        std::fesetround(before);
        const auto source=okage::material_source_color(mapMaterial,{.5f,.5f,.5f,1});
        require(okage::ordinary_material_modulation(source,{.5f,1,2,.25f})==okage::Float4({63.75f,127.5f,255,32}));
        require(okage::map_material_class(mapMaterial,-1).base_lighting);
        require(!okage::map_material_class(mapMaterial,0).base_lighting);
        mapMaterial.words[7]=1;require(okage::map_material_class(mapMaterial,0).base_lighting);
        mapMaterial.words[7]=2;rejects([&]{okage::map_material_class(mapMaterial,-1);});mapMaterial.words[7]=0;
        require(!okage::map_material_class(mapMaterial,-1).translucent);
        mapMaterial.words[5]=__builtin_bit_cast(unsigned, (.001f));
        require(okage::map_material_class(mapMaterial,-1).translucent);
        require(okage::material_source_color(mapMaterial,{1,1,1,1})[3]==128);
        mapMaterial.words[5]=__builtin_bit_cast(unsigned, (.009f));
        require(okage::map_material_class(mapMaterial,-1).translucent);
        require(okage::material_source_color(mapMaterial,{1,1,1,1})[3]==127);
        mapMaterial.words[5]=__builtin_bit_cast(unsigned, (-.001f));rejects([&]{okage::map_material_class(mapMaterial,-1);});
        mapMaterial.words[5]=0;mapMaterial.words[6]=1;rejects([&]{okage::map_material_class(mapMaterial,-1);});
        okage::Polygon polygon{};polygon.flags=0x20;polygon.texture_state=0;
        require(!okage::map_material_textured(polygon)); // Selected image, TME off.
        polygon.flags=0x30;require(okage::map_material_textured(polygon));
        polygon.texture_state=-1;rejects([&]{okage::map_material_textured(polygon);});
        polygon.flags=0x20;require(!okage::map_material_textured(polygon));
        polygon.flags=0x21;rejects([&]{okage::map_material_textured(polygon);});
        okage::RenderBlock normalBlock;
        normalBlock.materials.resize(2);normalBlock.polygons.resize(2);
        normalBlock.polygons[0].material=0;normalBlock.polygons[0].corners.push_back({0,-1,0,-1});
        normalBlock.polygons[1].material=1;normalBlock.polygons[1].corners.push_back({0,0,0,-1});
        normalBlock.materials[0].words[4]=__builtin_bit_cast(unsigned, (71.f));
        require(okage::map_material_block_uses_normals(normalBlock));
        rejects([&]{okage::validate_map_material_extra_pass(normalBlock.materials[0],true);});
        normalBlock.materials[1].words[7]=1;
        require(!okage::map_material_block_uses_normals(normalBlock));
        okage::validate_map_material_extra_pass(normalBlock.materials[0],false);
        normalBlock.materials[0].words[4]=__builtin_bit_cast(unsigned, (70.f));
        okage::validate_map_material_extra_pass(normalBlock.materials[0],true);
        for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            std::fesetround(rounding);
            for(float transparency:{0.f,.001f,.009f,.4f,1.f}) {
                okage::Material m{};m.words[5]=__builtin_bit_cast(unsigned, (transparency));
                for(const auto& input:std::array{okage::Float4{.59375f,.625f,.65625f,1},
                        okage::Float4{0,1,2,.5f},okage::Float4{.1234567f,.9876543f,.3333333f,.77f}})
                    for(const auto& base:std::array{okage::Float4{1,1,1,1},okage::Float4{.5f,1,2,.25f},okage::Float4{0,.31f,.99f,1.25f}}) {
                        okage::PreparedMapBaseColor cache;
                        for(bool packed:{false,false,true,true,false,false,true}) {
                            const auto source=okage::material_source_color(m,input);
                            const auto expected=packed?okage::ordinary_material_color(source,base):okage::ordinary_material_modulation(source,base);
                            const auto actual=cache.get(m,input,base,packed);
                            require(__builtin_bit_cast(std::array<unsigned,4>, (actual))==__builtin_bit_cast(std::array<unsigned,4>, (expected)));
                            require(std::fegetround()==rounding);
                        }
                    }
            }
            okage::PreparedMapBaseColor failed;okage::Material invalid{};
            invalid.words[5]=__builtin_bit_cast(unsigned, (-1.f));
            rejects([&]{failed.get(invalid,{1,1,1,1},{1,1,1,1},false);});
            invalid.words[5]=0;
            rejects([&]{failed.get(invalid,{1,1,1,1},{-1,1,1,1},false);});
            rejects([&]{failed.get(invalid,{1,1,1,1},{std::numeric_limits<float>::quiet_NaN(),1,1,1},false);});
            require(failed.get(invalid,{.75f,.75f,.75f,1},{1,1,1,1},false)==okage::Float4({191.25f,191.25f,191.25f,128}));
            require(std::fegetround()==rounding);
        }
        std::fesetround(before);
        const std::array<int,2> translucent{10,30};
        std::vector<std::pair<int,unsigned>> passes{{20,3},{40,3}};
        okage::map_translucent_passes(std::span<const int>(translucent),[&](int id,unsigned pass){passes.emplace_back(id,pass);});
        require(passes==std::vector<std::pair<int,unsigned>>{{20,3},{40,3},{10,1},{30,1},{10,2},{30,2}});
        rejects([]{okage::triangle_corners(5);});
        std::cout<<"Native render topology, UV boundaries and rounding checks passed\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
