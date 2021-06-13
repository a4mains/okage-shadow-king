#include "okage/pose.hpp"
#include <cfenv>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("pose test assertion failed"); }
void near(float a,float b) { require(std::abs(a-b)<0.00001f); }
int main() {
    try {
        std::fesetround(FE_TONEAREST);
        const auto sine=okage::make_sine_table();
        require(sine[0]==0 && sine[4096]==1 && sine[8192]==0 && sine[12288]==-1);
        for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            std::fesetround(mode);
            require(okage::make_sine_table()==sine && std::fegetround()==mode);
        }
        std::fesetround(FE_TONEAREST);
        okage::Model model{};
        model.bones.push_back({{2,3,4,0},{0,0,1.5707963705062866f,0},{1,2,3,0}});
        model.bones.push_back({{5,6,7,0},{0,0,0,0},{1,0,0,0}});
        model.hierarchy={{0,-1},{1,0}};
        const auto pose=okage::rest_pose(model,sine);
        // Translation uses accumulated parent scale followed by parent rotation.
        near(pose[0][0][0],0); near(pose[0][0][1],2);
        near(pose[1][3][0],1); near(pose[1][3][1],4); near(pose[1][3][2],3);
        near(pose[1][0][1],10); near(pose[1][1][0],-18); near(pose[1][2][2],28);
        require(std::fegetround()==FE_TONEAREST);
        const auto instance=okage::instance_matrix({10,-4,2,1},{0,0,0.25f,0},{2,3,4,0},sine);
        near(instance[0][0],0);near(instance[0][1],2);near(instance[1][0],-3);
        near(instance[3][0],10);near(instance[3][1],-4);near(instance[3][2],2);
        const auto placed=okage::compose_pose(instance,pose);
        near(placed[0][3][0],4);near(placed[0][3][1],-2);near(placed[0][3][2],14);
        const auto camera=okage::camera_matrix({1,2,3,1},{0,0,0.25f,0},{2,3,4,0},sine);
        near(camera[3][0],-4);near(camera[3][1],3);near(camera[3][2],12);
        require(std::fegetround()==FE_TONEAREST);
        okage::RenderBlock block{}; block.attribute_extents[0]=1;
        block.attributes.push_back({5,1,0,0,{{0.5f,0,0,0.25f}}});
        block.attributes.push_back({5,1,0,1,{{0,0,0,0.75f}}});
        const auto points=okage::posed_positions(block,pose);
        near(points[0][0],1); near(points[0][1],4.5f); near(points[0][2],3); near(points[0][3],1);
        require(std::fegetround()==FE_TONEAREST);
        model.hierarchy[0].parent=0;
        bool rejected=false;
        try { okage::rest_pose(model,sine); } catch (const std::runtime_error&) { rejected=true; }
        require(rejected && std::fegetround()==FE_TONEAREST);
        rejected=false;
        try {okage::instance_matrix({INFINITY,0,0,1},{0,0,0,0},{1,1,1,0},sine);}
        catch(const std::runtime_error&){rejected=true;}
        require(rejected && std::fegetround()==FE_TONEAREST);
        std::cout<<"Native hierarchy, weighted position and rounding tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
