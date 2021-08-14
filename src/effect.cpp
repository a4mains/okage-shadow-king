#include "okage/effect.hpp"
#include "okage/camera.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <set>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
struct Reader {
    Bytes b;
    void range(std::size_t p,std::size_t n)const{require(p<=b.size()&&n<=b.size()-p,"EFD record outside input");}
    std::uint32_t word(std::size_t p)const{range(p,4);return std::uint32_t(b[p])|(std::uint32_t(b[p+1])<<8)|(std::uint32_t(b[p+2])<<16)|(std::uint32_t(b[p+3])<<24);}
    float number(std::size_t p)const{const auto f=__builtin_bit_cast(float, (word(p)));require(std::isfinite(f),"Non-finite EFD value");return f;}
    Float4 vector(std::size_t p)const{return {number(p),number(p+4),number(p+8),number(p+12)};}
};
struct Rounding {
    int old=std::fegetround();
    Rounding(){require(!std::fesetround(FE_TOWARDZERO),"Cannot set effect rounding");}
    ~Rounding(){std::fesetround(old);}
};
unsigned angle(float t){return (static_cast<std::uint32_t>(static_cast<std::int32_t>(t*32768.0f))>>1)&16383;}
}
std::vector<EffectDefinition> read_efd(Bytes bytes) {
    Reader r{bytes};r.range(0,16);require(r.word(0)==0x20444645,"Invalid EFD signature");
    const auto count=r.word(4);require(count<=128,"EFD exceeds retail template limit");
    std::vector<EffectDefinition> result;std::set<std::uint32_t> ids;
    std::size_t p=16;
    for(unsigned i=0;i<count;++i) {
        r.range(p,48);EffectDefinition d;
        d.id=r.word(p);d.duration=r.word(p+4);d.flags=r.word(p+8);
        require(d.id<128&&ids.insert(d.id).second,"Invalid or duplicate EFD ID");
        const auto n=r.word(p+16);require(n<=(bytes.size()-p-48)/116,"EFD element count outside input");
        require(r.word(p+20)==p+48,"EFD element pointer does not match sequential records");
        d.parameters=r.vector(p+32);
        for(unsigned j=0;j<n;++j) {
            const auto a=p+48+j*116;EffectElement e;
            e.flags=r.word(a);e.id=r.word(a+4);e.shape=r.word(a+8);
            e.primitive_flags=r.word(a+12);e.channels=r.word(a+16);e.program=r.word(a+20);
            e.texture=__builtin_bit_cast(std::int32_t, (r.word(a+24)));e.change_time=r.number(a+28);e.next_program=r.word(a+32);
            for(unsigned k=0;k<5;++k)e.values[k]=r.vector(a+36+k*16);
            d.elements.push_back(e);
        }
        p+=48+n*116;result.push_back(std::move(d));
    }
    require(p==bytes.size(),"Trailing EFD data");return result;
}
std::vector<EffectDefinition> resolve_effect_references(std::vector<EffectDefinition> bank) {
    //0x10b5e8 copies the source's whole job header, sharing its element table.
    // Owning an independent C++ definition preserves those immutable elements.
    for(auto& target:bank) {
        if(!(target.flags&0x800))continue;
        require(target.elements.size()==1,"Unrecovered EFD reference extent");
        const auto reference=target.elements.front();
        require(!(reference.channels&~31u),"Unrecovered EFD reference channels");
        const auto source=std::find_if(bank.begin(),bank.end(),[&](const auto& d){return d.id==reference.id;});
        require(source!=bank.end()&&!(source->flags&0x800),"Unresolved EFD reference order or target");
        const auto lookup=target.id;
        target=*source;target.runtime_id=source->runtime_id.value_or(source->id);target.id=lookup;
        const std::array<Float4*,5> channels={&target.color,&target.rotation,&target.position,&target.scale,&target.parameters};
        for(unsigned i=0;i<5;++i)if(reference.channels&(1u<<i))*channels[i]=reference.values[i];
    }
    return bank;
}
MapEffect::MapEffect(const EffectDefinition& d,const Float4& p,const Float4& r,
                     const Float4& s,const EffectTextureSizes& sizes,
                     const std::function<std::uint32_t()>& random,EffectStart start):
    id(d.runtime_id.value_or(d.id)),flags(d.flags&~0x4000u),duration(d.duration),position(p),rotation(r),scale(s),color(d.color),
    parameters(d.parameters),texture_sizes_(sizes) {
    require(!(d.flags&0x800),"Unresolved effect reference");
    require((id==25&&d.elements.size()==8)||(id==34&&d.elements.size()==14)||(id==19&&d.elements.size()==9)||
            ((id==17||id==33)&&d.elements.size()==9&&start==EffectStart::FirstFrame)||
            (id==115&&d.elements.size()==5&&start==EffectStart::FirstFrame)||
            (id==91&&d.elements.size()==4)||(id==16&&d.elements.size()==3),
            "Unrecovered map effect template");
    require(duration>0&&duration<=0x7fffffff,"Invalid map effect duration");
    Rounding rounding;inverse_duration=1.0f/float(duration);
    const std::array<Float4,5> defaults={Float4{1,1,1,1},Float4{},Float4{},
                                       Float4{1,1,1,1},Float4{0,1,1,0}};
    for(const auto& source:d.elements) {
        require(!(source.channels&~31u),"Unrecovered map effect channel modifiers");
        MapEffectElement e;
        const auto channel=[&](unsigned i){return (source.channels&(1u<<i))?source.values[i]:defaults[i];};
        e.position=channel(2);e.rotation=channel(1);e.scale=channel(3);e.color=channel(0);e.interval=channel(4);
        e.id=source.id;e.shape=source.shape;e.flags=source.flags;e.primitive_flags=source.primitive_flags;
        e.program=source.program|((source.primitive_flags&0x300u)<<22);
        e.change_time=source.change_time;e.next_program=source.next_program;
        e.rendered={e.position,e.rotation,e.scale,e.color};
        unsigned texture;
        switch(e.shape){case 24:texture=20;break;case 29:texture=5;break;
                        case 28:texture=4;break;case 31:texture=17;break;
                        case 59:require(id==33,"Unrecovered effect shape59 owner");texture=25;break;
                        case 25:case 40:case 32:
                            require(id==33||id==115||id==91,"Unrecovered untextured effect owner");
                            texture=0xffffffffu;break;
                        default:throw std::runtime_error("Unrecovered map effect shape");}
        if(texture==0xffffffffu){e.texture=texture;e.uv={0,0,65535,65535};}
        else set_texture(e,texture);
        if(id==33&&e.shape==25)e.ring_y.assign(30,-192);
        if((id==91||id==16)&&e.shape==28) {
            e.quad_positions={{{32,-64,0},{-32,-64,0},{32,0,0},{-32,0,0}}};
            const auto width=std::uint16_t(sizes[4][0]*32),height=std::uint16_t((sizes[4][1]+1)*32);
            e.quad_uv={{{width,0},{0,0},{width,height},{0,height}}};
        }
        elements.push_back(e);
    }
    phase=start==EffectStart::RandomPhase?random()%duration:1;
}
void MapEffect::set_texture(MapEffectElement& e,unsigned texture) {
    require(texture<texture_sizes_.size(),"Invalid logical effect texture");
    const auto size=texture_sizes_[texture];
    require(size[0]>0&&size[1]>0&&size[0]<=4096&&size[1]<=4096,"Missing effect texture dimensions");
    e.texture=texture;e.uv={0,0,std::uint16_t(size[0]-1),std::uint16_t(size[1]-1)};
}
void MapEffect::set_parameter(unsigned selector,const Float4& payload) {
    require(selector<=0xfff,"Invalid effect parameter selector");
    if(selector<0xf00)parameters.fill(float(selector));
    else {
        Float4* target=nullptr;
        switch(selector){case 0xfff:target=&rotation;break;case 0xffe:target=&scale;break;case 0xffd:target=&color;break;}
        // Other reserved selectors consume their payload but perform no write.
        if(target)*target={payload[0],payload[1],payload[2],1};
    }
}
std::vector<MapEffectDraw> map_effect_draws(const MapEffect& effect,const Matrix4& view,
        const Float4& camera,float far_clip,const SineTable& sine,const Float4* light_tint) {
    if(effect.flags&0x4300)return {};
    require((effect.id==25&&effect.elements.size()==8)||((effect.id==17||effect.id==19)&&effect.elements.size()==9)||
            (effect.id==91&&effect.elements.size()==4),"Unrecovered map effect geometry template");
    require(!(effect.flags&0x400),"Deferred map effect rendering is unrecovered");
    require(std::isfinite(far_clip)&&far_clip>0,"Invalid effect far clip");
    Rounding rounding;
    const auto job=effect_matrix(effect.position,effect.rotation,effect.scale,sine,false);
    float fade=1;
    if(effect.flags&0x1000) {
        float z=view[0][2]*effect.position[0];z+=view[1][2]*effect.position[1];
        z+=view[2][2]*effect.position[2];z+=view[3][2];
        if(far_clip<z+40)fade=std::max(0.0f,(far_clip-z)*.025f);
    }
    std::vector<MapEffectDraw> result;
    for(const auto& e:effect.elements) {
        require((e.primitive_flags&255)==1||((effect.id==17||effect.id==19)&&e.primitive_flags==0x41),"Unrecovered map effect material");
        require(!(e.flags&0x400000)||light_tint,"Effect requires owned field light tint");
        require(e.uv[0]==0&&e.uv[1]==0&&!e.render_flags,"Unrecovered map effect texture region");
        auto rotation=e.rendered[1];
        switch(e.flags&15){case 0:break;case 1:
            rotation[0]=(1-camera[0])-effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];rotation[3]=camera[3];break;
            case 2:
            require(effect.id==91&&e.shape==40,"Unrecovered quarter-turn billboard owner");
            rotation[0]=((1-camera[0])+.25f)-effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];rotation[3]=camera[3];break;
            case 8:
            require(effect.id==91&&e.shape==28,"Unrecovered horizontal billboard owner");
            rotation[0]-=effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];break;
            default:throw std::runtime_error("Unrecovered map effect orientation");}
        auto matrix=effect_matrix(e.rendered[0],rotation,e.rendered[2],sine,true);
        matrix=compose_pose(job,std::span(&matrix,1))[0];
        matrix=compose_pose(view,std::span(&matrix,1))[0];
        Float4 color{};for(unsigned i=0;i<4;++i){
            color[i]=e.rendered[3][i]*effect.color[i];
            if(e.flags&0x400000)color[i]*=(*light_tint)[i];
        }
        color[3]*=fade;
        MapEffectDraw draw{matrix,color,e.texture,e.uv,{},(e.primitive_flags&255)==1};
        const Float4 white{255,255,255,255},clear{255,255,255,0};
        if(effect.id==91) {
            if(e.shape==28) {
                require(e.texture==4,"Unrecovered flame texture");
                // Stored geometry already has original strip order B,A,C,D.
                for(unsigned v:{0u,1u,2u,1u,2u,3u}) {
                    const auto& p=e.quad_positions[v];const auto& uv=e.quad_uv[v];
                    draw.vertices.push_back({{p[0]/64.f,p[1]/64.f,p[2]/64.f,1},white,
                                             {uv[0]/32768.f,uv[1]/32768.f,0,0}});
                }
            } else if(e.shape==40) {
                require(e.texture==0xffffffffu,"Unrecovered glow texture");
                //0x10fca8 emits sixteen independent center/next/current triangles.
                const auto point=[&](unsigned i){const auto a=(i%16)*1024;
                    return Float4{std::round(sine[(a+4096)&16383]*64)/64,0,
                                  std::round(sine[a]*64)/64,1};};
                for(unsigned i=0;i<16;++i) {
                    draw.vertices.push_back({{0,0,0,1},white,{}});
                    draw.vertices.push_back({point(i+1),clear,{}});
                    draw.vertices.push_back({point(i),clear,{}});
                }
            } else throw std::runtime_error("Unrecovered torch geometry");
            result.push_back(std::move(draw));continue;
        }
        const auto base=effect.texture_sizes().at(e.shape==24?20:e.shape==31?17:5);
        const auto current=effect.texture_sizes().at(e.texture);
        const bool replace=(e.program>>30)==1;
        //0x1122e8 constructs T using height+1.0x1055d8 rescales the
        // original packed UVs with truncated5-bit fixed ratios on replacement.
        auto uv=[&](float u,float v){
            int s=int(u*float(base[0])*32),t=int(v*float(base[1]+1)*32);
            require(s>=0&&s<=32767&&t>=0&&t<=32767,"Unrecovered effect UV packing range");
            if(replace){s=(s*int((current[0]<<5)/base[0]))>>5;t=(t*int((current[1]<<5)/base[1]))>>5;}
            return Float4{float(std::uint16_t(s))/32768,float(std::uint16_t(t))/32768,0,0};
        };
        auto quad=[&](const std::array<Float4,4>& p,const std::array<Float4,4>& c,const std::array<Float4,4>& uv) {
            // Original strip order B,A,C,D, hence the shared A-C diagonal.
            for(unsigned i:{1u,0u,2u,0u,2u,3u})draw.vertices.push_back({p[i],c[i],uv[i]});
        };
        if(e.shape==24) {
            auto point=[&](unsigned i,float y) {
                //0x113d88 packs nearest signed1/64 positions. Source W is a
                // separate byte tag; VU rendering supplies homogeneous1.
                return Float4{std::round(sine[(i*1024+4096)&16383]*64)/64,y,
                              std::round(sine[(i*1024)&16383]*64)/64,1};
            };
            for(unsigned i=0;i<16;++i){
                const float a=float(i)/16,b=float(i+1)/16;
                quad({point(i,-3),point(i,0),point(i+1,0),point(i+1,-3)},
                    {clear,white,white,clear},
                    {uv(a,0),uv(a,1),uv(b,1),uv(b,0)});
            }
        } else if(e.shape==29||((effect.id==17||effect.id==19)&&e.shape==31)) {
            const float bottom=e.shape==31?-.5f:-1,top=e.shape==31?.5f:0;
            quad({Float4{-.5f,bottom,0,1},Float4{.5f,bottom,0,1},Float4{.5f,top,0,1},Float4{-.5f,top,0,1}},
                 {white,white,white,white},
                 {uv(0,0),uv(1,0),uv(1,1),uv(0,1)});
        } else throw std::runtime_error("Unrecovered map effect primitive geometry");
        result.push_back(std::move(draw));
    }
    return result;
}
Float4 effect_light_tint(const Float4& base,const Float4& ambient,const Float4& light) {
    Rounding rounding;
    Float4 result{};float maximum=-1;
    for(unsigned i=0;i<3;++i) {
        require(std::isfinite(base[i])&&std::isfinite(ambient[i])&&std::isfinite(light[i]),"Invalid effect light input");
        result[i]=((base[i]+ambient[i])+light[i])*__builtin_bit_cast(float, (0x3eaaaa3bu));
        maximum=std::max(maximum,result[i]);
    }
    require(std::isfinite(maximum)&&maximum>0,"Unrecovered degenerate effect lighting");
    for(unsigned i=0;i<3;++i)result[i]/=maximum;
    result[3]=1;return result;
}
void MapEffect::tick(const std::function<std::uint32_t()>& random,bool clock_enabled,const Float4* eye) {
    if(!clock_enabled)return;
    require(!(flags&0x100),"Cannot update a removed map effect");
    require(id==25||id==17||id==19||id==33||id==115||id==91||id==16,"Unrecovered map effect update program");
    if(id==91){require(eye,"Torch effect requires the current camera position");
        for(float f:*eye)require(std::isfinite(f),"Non-finite torch camera position");}
    Rounding rounding;
    if(phase>duration){if(!(flags&0x8000)){request_removal();return;}phase-=duration;}
    const float time=float(phase)*inverse_duration;
    for(auto& e:elements) {
        require(e.interval[0]<e.interval[1],"Invalid map effect interval");
        float t=time<e.interval[0]?0:time>e.interval[1]?1:(time-e.interval[0])/(e.interval[1]-e.interval[0]);
        //0x105a40 snapshots the current program before storing its successor.
        // The successor therefore starts on the following update, even when
        // the threshold was crossed on the first tick.
        auto program=e.program;
        if(e.change_time<t) {
            e.program=e.next_program;
            if(e.flags&0x20000)t-=e.change_time;
            if(!(e.render_flags&1)) {
                if(e.flags&0x40000)e.position=e.rendered[0];
                if(e.flags&0x80000)e.scale=e.rendered[2];
                e.render_flags|=1;
            }
        }
        if(!(flags&0x20)) {
            const auto mode=program>>30;
            require(mode!=3&&!(e.flags&0x800000),"Unrecovered map effect texture mode");
            if(mode)set_texture(e,unsigned(e.interval[2]));
            e.interval[2]=0;
        }
        if((e.flags&0x2000)&&e.interval[2]<t)program&=0xc0ffffff;
        const auto timing=(program>>24)&63;
        require(timing==0||timing==1||timing==5||timing==7||timing==17,"Unrecovered map effect time program");
        if(timing==5) {
            const float start=float((int(e.id)-1)>>2)*e.interval[3];
            const int cycles=int((1.0f-start)*450.0f);
            const float end=float(cycles)/450.0f+start;
            require(cycles>0&&start<end,"Unrecovered flame cycle interval");
            float local=t<start?0:t>end?1:(t-start)/(end-start);
            local*=float(cycles);t=local-float(int(local));
        }
        if(timing==1) {
            //0x105f80 ->0x103f88(1,handle,elementID,tag2,axisY/mode0).
            // Shape25 has thirty tag2 vertices; preserve their packed Y values
            // as well as the RNG advancement needed by later elements.
            require(id==33&&e.shape==25&&e.ring_y.size()==30,"Unrecovered effect deformation geometry");
            for(auto& y:e.ring_y)y=std::int16_t(std::min(0,int(random()%50)-25-64));
        }
        if(timing==17) {
            //0x105518 links geometry to the element with the preceding ID.
            // Template17's shape31 packets are identical immutable quads.
            const auto previous=std::find_if(elements.begin(),elements.end(),[&](const auto& p){return p.id+1==e.id;});
            require(id==17&&previous!=elements.end()&&previous->shape==31&&e.shape==31&&
                    previous->texture==e.texture,"Unrecovered effect geometry sharing");
        }
        if(timing==7){t=t*3.0f+e.interval[3];t-=float(int(t));}
        if((e.flags&0x1000)&&e.interval[2]<t)program&=0xfffc0fff;
        auto& p=e.rendered[0];auto& r=e.rendered[1];auto& s=e.rendered[2];auto& c=e.rendered[3];
        switch((program>>12)&63) {
        case 0:break;
        case 1:
            for(unsigned i=0;i<3;++i)p[i]=float(int(random()%40)-20)*parameters[i]+e.position[i];
            break;
        case 17:p[1]=e.position[1]+e.position[3]*t;break;
        case 10: {
            //0x104ba8 maps the original quad UVs into a32x64 atlas cell.
            require((id==91||id==16)&&e.shape==28,"Unrecovered effect atlas geometry");
            const auto column=((int(e.id)-1)>>2)*32;
            const auto width=texture_sizes_[e.texture][0],height=texture_sizes_[e.texture][1];
            const int xstep=int(width*32)/32,ystep=int(height*32)/64;
            require(xstep>0&&ystep>0,"Invalid torch atlas size");
            const int right=((int(width*32)<<12)/xstep)>>7;
            const int bottom=((int((height+1)*32)<<12)/ystep)>>7;
            const auto left=std::uint16_t(column*32),rgt=std::uint16_t(right+column*32);
            e.quad_uv={{{rgt,0},{left,0},{rgt,std::uint16_t(bottom)},{left,std::uint16_t(bottom)}}};
            break;
        }
        case 49: {
            //0x103c88 aims from the camera toward the job, then position49
            // rotates a local negative-Z offset. It does not add job position.
            require(eye,"Effect glow requires camera position");
            static const auto angles=make_atan_table();
            const float dx=position[0]-(*eye)[0],dy=position[1]-(*eye)[1],dz=position[2]-(*eye)[2];
            const float horizontal=std::sqrt(dx*dx+dz*dz);
            const Float4 look{camera_angle(-dy,horizontal,angles),camera_angle(dx,dz,angles),0,1};
            const auto matrix=effect_matrix({},look,{1,1,1,1},sine_,true);
            const float depth=rotation[2]<0?rotation[2]:-5;
            for(unsigned i=0;i<4;++i){float v=matrix[0][i]*0.0f;v+=matrix[1][i]*0.0f;
                v+=matrix[2][i]*depth;v+=matrix[3][i]*0.0f;p[i]=v;}
            break;
        }
        case 12:
            if(flags&0x20) {
                for(unsigned i=0;i<3;++i)p[i]=(e.position[i]*t)*parameters[i];
            } else {
                for(unsigned i=0;i<3;++i)e.position[i]=(float(int(random()%40)-20)*.2f)*parameters[i]+e.position[i];
            }
            break;
        case 15:
            // 0x10732c: integrate per update while local time increases;
            // at a wrap/equal time reset XYZ with three shared RNG draws.
            if(e.interval[2]<t) {
                for(unsigned i=0;i<3;++i)p[i]+=parameters[i];
            } else {
                for(unsigned i=0;i<3;++i)p[i]=float(int(random()%40)-20)*.02f+e.position[i];
            }
            break;
        case 42:
            if(e.interval[2]<t) {
                const auto theta=angle(t*e.position[3]+e.position[2]);
                p[0]=e.position[0]*sine_[(theta+4096)&16383];
                p[1]=e.position[1]+sine_[angle((t*3.0f)*e.position[3])];
                p[2]=e.position[0]*sine_[theta];
            } else {
                e.position[1]=float(int(random()%70)-100)*.1f;
                e.position[2]=float(random()%100)*.01f;
                e.position[3]=float(int(random()%100)-50)*.02f;
            }
            break;
        default:throw std::runtime_error("Unrecovered map effect position program");
        }
        if((e.flags&0x4000)&&e.interval[2]<t)program&=0xfffff03f;
        switch((program>>6)&63){case 0:break;
        case 1:{const float cycle=t*e.rotation[3];r[1]=(cycle-float(int(cycle)))+e.rotation[1];break;}
        case 4:r[2]=float(random()%360+360)*.00278f;break;case 11:r[1]=float(random()%360+360)*.00278f;break;
        case 5:
            require((id==91||id==16)&&e.shape==28,"Unrecovered effect flip geometry");
            if(t<e.interval[2]) {
                const int sign=((random()%10+10)&4)?1:-1;
                e.quad_positions={{{std::int16_t(sign*32),-64,0},{std::int16_t(sign*-32),-64,0},
                                  {std::int16_t(sign*32),0,0},{std::int16_t(sign*-32),0,0}}};
            }
            break;
                              default:throw std::runtime_error("Unrecovered map effect rotation program");}
        switch((program>>18)&63){case 0:break;case 1:for(unsigned i=0;i<3;++i)s[i]=(t*e.scale[3]+1.0f)*e.scale[i];break;
        case 2:{const float q=-.1f/t;const float factor=(q < -1?0:q+1)*e.scale[3];
            for(unsigned i=0;i<3;++i)s[i]=factor*e.scale[i];break;}
        case 3:for(unsigned i=0;i<3;++i)s[i]=t*e.scale[i];break;
        case 16:s[1]+=e.scale[3];break;
        case 20:{const float x=e.scale[0],cycle=t*30.0f;
            const float size=((x*.75f+x)-(t*.75f)*x)+((1.0f-(cycle-float(int(cycle))))*x)*.15f;
            s[0]=size;s[1]=size;s[2]=size;break;}
        case 6:{const float q=-e.scale[3]/t;const float factor=q < -1?0:q+1;
            for(unsigned i=0;i<3;++i)s[i]=factor*e.scale[i];break;}
        case 13:s[0]=t*e.scale[0];s[1]=(1.0f-t)*e.scale[1];s[2]=t*e.scale[2];break;
        case 10:{const float envelope=t>.5f?1.0f-(t-.5f)/.5f:t<.2f?t/.2f:1;
            s[0]=envelope*e.scale[0];s[1]=t*e.scale[1];s[2]=envelope*e.scale[2];break;}
        case 25: {
            // 0x109950: duration*.01 precedes multiplication by local time;
            // only X/Y change, with the doubled time added after the base.
            const float cycle=t*(float(duration)*.01f);
            const float wave=sine_[angle(cycle-float(int(cycle)))];
            const float growth=t+t;
            for(unsigned i=0;i<2;++i)s[i]=(wave*parameters[3]+e.scale[i])+growth;
            break;
        }
        default:throw std::runtime_error("Unrecovered map effect scale program");}
        if((e.flags&0x8000)&&e.interval[2]<t)program&=0xffffffc0;
        switch(program&63) {
        case 0:break;
        case 1:c[3]=0;break;
        case 5:
            c[3]=(t==0||t==1)?0:(t>.6f?(1.0f-(t-.6f)/(1.0f-.6f))*e.color[3]:e.color[3]);break;
        case 13: {
            const float cycle=t*e.interval[3],local=cycle-float(int(cycle));
            const float envelope=local>.6f?1.0f-(local-.6f)/.39999998f:local<.4f?local/.4f:1;
            c[3]=e.color[3]-(envelope*e.color[3])*.25f;break;
        }
        case 14:{const float envelope=t>.7f?1.0f-(t-.7f)/.3f:t<.45f?t/.45f:1;
            for(unsigned i=0;i<3;++i)c[i]=envelope*e.color[i];break;}
        case 15:case 16:case 17:case 20: {
            const float head=(program&63)==15?.6f:(program&63)==17?.4f:.1f,tail=((program&63)==15||(program&63)==16)?.8f:(program&63)==17?.7f:.4f;
            float alpha=0;
            if(t!=0&&t!=1){alpha=1;if(t>tail)alpha=1.0f-(t-tail)/(1.0f-tail);else if(t<head)alpha=t/head;alpha*=e.color[3];}
            c[3]=alpha;break;
        }
        default:throw std::runtime_error("Unrecovered map effect color program");
        }
        e.interval[2]=t;
        if(!(flags&0x20)) {
            if(e.flags&0x200)e.program&=0xc0ffffff;
            if(e.flags&0x100)e.program&=0xfffc0fff;
            if(e.flags&0x400)e.program&=0xfffff03f;
            if(e.flags&0x800)e.program&=0xffffffc0;
        }
    }
    flags|=0x20;if(!(flags&0x2000))++phase;
}
BubbleEffect::BubbleEffect(const EffectDefinition& d,int parameter):definition_(d),parameter_(parameter) {
    require(d.id==30&&d.flags==0x8400&&d.duration>0&&d.elements.size()==2,"Unsupported speech-bubble definition");
    require(parameter>=0&&parameter<32,"Unsupported speech-bubble symbol");
    flags=d.flags&~0x4000u;
    for(unsigned i=0;i<2;++i) {
        const auto& e=d.elements[i];auto& s=sprites[i];
        require(e.shape==34&&e.flags==(i?0x241u:1u)&&e.channels==(i?29u:25u)&&
                e.program==(i?0x1961e000u:0x00600000u)&&e.primitive_flags==(i?0x140u:0x40u)&&
                e.texture==int(14+i),"Unsupported speech-bubble element program");
        require(e.values[4][0]==0&&e.values[4][1]==1&&e.change_time>1,"Unsupported speech-bubble time interval");
        s.color=e.values[0];s.rotation={0,0,0,0};s.position=i?e.values[2]:Float4{};
        s.scale=e.values[3];s.damping=s.scale[3];s.previous_time=e.values[4][2];
        s.program=e.program|((e.primitive_flags&0x300)<<22);
        s.texture=14;s.uv={0,0,31,31};
    }
}
void BubbleEffect::tick(const Float4& camera) {
    require(!(flags&0x100),"Cannot update a removed effect");
    for(float f:camera)require(std::isfinite(f)&&std::abs(f)<1000,"Invalid effect camera rotation");
    Rounding rounding;
    if(phase>definition_.duration)phase-=definition_.duration;
    const float time=float(phase)*(1.0f/float(definition_.duration));
    const auto inverse=instance_matrix({0,0,0,1},{1.0f-camera[0],1.0f-camera[1],0,0},{1,1,1,1},sine_);
    for(unsigned i=0;i<2;++i) {
        auto& s=sprites[i];const auto& e=definition_.elements[i];
        if(i) {
            if(!(flags&0x20)) {
                s.texture=parameter_<16?15:16;
                const auto x=std::uint16_t((parameter_&15)*16);s.uv={x,0,std::uint16_t(x+16),15};
                s.program&=0xc0ffffff;
            }
            const auto a=e.values[2];
            Float4 p{sine_[angle(time)]*a[3]+a[0],
                     sine_[angle(time+time-float(int(time+time)))]*a[3]+a[1],0,0};
            for(unsigned r=0;r<3;++r)s.position[r]=inverse[0][r]*p[0]+inverse[1][r]*p[1]+inverse[2][r]*p[2];
        }
        const float cycle=time*20.0f-float(int(time*20.0f));
        s.scale[1]=(1.0f-(sine_[angle(cycle)]*s.damping+s.damping))*e.values[3][1];
        s.damping*=0.9f;s.previous_time=time;
    }
    flags|=0x20;++phase;
}
}
