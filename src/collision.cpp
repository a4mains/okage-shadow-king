#include "okage/collision.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
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
}
