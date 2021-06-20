#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

class ReferenceWalk {
    struct Point {float x,z,radius;};
    std::vector<Point> points;
    std::size_t next=0;
    unsigned scene=0,still=0;
    bool stopped=false,havePrevious=false;
    float previousX=0,previousZ=0;
    std::ofstream recording;
    std::array<bool,4> previous{};
    std::array<bool,4> stop(unsigned frame,const char* reason) {
        if(!stopped)std::cout<<"walk_stop_vblank="<<frame<<" reason="<<reason<<" waypoint="<<next<<std::endl;
        stopped=true;return {};
    }
public:
    // Result order is up, down, left, right, matching RecordedPad controls4..7.
    static std::array<bool,4> steer(float dx,float dz,float yaw) {
        if(!std::isfinite(dx)||!std::isfinite(dz)||!std::isfinite(yaw))return {};
        const std::array<float,8> angles={0,.125f,.25f,.375f,.5f,.625f,.75f,.875f};
        const std::array<std::array<bool,4>,8> buttons={{{false,true,false,false},
            {false,true,false,true},{false,false,false,true},{true,false,false,true},
            {true,false,false,false},{true,false,true,false},{false,false,true,false},
            {false,true,true,false}}};
        float best=-INFINITY;unsigned selected=0;
        for(unsigned i=0;i<angles.size();++i) {
            const double angle=(1.-yaw+angles[i])*6.283185307179586;
            const float score=dx*std::sin(angle)-dz*std::cos(angle);
            if(score>best){best=score;selected=i;}
        }
        return buttons[selected];
    }
    ReferenceWalk(const std::filesystem::path& path,const std::filesystem::path& output) {
        if(std::filesystem::file_size(path)>65536)throw std::runtime_error("Waypoint file exceeds limit");
        std::ifstream input(path);std::string line,extra;
        if(!std::getline(input,line))throw std::runtime_error("Missing waypoint scene");
        std::istringstream head(line);
        if(!(head>>scene)||head>>extra||scene>4095)throw std::runtime_error("Invalid waypoint scene");
        while(std::getline(input,line)) {
            std::istringstream row(line);row>>std::ws;if(row.eof())continue;
            Point p;
            if(!(row>>p.x>>p.z>>p.radius)||row>>extra||!std::isfinite(p.x)||!std::isfinite(p.z)||
               !std::isfinite(p.radius)||p.radius<=0||p.radius>20||points.size()>=256)
                throw std::runtime_error("Invalid waypoint row (X Z RADIUS)");
            points.push_back(p);
        }
        if(points.empty())throw std::runtime_error("Empty waypoint route");
        if(output.empty()||std::filesystem::exists(output)||std::filesystem::is_symlink(output))
            throw std::runtime_error("Refusing missing or existing walk recording");
        recording.open(output);if(!recording)throw std::runtime_error("Cannot create walk recording");
    }
    std::array<bool,4> sample(const std::uint8_t* ram,unsigned frame) {
        if(stopped)return {};
        if(!ram)return stop(frame,"missing-ram");
        auto valid=[](std::uint32_t at,unsigned size){return at>=0x100000&&at<=0x2000000-size;};
        auto word=[&](std::uint32_t at){std::uint32_t v;std::memcpy(&v,ram+at,4);return v;};
        auto number=[&](std::uint32_t at){float v;std::memcpy(&v,ram+at,4);return v;};
        const auto field=word(0x2d9bf4),instance=word(0x337b64),camera=word(0x337d8c);
        const auto objects=word(0x21301c),channels=word(0x33760c),ctx=word(0x337614);
        if(!valid(field,0x800)||instance>=2048||camera>=5119||!valid(objects,2048*0x3c)||
           !valid(channels,5120*0x90)||!valid(ctx,24))return stop(frame,"invalid-field");
        const auto game=word(field+0x14),player=word(field+0x44);
        if(!valid(game,0x60)||!valid(player,0x284))return stop(frame,"invalid-player");
        if(word(game+0x44)!=scene)return stop(frame,"scene-changed");
        if(word(field+0x48)!=0||(word(ctx)&0x100)||(word(player+0x22c)&1))
            return stop(frame,"event-or-control-change");
        const auto hero=word(objects+instance*0x3c+4)&0xffff;
        if(hero>=5119)return stop(frame,"invalid-hero-channel");
        const auto position=channels+(hero+1)*0x90+0x20;
        const float x=number(position),z=number(position+8),yaw=number(channels+(camera+1)*0x90+0x24);
        if(!std::isfinite(x)||!std::isfinite(z)||!std::isfinite(yaw))return stop(frame,"nonfinite-position");
        if(havePrevious&&std::hypot(x-previousX,z-previousZ)<.01f)++still;else still=0;
        previousX=x;previousZ=z;havePrevious=true;
        if(still>=120)return stop(frame,"no-movement-for-120-polls");
        while(next<points.size()&&std::hypot(points[next].x-x,points[next].z-z)<=points[next].radius) {
            std::cout<<"walk_waypoint_vblank="<<frame<<" index="<<next<<" x="<<x<<" z="<<z<<std::endl;++next;
        }
        if(next==points.size())return stop(frame,"route-complete");
        return steer(points[next].x-x,points[next].z-z,yaw);
    }
    void record(unsigned frame,const std::array<bool,4>& buttons) {
        constexpr std::array<const char*,4> names={"up","down","left","right"};
        for(unsigned i=0;i<buttons.size();++i)if(buttons[i]!=previous[i])
            recording<<frame<<' '<<names[i]<<' '<<unsigned(buttons[i])<<'\n';
        previous=buttons;recording.flush();
        if(!recording)throw std::runtime_error("Cannot write walk input recording");
    }
};
