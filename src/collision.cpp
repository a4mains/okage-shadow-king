#include "okage/collision.hpp"
#include "collision_geometry.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
struct Reader {
    Bytes b;
    void range(std::size_t at,std::size_t size)const{require(at<=b.size()&&size<=b.size()-at,"XCL range outside input");}
    std::uint32_t integer(std::size_t at,unsigned size=4)const {
        range(at,size);std::uint32_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint32_t(b[at+i])<<(8*i);return value;
    }
};
struct Rounding {
    int old=std::fegetround();
    Rounding(){require(!std::fesetround(FE_TOWARDZERO),"Cannot set collision rounding");}
    ~Rounding(){std::fesetround(old);}
};
}
CollisionMap read_xcl(Bytes bytes) {
    Reader source{bytes};source.range(0,69);
    const auto end=bytes.size()-4,stream=std::size_t(source.integer(end));
    require(stream>=64&&stream%4==0&&stream<end,"Invalid XCL relocation extent");
    CollisionMap result;result.payload.assign(bytes.begin(),bytes.begin()+stream);
    Reader r{result.payload};std::set<std::uint32_t> relocated;
    std::size_t at=stream+1,cursor=0;bool initialized=false;
    unsigned tag=source.integer(stream,1),shift=6;
    auto relocate=[&] {
        require(initialized&&cursor%4==0,"Invalid XCL relocation cursor");r.range(cursor,4);
        require(relocated.insert(std::uint32_t(cursor)).second,"Repeated XCL relocation slot");
        require(r.integer(cursor)<stream,"XCL pointer outside payload");
        result.pointer_locations.push_back(std::uint32_t(cursor));cursor+=4;
    };
    while(at<end) {
        const auto op=(tag>>shift)&3;
        const unsigned width=op==2?2:(op==3?4:1);
        require(width<=end-at,"Truncated XCL relocation operand");
        const auto value=source.integer(at,width);at+=width;
        if(op==0) {for(unsigned i=0;i<=value;++i)relocate();}
        else {
            if(op==3){cursor=value;initialized=true;}
            else {require(initialized,"Relative XCL relocation before absolute cursor");cursor+=(std::size_t(value)+1)*4;}
            relocate();
        }
        if(shift==0) {
            // Retail may read an unused next tag from the footer on exit.
            if(at<end)tag=source.integer(at++,1);shift=6;
        } else shift-=2;
    }
    require(at==end,"XCL relocation stream overrun");
    auto pointer=[&](std::size_t position) {
        const auto value=r.integer(position);
        require(relocated.contains(std::uint32_t(position))||value==0,"XCL pointer missing relocation");
        return value;
    };
    for(unsigned i=0;i<15;++i)result.header[i]=__builtin_bit_cast(std::int32_t, (r.integer(i*4)));
    std::uint64_t count=1;
    for(unsigned axis=0;axis<3;++axis) {
        const auto& h=result.header;
        require(h[axis]>0&&h[3+axis]>=0&&h[3+axis]<31,"Invalid XCL cell scale");
        require(h[12+axis]>=0&&h[12+axis]<=65536,"Invalid XCL grid dimension");
        count*=unsigned(h[12+axis]);require(count<=1048576,"XCL grid allocation limit");
    }
    result.cells.assign(std::size_t(count),-1);
    const auto nx=unsigned(result.header[12]),ny=unsigned(result.header[13]),nz=unsigned(result.header[14]);
    const auto root=pointer(60);std::map<std::uint32_t,unsigned> chunks;
    if(root)r.range(root,std::size_t(nz)*4);
    for(unsigned z=0;root&&z<nz;++z) {
        const auto xs=pointer(root+z*4);if(!xs)continue;r.range(xs,std::size_t(nx)*4);
        for(unsigned x=0;x<nx;++x) {
            const auto ys=pointer(xs+x*4);if(!ys)continue;r.range(ys,std::size_t(ny)*4);
            for(unsigned y=0;y<ny;++y) {
                const auto leaf=pointer(ys+y*4);if(!leaf)continue;
                auto found=chunks.find(leaf);
                if(found==chunks.end()) {
                    r.range(leaf,12);CollisionChunk chunk;chunk.source_offset=leaf;
                    const auto nv=r.integer(leaf,2),np=r.integer(leaf+2,2);
                    require(nv<32768&&np<32768,"Negative XCL chunk count");
                    const auto vs=pointer(leaf+4),ps=pointer(leaf+8);
                    r.range(vs,std::size_t(nv)*16);r.range(ps,std::size_t(np)*20);
                    for(unsigned i=0;i<nv;++i) {
                        Float4 v;for(unsigned j=0;j<4;++j){v[j]=__builtin_bit_cast(float, (r.integer(vs+i*16+j*4)));require(std::isfinite(v[j]),"Non-finite XCL vertex");}
                        chunk.vertices.push_back(v);
                    }
                    for(unsigned i=0;i<np;++i) {
                        CollisionPolygon polygon;for(unsigned j=0;j<10;++j)polygon.words[j]=r.integer(ps+i*20+j*2,2);
                        // The edge loops also read the closing vertex index.
                        for(unsigned j=0;j<=polygon.vertex_count();++j)require(polygon.words[5+j]<nv,"XCL polygon vertex outside chunk");
                        chunk.polygons.push_back(polygon);
                    }
                    const auto index=unsigned(result.chunks.size());result.chunks.push_back(std::move(chunk));
                    found=chunks.emplace(leaf,index).first;
                }
                result.cells[(std::size_t(z)*nx+x)*ny+y]=found->second;
            }
        }
    }
    return result;
}
std::vector<std::uint32_t> CollisionMap::query(const Float4& first,const Float4& second,unsigned mode) const {
    require(mode<=2,"Unsupported XCL query mode");Rounding rounding;
    std::array<std::int32_t,3> low,high;
    for(unsigned axis=0;axis<3;++axis) {
        const auto product=std::uint32_t(header[axis])*std::uint32_t(header[6+axis]);
        const float origin=float(__builtin_bit_cast(std::int32_t, (product)));
        auto index=[&](float v) {
            const float p=v-origin;
            require(std::isfinite(p)&&p>-2147483648.0f&&p<2147483648.0f,"XCL query coordinate outside conversion range");
            auto n=std::int32_t(p)>>header[3+axis];if(p<0)--n;return n;
        };
        const auto a=index(first[axis]),b=index(second[axis]);
        low[axis]=std::max(0,std::min(a,b));high[axis]=std::min(header[12+axis]-1,std::max(a,b));
    }
    std::vector<std::uint32_t> result;
    for(int z=low[2];z<=high[2];++z)for(int x=low[0];x<=high[0];++x) {
        const int begin=mode==1?high[1]:low[1],last=mode==1?low[1]:high[1],step=mode==1?-1:1;
        if(low[1]>high[1])continue;
        for(int y=begin;;y+=step) {
            const auto chunk=cells[(std::size_t(z)*header[12]+x)*header[13]+y];
            if(chunk>=0){require(result.size()<64,"XCL exceeds original query capacity");result.push_back(unsigned(chunk));}
            if(y==last)break;
        }
    }
    return result;
}
SurfaceContact CollisionMap::vertical_contact(const Float4& previous,const Float4& desired,
                                             bool ceiling,float reach,std::int32_t threshold,
                                             std::uint32_t disabled_mask) const {
    Rounding rounding;
    require(std::isfinite(reach)&&reach>=0&&threshold>=0&&threshold<=16384,"Invalid XCL vertical-contact settings");
    for(unsigned i=0;i<3;++i)require(std::isfinite(previous[i])&&std::isfinite(desired[i]),"Non-finite XCL contact position");
    Float4 first=desired,second=desired;
    first[1]=ceiling ? previous[1]+float(header[1]) : previous[1]-float(header[1]);
    if(!ceiling)second[1]=desired[1]+reach;
    const auto candidates=query(first,second,ceiling?1:0);
    SurfaceContact result;float height=0;
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto chunk_index=candidates[ci];const auto& chunk=chunks.at(chunk_index);
        const int count=int(chunk.polygons.size()),step=ceiling?-1:1;
        for(int pi=ceiling?count-1:0;pi>=0&&pi<count;pi+=step) {
            const auto& polygon=chunk.polygons[pi];const auto& w=polygon.words;
            const auto ny=__builtin_bit_cast(std::int16_t, (w[2]));
            if(disabled_mask&(1u<<((w[0]&0x3e)>>1)))continue;
            const bool opposite=result.hit;
            if(ceiling ? (opposite?ny>=0:ny<=threshold)
                       : (opposite?ny<=0:ny>=-threshold))continue;
            bool inside=true;const Float4* vertex=nullptr;
            for(unsigned i=0;i<polygon.vertex_count();++i) {
                vertex=&chunk.vertices.at(w[5+i]);
                const auto& next=chunk.vertices.at(w[6+i]);
                const float ex=next[0]-(*vertex)[0],ez=next[2]-(*vertex)[2];
                const float px=desired[0]-(*vertex)[0],pz=desired[2]-(*vertex)[2];
                const float side=ex*(-pz)+ez*px;
                const bool positive=ceiling!=opposite;
                if(positive?side<0:side>0){inside=false;break;}
            }
            if(!inside)continue;
            const Float4 normal={float(__builtin_bit_cast(std::int16_t, (w[3])))/16384.f,
                                 float(ny)/16384.f,
                                 float(__builtin_bit_cast(std::int16_t, (w[4])))/16384.f,0};
            float y=normal[0]*((*vertex)[0]-desired[0]);
            y=y+normal[1]*(*vertex)[1];
            y=(y+normal[2]*((*vertex)[2]-desired[2]))/normal[1];
            if(opposite) {
                if(ceiling?desired[1]<=y:y<=desired[1]-0.01f)result.hit=false;
            } else if(ceiling?desired[1]<=y:y<=desired[1]+reach) {
                result.hit=true;result.material=w[1];result.normal=normal;
                result.chunk=chunk_index;result.polygon=unsigned(pi);result.candidate=ci;height=y;
            }
        }
    }
    if(result.hit)result.correction={0,ceiling?(height-desired[1])+0.001f:(height-desired[1])-0.001f,0,1};
    return result;
}
SurfaceContact CollisionMap::capsule_wall_contact(const Float4& previous,const Float4& desired,
                                                 const CollisionCapsule& capsule,std::int32_t threshold,
                                                 std::uint32_t disabled_mask) const {
    using namespace collision_geometry;Rounding rounding;
    require(std::isfinite(capsule.radius)&&capsule.radius>0&&threshold>=0&&threshold<=16384,"Invalid capsule wall settings");
    for(const auto& v:{previous,desired,capsule.first,capsule.second})
        for(unsigned i=0;i<3;++i)require(std::isfinite(v[i]),"Non-finite capsule wall point");
    const auto first=add(desired,capsule.first),second=add(desired,capsule.second);
    const auto old_first=add(previous,capsule.first),old_second=add(previous,capsule.second);
    const auto movement=sub(first,old_first),axis=sub(second,first);
    require(dot(axis,axis)>0,"Degenerate collision capsule");
    if(movement[0]==0&&movement[1]==0&&movement[2]==0)return {};
    Float4 low{},high{};
    for(unsigned i=0;i<3;++i) {
        low[i]=std::min(std::min(first[i],second[i]),std::min(old_first[i],old_second[i]))-capsule.radius;
        high[i]=std::max(std::max(first[i],second[i]),std::max(old_first[i],old_second[i]))+capsule.radius;
    }
    const auto candidates=query(low,high,2);std::vector<std::vector<bool>> marked;
    const float radius_squared=capsule.radius*capsule.radius;
    auto normal=[](const CollisionPolygon& p) {
        return Float4{float(__builtin_bit_cast(std::int16_t, (p.words[3])))/16384.f,
                      float(__builtin_bit_cast(std::int16_t, (p.words[2])))/16384.f,
                      float(__builtin_bit_cast(std::int16_t, (p.words[4])))/16384.f,0};
    };
    auto contact=[&](unsigned ci,unsigned pi,Float4 delta,float distance) {
        SurfaceContact r;r.hit=true;r.chunk=candidates[ci];r.polygon=pi;r.candidate=ci;
        const auto& p=chunks[r.chunk].polygons[pi];r.material=p.words[1];r.normal=normal(p);
        delta[1]=0;const auto direction=normalized(delta);
        const float amount=(capsule.radius-std::sqrt(distance))+0.001f;
        for(unsigned i=0;i<4;++i)r.correction[i]=direction[i]*amount;
        return r;
    };
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto& chunk=chunks[candidates[ci]];marked.emplace_back(chunk.polygons.size(),false);
        for(unsigned pi=0;pi<chunk.polygons.size();++pi) {
            const auto& polygon=chunk.polygons[pi];const auto& w=polygon.words;
            const auto ny=__builtin_bit_cast(std::int16_t, (w[2]));
            if(ny>threshold||ny< -threshold||(disabled_mask&(1u<<((w[0]&0x3e)>>1))))continue;
            const auto n=normal(polygon);if(dot(movement,n)>0)continue;
            const Plane plane{chunk.vertices.at(w[5]),n};
            const float first_distance=plane_distance_squared(plane,first),second_distance=plane_distance_squared(plane,second);
            if(first_distance>radius_squared&&second_distance>radius_squared)continue;
            marked[ci][pi]=true;
            if(dot(sub(old_first,plane.point),n)<0||dot(sub(old_second,plane.point),n)<0)continue;
            bool inside=true;
            for(unsigned edge=0;edge<polygon.vertex_count();++edge) {
                const auto& a=chunk.vertices.at(w[5+edge]);const auto& b=chunk.vertices.at(w[6+edge]);
                const auto edge_normal=cross(sub(b,a),n);
                if(dot(edge_normal,sub(first,a))>0||dot(edge_normal,sub(second,a))>0){inside=false;break;}
            }
            if(!inside)continue;
            const auto point=second_distance<=first_distance?second:first;
            Line intersection;
            if(!intersect_planes(plane,{point,axis},intersection))continue;
            const auto projection=project_line(intersection,point);
            if(projection.distance_squared<=radius_squared)
                return contact(ci,pi,sub(point,projection.point),projection.distance_squared);
        }
    }
    const Line capsule_line{first,axis};
    for(unsigned ci=0;ci<candidates.size();++ci) {
        const auto& chunk=chunks[candidates[ci]];
        for(unsigned pi=0;pi<chunk.polygons.size();++pi)if(marked[ci][pi]) {
            const auto& polygon=chunk.polygons[pi];const auto& w=polygon.words;
            for(unsigned edge=0;edge<polygon.vertex_count();++edge) {
                const auto& a=chunk.vertices.at(w[5+edge]);const auto& b=chunk.vertices.at(w[6+edge]);
                const Line line{a,sub(b,a)};
                if(parallel(line.direction,axis)) {
                    const auto projection=project_line(line,first);
                    if(projection.distance_squared>radius_squared)continue;
                    const float second_parameter=project_line(line,second).parameter;
                    if(projection.parameter<0&&second_parameter<0)continue;
                    if(projection.parameter>1&&second_parameter>1)continue;
                    return contact(ci,pi,sub(first,projection.point),projection.distance_squared);
                }
                const auto closest=closest_lines(line,capsule_line);
                if(closest.distance_squared>radius_squared)continue;
                const unsigned region=unsigned(in_segment(closest.first_parameter))|
                                      (unsigned(in_segment(closest.second_parameter))<<1);
                if(region==3)return contact(ci,pi,sub(closest.second,closest.first),closest.distance_squared);
                if(region==2) {
                    const auto point=closest.first_parameter<0?a:b;
                    const auto projection=project_line(capsule_line,point);
                    if(projection.distance_squared<=radius_squared&&in_segment(projection.parameter))
                        return contact(ci,pi,sub(projection.point,point),projection.distance_squared);
                } else if(region==0) {
                    const auto point=closest.second_parameter<0?first:second;Float4 intersection;
                    if(!intersect_line_plane({point,axis},line,intersection))continue;
                    if(!in_segment(line_parameter(line,intersection)))continue;
                    const float distance=distance_squared(intersection,point);
                    if(distance<=radius_squared)return contact(ci,pi,sub(point,intersection),distance);
                }
            }
        }
    }
    return {};
}
MapContact CollisionMap::move_contact(const Float4& previous,const Float4& desired,
                                     std::span<const CollisionShape> shapes,const MapCollisionSettings& settings) const {
    using namespace collision_geometry;Rounding rounding;
    require(std::isfinite(settings.step)&&settings.step>0&&std::isfinite(settings.floor_reach)&&settings.floor_reach>=0,
            "Invalid map contact distances");
    require(settings.normal_threshold>=0&&settings.normal_threshold<=16384&&settings.wall_iterations<=64,
            "Invalid map contact settings");
    for(const auto& v:{previous,desired})for(unsigned i=0;i<3;++i)require(std::isfinite(v[i]),"Non-finite map contact point");
    for(const auto& shape:shapes) {
        const auto type=shape.flags&7;
        require(type==0||type==2,"Unrecovered map contact shape");
        require(!(type==0&&(shape.flags&0x20)),"Unrecovered point wall contact");
        for(unsigned i=0;i<3;++i)require(std::isfinite(shape.geometry.first[i]),"Non-finite shape point");
        if(type==2) {
            for(unsigned i=0;i<3;++i)require(std::isfinite(shape.geometry.second[i]),"Non-finite shape endpoint");
            require(std::isfinite(shape.geometry.radius)&&shape.geometry.radius>0&&
                    dot(sub(shape.geometry.second,shape.geometry.first),sub(shape.geometry.second,shape.geometry.first))>0,
                    "Invalid map capsule");
        }
    }
    MapContact result;Float4 step_correction{};
    auto single_step=[&](const Float4& start,const Float4& end) {
        Float4 position=end;bool hit=false;
        auto apply=[&](const SurfaceContact& contact,unsigned kind) {
            if(!contact.hit)return false;
            hit=true;result.hit=true;result.flags|=1u<<kind;
            if(result.materials[kind]==0)result.materials[kind]=contact.material;
            result.normals[kind]=contact.normal;position=add(position,contact.correction);return true;
        };
        // Original 0x187498 processes all walls, then ceilings, then floors.
        for(const auto& shape:shapes)if(shape.flags&0x20)
            for(unsigned i=0;i<settings.wall_iterations;++i)
                if(!apply(capsule_wall_contact(start,position,shape.geometry,settings.normal_threshold,settings.disabled_mask),2))break;
        for(unsigned kind:{1u,0u})for(const auto& shape:shapes)if(shape.flags&(kind?0x10:8)) {
            auto offset=shape.geometry.first;
            if((shape.flags&7)==2) {
                const auto& second=shape.geometry.second;
                if(kind?second[1]<=offset[1]:offset[1]<=second[1])offset=second;
            }
            apply(vertical_contact(add(start,offset),add(position,offset),kind,settings.floor_reach,
                                   settings.normal_threshold,settings.disabled_mask),kind);
        }
        if(hit) {
            step_correction=sub(position,end);step_correction[3]=1;position=add(end,step_correction);
        }
        return position;
    };
    const auto delta=sub(desired,previous);const float length=std::sqrt(dot(delta,delta));
    require(std::isfinite(length)&&length/settings.step<=65536,"Map contact subdivision bound");
    Float4 position;
    if(length<=settings.step) {
        single_step(previous,desired);
        if(result.hit)result.correction=step_correction;
        return result;
    }
    else {
        auto increment=normalized(delta);for(auto& f:increment)f=f*settings.step;
        position=previous;float distance=settings.step;
        while(distance<length) {
            position=single_step(position,add(position,increment));distance=distance+settings.step;
        }
        auto remainder=normalized(delta);const float amount=length-(distance-settings.step);
        for(auto& f:remainder)f=f*amount;
        position=single_step(position,add(position,remainder));
    }
    if(result.hit){result.correction=sub(position,desired);result.correction[3]=1;}
    return result;
}
ActorContact capsule_pair_contact(const Float4& previous,const Float4& desired,
                                  const CollisionCapsule& moving,const Float4& other_position,
                                  const CollisionCapsule& other,float reach,float threshold) {
    using namespace collision_geometry;Rounding rounding;
    require(std::isfinite(reach)&&reach>=0&&std::isfinite(threshold)&&threshold>=0&&threshold<=1,"Invalid actor contact settings");
    for(const auto& v:{previous,desired,other_position,moving.first,moving.second,other.first,other.second})
        for(unsigned i=0;i<3;++i)require(std::isfinite(v[i]),"Non-finite actor contact point");
    for(const auto& c:{moving,other})require(std::isfinite(c.radius)&&c.radius>0&&
        dot(sub(c.second,c.first),sub(c.second,c.first))>0,"Invalid actor capsule");
    const auto first=add(desired,moving.first),second=add(desired,moving.second);
    const auto old_first=add(previous,moving.first),old_second=add(previous,moving.second);
    const auto other_first=add(other_position,other.first),other_second=add(other_position,other.second);
    // 0x18d098 tests each endpoint against the other capsule's end planes,
    // using the other radius alone. The horizontal stage uses the radius sum.
    auto endpoint=[&](const Float4& old,const Float4& point,const Plane& plane) {
        ActorContact result;
        if(dot(sub(old,plane.point),plane.normal)<0)return result;
        Float4 intersection{};unsigned kind;
        if(plane.normal[1]< -threshold) {
            auto probe=point;probe[1]=probe[1]+reach;probe[3]=1;
            if(dot(sub(probe,plane.point),plane.normal)>0)return result;
            if(!intersect_line_plane(plane,{point,{0,1,0,0}},intersection)||
                distance_squared(plane.point,intersection)>other.radius*other.radius)return result;
            result.correction[1]=(intersection[1]-0.001f)-point[1];kind=0;
        } else {
            if(dot(sub(point,plane.point),plane.normal)>0)return result;
            if(!intersect_line_plane(plane,{old,sub(point,old)},intersection)||
                distance_squared(plane.point,intersection)>other.radius*other.radius)return result;
            for(unsigned i=0;i<3;++i)result.correction[i]=(intersection[i]+plane.normal[i]*0.001f)-point[i];
            kind=plane.normal[1]>threshold?1:2;
        }
        result.hit=true;result.flags=1u<<kind;result.normals[kind]=plane.normal;return result;
    };
    for(const auto& plane:{Plane{other_first,normalized(sub(other.first,other.second))},
                          Plane{other_second,normalized(sub(other.second,other.first))}}) {
        auto result=endpoint(old_first,first,plane);if(result.hit)return result;
        result=endpoint(old_second,second,plane);if(result.hit)return result;
    }
    const float radius=moving.radius+other.radius,radius_squared=radius*radius;
    const Line line{other_first,sub(other_second,other_first)},capsule{first,sub(second,first)};
    auto horizontal=[&](Float4 delta,float distance) {
        ActorContact result;result.hit=true;delta[1]=0;const auto direction=normalized(delta);
        const float amount=(radius-std::sqrt(distance))+0.001f;
        for(unsigned i=0;i<3;++i)result.correction[i]=direction[i]*amount;
        return result;
    };
    if(parallel(line.direction,capsule.direction)) {
        const auto projection=project_line(line,first);
        if(projection.distance_squared>radius_squared)return {};
        const float t=project_line(line,second).parameter;
        if((projection.parameter<0&&t<0)||(projection.parameter>1&&t>1))return {};
        return horizontal(sub(first,projection.point),projection.distance_squared);
    }
    const auto closest=closest_lines(line,capsule);
    if(closest.distance_squared>radius_squared)return {};
    const unsigned region=unsigned(in_segment(closest.first_parameter))|(unsigned(in_segment(closest.second_parameter))<<1);
    if(region==3)return horizontal(sub(closest.second,closest.first),closest.distance_squared);
    if(region==2) {
        const auto point=closest.first_parameter<0?other_first:other_second;
        const auto projection=project_line(capsule,point);
        if(projection.distance_squared>radius_squared||!in_segment(projection.parameter))return {};
        return horizontal(sub(projection.point,point),projection.distance_squared);
    }
    if(region==0) {
        const auto point=closest.second_parameter<0?first:second;Float4 intersection{};
        if(!intersect_line_plane({point,capsule.direction},line,intersection))return {};
        const unsigned axis=std::fabs(double(line.direction[0]))>double(0.1f)?0:(std::fabs(double(line.direction[1]))>double(0.1f)?1:2);
        require(line.direction[axis]!=0,"Degenerate actor contact parameter");
        const float parameter=(intersection[axis]-line.point[axis])/line.direction[axis];
        if(!in_segment(parameter))return {};
        const float distance=distance_squared(intersection,point);
        if(distance<=radius_squared)return horizontal(sub(point,intersection),distance);
    }
    return {};
}
bool resolve_collision(const CollisionMap& map,std::span<CollisionBody> bodies,std::size_t moving,
                       Float4& desired,const MapCollisionSettings& settings,float actor_threshold,std::int32_t priority_bias) {
    using namespace collision_geometry;Rounding rounding;
    require(bodies.size()<=256&&moving<bodies.size(),"Invalid collision registry");
    for(const auto& body:bodies) {
        require(std::isfinite(body.broad_radius)&&body.broad_radius>=0,"Invalid collision broad radius");
        for(const auto& v:{body.position,body.previous})for(unsigned i=0;i<3;++i)
            require(std::isfinite(v[i]),"Non-finite collision body position");
    }
    std::function<bool(std::size_t,Float4&,unsigned)> resolve;
    auto merge_map=[](MapContact& into,const MapContact& from) {
        into.flags|=from.flags;
        for(unsigned kind=0;kind<3;++kind) {
            if(!into.materials[kind])into.materials[kind]=from.materials[kind];
            if(from.flags&(1u<<kind))into.normals[kind]=from.normals[kind];
        }
        into.correction=from.correction;
    };
    auto pair=[&](std::size_t a,std::size_t b,const Float4& end) {
        auto& body=bodies[a];const auto& other=bodies[b];Float4 position=end;bool hit=false;
        for(const auto& shape:body.shapes)if(shape.flags&0x40)for(const auto& target:other.shapes) {
            const auto type=shape.flags&7,other_type=target.flags&7;
            // Original dispatch has no point target handler for these types.
            if(other_type==0&&(type==0||type==1||type==2))continue;
            require(type==2&&other_type==2,"Unrecovered dynamic shape pair");
            const auto contact=capsule_pair_contact(body.previous,position,shape.geometry,other.position,
                                                    target.geometry,body.floor_reach,actor_threshold);
            if(!contact.hit)continue;
            position=add(position,contact.correction);body.contact.flags|=contact.flags|8;
            for(unsigned kind=0;kind<3;++kind)if(contact.flags&(1u<<kind))body.contact.normals[kind]=contact.normals[kind];
            if(body.other_body<0)body.other_body=std::int32_t(b);hit=true;
        }
        if(hit){body.contact.correction=sub(position,end);body.contact.correction[3]=1;}
        return hit;
    };
    auto dynamic=[&](std::size_t index,const Float4& end,unsigned depth) {
        auto& body=bodies[index];bool hit=false;
        if(body.flags&2)return hit;
        for(std::size_t other_index=0;other_index<bodies.size();++other_index) {
            auto& other=bodies[other_index];
            if(!other.registered||other.id==body.id||(other.flags&2))continue;
            const auto delta=sub(end,other.position);const float radius=body.broad_radius+other.broad_radius;
            if(dot(delta,delta)>radius*radius||!pair(index,other_index,end))continue;
            hit=true;
            if(body.priority>=other.priority+std::uint32_t(priority_bias))continue;
            const auto old_id=other.id;other.id=body.id;
            auto push=body.contact.correction;for(unsigned i=0;i<3;++i)push[i]=-push[i];
            auto destination=add(other.position,push);bool pushed_hit;
            try {pushed_hit=resolve(other_index,destination,depth+1);}
            catch(...){other.id=old_id;throw;}
            const bool blocked=pushed_hit&&((other.contact.flags&0xc)||
                ((other.contact.flags&1)&&((body.contact.flags&9)==9)));
            if(!blocked) {
                other.position=destination;
                for(unsigned i=0;i<3;++i)body.contact.correction[i]=0;
            }
            other.id=old_id;
        }
        return hit;
    };
    resolve=[&](std::size_t index,Float4& end,unsigned depth) {
        require(depth<=256,"Collision push recursion bound");auto& body=bodies[index];
        body.other_body=-1;body.contact.flags&=~15u;body.contact.materials={};body.contact.hit=false;
        auto position=end;bool hit=false;
        for(unsigned pass=0;pass<2;++pass) {
            if(!dynamic(index,position,depth))break;
            hit=true;position=add(position,body.contact.correction);
        }
        auto map_settings=settings;map_settings.floor_reach=body.floor_reach;
        for(unsigned pass=0;pass<2;++pass) {
            const auto contact=map.move_contact(body.previous,position,body.shapes,map_settings);
            if(!contact.hit)break;
            hit=true;merge_map(body.contact,contact);position=add(position,body.contact.correction);
        }
        body.contact.hit=hit;
        if(hit) {
            body.contact.correction=sub(position,end);body.contact.correction[3]=1;
            if(body.flags&1){end=position;end[3]=1;}
        }
        return hit;
    };
    return resolve(moving,desired,0);
}
}
