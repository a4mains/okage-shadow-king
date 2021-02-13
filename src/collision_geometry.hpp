#pragma once
#include "okage/collision.hpp"
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage::collision_geometry {
inline Float4 add(const Float4& a,const Float4& b){Float4 r;for(unsigned i=0;i<4;++i)r[i]=a[i]+b[i];return r;}
inline Float4 sub(const Float4& a,const Float4& b){Float4 r;for(unsigned i=0;i<4;++i)r[i]=a[i]-b[i];return r;}
inline float dot(const Float4& a,const Float4& b){return (a[0]*b[0]+a[1]*b[1])+a[2]*b[2];}
inline Float4 cross(const Float4& a,const Float4& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0],0};
}
inline float distance_squared(const Float4& a,const Float4& b){const auto d=sub(a,b);return dot(d,d);}
inline Float4 normalized(const Float4& v) {
    const float length=std::sqrt(dot(v,v));
    if(length==0)return {};
    const float inverse=1.f/length;return {v[0]*inverse,v[1]*inverse,v[2]*inverse,0};
}
inline bool parallel(const Float4& a,const Float4& b) {
    const float product=dot(a,b);return (dot(a,a)*dot(b,b))*0.9997999668121337890625f<product*product;
}
struct Line {Float4 point,direction;};
struct Plane {Float4 point,normal;};
struct Projection {Float4 point{};float parameter=0,distance_squared=0;};
inline Projection project_line(const Line& line,const Float4& point) {
    // 0x188be8 accumulates Z/Y/X and subtracts products before summing.
    const auto& d=line.direction;const auto& p=line.point;
    const float denominator=(d[2]*d[2]+d[1]*d[1])+d[0]*d[0];
    if(denominator==0)throw std::runtime_error("Degenerate collision line");
    const float dz=point[2]*d[2]-p[2]*d[2],dy=point[1]*d[1]-p[1]*d[1],dx=point[0]*d[0]-p[0]*d[0];
    const float numerator=(dz+dy)+dx,inverse=1.f/denominator;
    Projection result;result.parameter=numerator*inverse;
    for(unsigned i=0;i<3;++i)result.point[i]=p[i]+(d[i]*numerator)*inverse;
    const auto delta=sub(point,result.point);
    result.distance_squared=(delta[2]*delta[2]+delta[1]*delta[1])+delta[0]*delta[0];return result;
}
inline float plane_distance_squared(const Plane& plane,const Float4& point) {
    const float denominator=dot(plane.normal,plane.normal);
    if(denominator==0)throw std::runtime_error("Degenerate collision plane");
    const float numerator=dot(sub(point,plane.point),plane.normal);
    return numerator*(numerator/denominator);
}
inline bool intersect_line_plane(const Plane& plane,const Line& line,Float4& point) {
    // Original 0x188770 distributes the dot product into each coordinate.
    const auto& n=plane.normal;const auto& d=line.direction;
    const float denominator=dot(n,d);if(denominator==0)return false;
    const float inverse=1.f/denominator;Float4 product{},delta{};
    for(unsigned i=0;i<3;++i){product[i]=n[i]*d[i];delta[i]=n[i]*(plane.point[i]-line.point[i]);}
    for(unsigned i=0;i<3;++i) {
        float v=(i==0?plane.point[i]:line.point[i])*product[0];
        v=v+(i==1?plane.point[i]:line.point[i])*product[1];
        v=v+(i==2?plane.point[i]:line.point[i])*product[2];
        const float others=i==0?delta[1]+delta[2]:(i==1?delta[0]+delta[2]:delta[0]+delta[1]);
        point[i]=(v+d[i]*others)*inverse;
    }
    point[3]=0;return true;
}
inline bool intersect_planes(const Plane& a,const Plane& b,Line& line) {
    if(parallel(a.normal,b.normal))return false;
    const auto& n=a.normal;const auto& m=b.normal;
    const float ad=dot(n,a.point),bd=dot(m,b.point);
    float denominator=n[0]*m[1]-m[0]*n[1];
    if(denominator!=0) {
        const float inverse=1.f/denominator;
        line.point={(m[1]*ad-n[1]*bd)*inverse,(m[0]*ad-n[0]*bd)*(-inverse),0,0};
        line.direction={(n[1]*m[2]-m[1]*n[2])*inverse,(n[0]*m[2]-m[0]*n[2])*(-inverse),1,0};
    } else {
        denominator=n[2]*m[1]-m[2]*n[1];
        if(denominator!=0) {
            const float inverse=1.f/denominator;
            line.point={0,(m[2]*ad-n[2]*bd)*(-inverse),(m[1]*ad-n[1]*bd)*inverse,0};
            line.direction={1,(m[0]*n[2]-n[0]*m[2])*(-inverse),(n[1]*m[0]-m[1]*n[0])*inverse,0};
        } else {
            denominator=n[0]*m[2]-m[0]*n[2];if(denominator==0)return false;
            const float inverse=1.f/denominator;
            line.point={(m[2]*ad-n[2]*bd)*inverse,0,(m[0]*ad-n[0]*bd)*(-inverse),0};
            line.direction={(m[1]*n[2]-n[1]*m[2])*inverse,1,(m[1]*n[0]-n[1]*m[0])*(-inverse),0};
        }
    }
    return true;
}
struct ClosestLines {Float4 first{},second{};float first_parameter=0,second_parameter=0,distance_squared=0;};
inline ClosestLines closest_lines(const Line& a,const Line& b) {
    const float bb=dot(b.direction,b.direction),aa=dot(a.direction,a.direction),ab=dot(a.direction,b.direction);
    const auto delta=sub(b.point,a.point);
    const float bd=dot(b.direction,delta),ad=dot(a.direction,delta),denominator=ab*ab-aa*bb;
    ClosestLines result;
    if(denominator==0) {
        const auto projection=project_line(a,b.point);
        result.first=projection.point;result.first_parameter=projection.parameter;
        result.second=b.point;result.distance_squared=projection.distance_squared;return result;
    }
    const float inverse=1.f/denominator;
    result.second_parameter=(aa*bd-ab*ad)*inverse;
    result.first_parameter=(ab*bd-bb*ad)*inverse;
    for(unsigned i=0;i<3;++i) {
        result.first[i]=a.direction[i]*result.first_parameter+a.point[i];
        result.second[i]=b.direction[i]*result.second_parameter+b.point[i];
    }
    result.distance_squared=distance_squared(result.second,result.first);return result;
}
inline float line_parameter(const Line& line,const Float4& point) {
    const unsigned axis=std::fabs(line.direction[0])>0.1f?0:(std::fabs(line.direction[1])>0.1f?1:2);
    if(line.direction[axis]==0)throw std::runtime_error("Degenerate collision edge parameter");
    return (point[axis]-line.point[axis])/line.direction[axis];
}
inline bool in_segment(float parameter){return parameter>=0&&parameter<=1;}
}
