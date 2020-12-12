#include "okage/render.hpp"
#include <cfenv>
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
        require(std::fegetround()==FE_UPWARD); std::fesetround(before);
        require(okage::triangle_corners(3)==std::vector<std::array<unsigned,3>>{{0,1,2}});
        require(okage::triangle_corners(4)==std::vector<std::array<unsigned,3>>{{0,1,2},{0,2,3}});
        rejects([]{okage::triangle_corners(5);});
        std::cout<<"Native render topology, UV boundaries and rounding checks passed\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
