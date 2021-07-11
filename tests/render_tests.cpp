#include "okage/render.hpp"
#include <cfenv>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("Render test failed"); }
template<class F> void rejects(F f) {
    bool threw=false;
    try {f();} catch (const std::runtime_error&) {threw=true;}
    require(threw);
}
int main() {
    try {
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
