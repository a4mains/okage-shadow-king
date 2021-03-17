#include "okage/collision.hpp"
#include "collision_geometry.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
using namespace collision_geometry;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Rounding {
    int old=std::fegetround();
    Rounding(){require(!std::fesetround(FE_TOWARDZERO),"Camera collision rounding unavailable");}
    ~Rounding(){std::fesetround(old);}
};
Float4 normal(const CollisionPolygon& p) {
    return {float(__builtin_bit_cast(std::int16_t, (p.words[3])))/16384.f,
            float(__builtin_bit_cast(std::int16_t, (p.words[2])))/16384.f,
            float(__builtin_bit_cast(std::int16_t, (p.words[4])))/16384.f,0};
}
// Original 0x1895a0 clips X, then Z, then tests Y. Upper bounds are exclusive.
bool segment_cell(Float4 a,Float4 b,const Float4& low,const Float4& high) {
    for(unsigned axis:{0u,2u,1u}) {
        const unsigned first=a[axis]<low[axis]?1:(a[axis]>=high[axis]?2:0);
        const unsigned last=b[axis]<low[axis]?1:(b[axis]>=high[axis]?2:0);
        if(first&&first==last)return false;
        if(axis==1)return true;
        Float4 slope{};
        for(unsigned i=0;i<3;++i)if(i!=axis)
            slope[i]=b[axis]==a[axis]?0:(b[i]-a[i])/(b[axis]-a[axis]);
        const auto start=a;
        if(first)for(unsigned i=0;i<3;++i)if(i!=axis)
            a[i]=start[i]+slope[i]*((first==1?low[axis]:high[axis])-start[axis]);
        if(last)for(unsigned i=0;i<3;++i)if(i!=axis)
            b[i]=start[i]+slope[i]*((last==1?low[axis]:high[axis])-start[axis]);
    }
    return true;
}
Float4 project_plane(const Plane& plane,const Float4& point) {
    const float denominator=dot(plane.normal,plane.normal);
    require(denominator!=0,"Degenerate sphere contact plane");
    const float ratio=dot(sub(point,plane.point),plane.normal)/denominator;
    auto result=point;for(unsigned i=0;i<4;++i)result[i]-=plane.normal[i]*ratio;
    return result;
}
}
std::vector<std::uint32_t> CollisionMap::segment_query(const Float4& first,const Float4& second) const {
    Rounding rounding;std::array<int,3> low{},high{};Float4 origin{};
    for(unsigned axis=0;axis<3;++axis) {
        origin[axis]=float(__builtin_bit_cast(std::int32_t, (std::uint32_t(header[axis])*std::uint32_t(header[axis+6]))));
        auto index=[&](float v) {
            const float p=v-origin[axis];
            require(std::isfinite(p)&&p> -2147483648.f&&p<2147483648.f,"Segment query coordinate bounds");
            auto n=std::int32_t(p)>>header[axis+3];if(p<0)--n;return n;
        };
        const int a=index(first[axis]),b=index(second[axis]);
        low[axis]=std::max(0,std::min(a,b));high[axis]=std::min(header[12+axis]-1,std::max(a,b));
    }
    std::vector<std::uint32_t> result;std::vector<float> distance;
    for(int z=low[2];z<=high[2]&&result.size()<64;++z)
        for(int x=low[0];x<=high[0]&&result.size()<64;++x)
            for(int y=low[1];y<=high[1]&&result.size()<64;++y) {
                const auto chunk=cells.at((std::size_t(z)*header[12]+x)*header[13]+y);if(chunk<0)continue;
                Float4 a{},b{},delta{};const std::array<int,3> cell{x,y,z};
                for(unsigned i=0;i<3;++i) {
                    a[i]=float(__builtin_bit_cast(std::int32_t, (std::uint32_t(cell[i])<<header[3+i])))+origin[i];
                    b[i]=float(__builtin_bit_cast(std::int32_t, (std::uint32_t(cell[i]+1)<<header[3+i])))+origin[i];
                    delta[i]=(a[i]+b[i])*.5f-first[i];
                }
                if(segment_cell(first,second,a,b)){result.push_back(chunk);distance.push_back(dot(delta,delta));}
            }
    // Preserve the original swap ordering, including equal-distance candidates.
    for(unsigned i=0;i<result.size();++i)for(unsigned j=i+1;j<result.size();++j)
        if(distance[j]<distance[i]){std::swap(distance[i],distance[j]);std::swap(result[i],result[j]);}
    return result;
}
SurfaceContact CollisionMap::segment_contact(const Float4& first,const Float4& second,std::uint32_t mask) const {
    Rounding rounding;const auto direction=sub(second,first);
    if(direction[0]==0&&direction[1]==0&&direction[2]==0)return {};
    const auto candidates=segment_query(first,second);SurfaceContact result;Float4 closest{};float distance=0;
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto& chunk=chunks.at(candidates[ci]);
        for(unsigned pi=0;pi<chunk.polygons.size();++pi) {
            const auto& polygon=chunk.polygons[pi];const auto& w=polygon.words;
            if(mask&(1u<<((w[0]&0x3e)>>1)))continue;
            const auto n=normal(polygon);const Plane plane{chunk.vertices.at(w[5]),n};
            if(dot(sub(second,plane.point),n)>0||dot(sub(first,plane.point),n)<0)continue;
            Float4 point{};if(!intersect_line_plane(plane,{first,direction},point))continue;
            bool inside=true;
            for(unsigned edge=0;edge<polygon.vertex_count();++edge) {
                const auto& a=chunk.vertices.at(w[5+edge]);const auto& b=chunk.vertices.at(w[6+edge]);
                if(dot(cross(sub(b,a),n),sub(point,a))>0){inside=false;break;}
            }
            if(!inside)continue;
            const float squared=distance_squared(point,first);
            if(!result.hit||squared<distance) {
                result.hit=true;result.material=w[1];result.normal=n;
                result.chunk=candidates[ci];result.polygon=pi;result.candidate=ci;closest=point;distance=squared;
            }
        }
        if(result.hit) {
            for(unsigned i=0;i<3;++i) {
                const float delta=(closest[i]-second[i])+result.normal[i]*.001f;
                // 0x1882a0 adds then subtracts the requested endpoint.
                result.correction[i]=(second[i]+delta)-second[i];
            }
            result.correction[3]=1;return result;
        }
    }
    return result;
}
SurfaceContact CollisionMap::sphere_wall_contact(const Float4& previous,const Float4& desired,
                                                 const Float4& offset,float radius,std::int32_t threshold,
                                                 std::uint32_t mask) const {
    Rounding rounding;
    require(std::isfinite(radius)&&radius>0&&threshold>=0&&threshold<=16384,"Invalid sphere contact settings");
    for(const auto& point:{previous,desired,offset})for(float v:point)require(std::isfinite(v),"Invalid sphere contact point");
    const auto first=add(previous,offset),second=add(desired,offset),movement=sub(second,first);
    Float4 low{},high{};
    for(unsigned i=0;i<3;++i){low[i]=std::min(first[i],second[i])-radius;high[i]=std::max(first[i],second[i])+radius;}
    const auto candidates=query(low,high,2);std::vector<std::vector<unsigned>> marked;
    const float squared_radius=radius*radius;
    auto hit=[&](unsigned ci,unsigned pi,Float4 correction) {
        SurfaceContact r;r.hit=true;r.chunk=candidates[ci];r.polygon=pi;r.candidate=ci;
        const auto& p=chunks.at(r.chunk).polygons[pi];r.material=p.words[1];r.normal=normal(p);
        r.correction=correction;return r;
    };
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto& chunk=chunks.at(candidates[ci]);marked.emplace_back(chunk.polygons.size(),0);
        for(unsigned pi=0;pi<chunk.polygons.size();++pi) {
            const auto& polygon=chunk.polygons[pi];const auto& w=polygon.words;
            const auto ny=__builtin_bit_cast(std::int16_t, (w[2]));
            if(ny>threshold||ny< -threshold||(mask&(1u<<((w[0]&0x3e)>>1))))continue;
            const auto n=normal(polygon);const Plane plane{chunk.vertices.at(w[5]),n};
            if(dot(movement,n)>0||dot(sub(first,plane.point),n)<0)continue;
            if(plane_distance_squared(plane,second)>squared_radius)continue;
            marked[ci][pi]=1;bool inside=true;
            for(unsigned edge=0;edge<polygon.vertex_count();++edge) {
                const auto& a=chunk.vertices.at(w[5+edge]);const auto& b=chunk.vertices.at(w[6+edge]);
                if(dot(cross(sub(b,a),n),sub(second,a))>0){inside=false;break;}
            }
            if(!inside)continue;
            auto flat=n;flat[1]=0;flat=normalized(flat);
            const auto projected=project_plane({project_plane(plane,second),flat},second);
            Float4 correction{0,0,0,1};for(unsigned i:{0u,2u})correction[i]=(projected[i]+n[i]*(radius+.001f))-second[i];
            return hit(ci,pi,correction);
        }
    }
    auto radial=[&](unsigned ci,unsigned pi,const Float4& point,bool fallback) {
        auto delta=sub(second,point);
        if(fallback&&delta[0]==0&&delta[1]==0&&delta[2]==0)delta=sub(desired,previous);
        const auto unit=normalized(delta);Float4 correction{};
        for(unsigned i=0;i<3;++i)correction[i]=(point[i]+(unit[i]+.001f)*radius)-second[i];
        return hit(ci,pi,correction);
    };
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto& chunk=chunks.at(candidates[ci]);
        for(unsigned pi=0;pi<chunk.polygons.size();++pi)if(marked[ci][pi]) {
            const auto& polygon=chunk.polygons[pi];const auto& w=polygon.words;
            for(unsigned edge=0;edge<polygon.vertex_count();++edge) {
                const auto& a=chunk.vertices.at(w[5+edge]);const auto& b=chunk.vertices.at(w[6+edge]);
                const auto projection=project_line({a,sub(b,a)},second);
                if(projection.distance_squared<=squared_radius) {
                    marked[ci][pi]=2;
                    if(in_segment(projection.parameter))return radial(ci,pi,projection.point,true);
                }
            }
        }
    }
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto& chunk=chunks.at(candidates[ci]);
        for(unsigned pi=0;pi<chunk.polygons.size();++pi)if(marked[ci][pi]==2) {
            const auto& polygon=chunk.polygons[pi];
            for(unsigned v=0;v<polygon.vertex_count();++v) {
                const auto& point=chunk.vertices.at(polygon.words[5+v]);
                if(distance_squared(second,point)<=squared_radius)return radial(ci,pi,point,false);
            }
        }
    }
    return {};
}
MapContact CollisionMap::camera_move(const Float4& previous,Float4& desired,std::uint32_t mask) const {
    Rounding rounding;MapContact result;const auto initial=desired;
    const std::array<CollisionShape,1> shapes={{{0x39,{{0,0,0,1},{},4.5f}}}};
    MapCollisionSettings settings;settings.floor_reach=0;settings.wall_iterations=2;settings.disabled_mask=mask;
    for(unsigned pass=0;pass<2;++pass) {
        const auto contact=move_contact(previous,desired,shapes,settings);if(!contact.hit)break;
        result.hit=true;result.flags|=contact.flags;desired=add(desired,contact.correction);
        for(unsigned i=0;i<3;++i)if(contact.flags&(1u<<i)) {
            if(!result.materials[i])result.materials[i]=contact.materials[i];
            result.normals[i]=contact.normals[i];
        }
    }
    if(result.hit){result.correction=sub(desired,initial);result.correction[3]=1;desired[3]=1;}
    return result;
}
}
