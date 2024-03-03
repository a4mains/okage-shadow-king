#include "okage/effect.hpp"
#include "okage/camera.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <set>
#include <limits>
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
    float number(std::size_t p)const{const auto f=std::bit_cast<float>(word(p));require(std::isfinite(f),"Non-finite EFD value");return f;}
    Float4 vector(std::size_t p)const{return {number(p),number(p+4),number(p+8),number(p+12)};}
};
struct Rounding {
    int old=std::fegetround();
    Rounding(){require(!std::fesetround(FE_TOWARDZERO),"Cannot set effect rounding");}
    ~Rounding(){std::fesetround(old);}
};
unsigned angle(float t){return (static_cast<std::uint32_t>(static_cast<std::int32_t>(t*32768.0f))>>1)&16383;}
void pack_water(EffectWaterSurface& water,unsigned packet,
                const std::array<std::uint32_t,1024>& heights) {
    require(packet<2,"Invalid water packet selector");
    auto& cache=water.packets[packet];
    for(unsigned i=0;i<1024;++i) {
        cache.y[i]=std::bit_cast<std::int16_t>(std::uint16_t(std::bit_cast<std::int32_t>(heights[i])>>5));
        for(unsigned axis=0;axis<2;++axis)
            cache.uv[i][axis]=std::uint16_t(std::uint32_t(water.coordinates[i][axis])<<5);
    }
    water.selected_packet=packet;
    water.packed_y=cache.y;water.packed_uv=cache.uv;
}
void update_water(EffectWaterSurface& water,const MapEffectElement& e,const Float4& parameters) {
    require(parameters[0]==32&&parameters[1]==32&&parameters[3]==1&&e.interval[3]==0,
            "Unrecovered water dimensions or impulse mode");
    require(parameters[2]>=1&&parameters[2]<=65535,"Invalid water update interval");
    const int right=std::bit_cast<std::int16_t>(e.uv[2]),bottom=std::bit_cast<std::int16_t>(e.uv[3]);
    if(!water.initialized) {
        water.rate=unsigned(parameters[2]);water.period=50;
        for(int y=0;y<32;++y)for(int x=0;x<32;++x)
            water.coordinates[y*32+x]={right*x/31,bottom-bottom*y/31};
        for(auto& packet:water.packets){packet.y=water.packed_y;packet.uv=water.packed_uv;}
        water.selected_packet=1;water.initialized=true;
    }
    ++water.rate_counter;++water.period_counter;
    if(water.rate_counter<water.rate)return;
    water.rate_counter=0;
    auto& source=water.heights[water.active];
    auto& destination=water.heights[water.active^1];
    if(water.period_counter>=water.period) {
        water.period_counter=0;water.period=50;
        source[30*32+16]=std::uint32_t(-15*32);
    }
    const auto signed_word=[](std::uint32_t value){return std::bit_cast<std::int32_t>(value);};
    for(int y=1;y<31;++y)for(int x=1;x<31;++x) {
        const auto at=y*32+x;
        const auto sum=source[at-33]+source[at-32]+source[at-31]+source[at-1]+
                       source[at+1]+source[at+31]+source[at+32]+source[at+33];
        const auto value=std::uint32_t(signed_word(sum)>>2)-destination[at];
        destination[at]=value-std::uint32_t(signed_word(value)>>4);
    }
    water.active^=1;
    for(int y=2;y<31;++y)for(int x=2;x<31;++x) {
        const auto at=y*32+x;
        const auto nw=source[at-33],ne=source[at-31],sw=source[at+31],se=source[at+33];
        const auto dx=std::uint32_t(signed_word(ne+se-nw-sw)>>1)+source[at+1]-source[at-1];
        const auto dy=std::uint32_t(signed_word(sw+se-nw-ne)>>1)+source[at+32]-source[at-32];
        auto v=bottom-(bottom*(y-1)/29+(signed_word(dy)>>6));
        //10cc68..84: ordered comparisons, including signed negative bounds.
        if(v<0)v=0;
        if(v>bottom)v=bottom;
        water.coordinates[at]={right*(x-1)/29+(signed_word(dx)>>6),v};
    }
    pack_water(water,water.active,source);
}
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
            e.texture=std::bit_cast<std::int32_t>(r.word(a+24));e.change_time=r.number(a+28);e.next_program=r.word(a+32);
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
            ((id==17||id==20||id==33)&&d.elements.size()==9&&start==EffectStart::FirstFrame)||
            (id==115&&d.elements.size()==5&&start==EffectStart::FirstFrame)||
            (id==26&&d.elements.size()==2&&start==EffectStart::FirstFrame)||
            (id==6&&d.elements.size()==4&&start==EffectStart::FirstFrame)||
            (id==14&&d.elements.size()==7&&start==EffectStart::FirstFrame)||
            (id==24&&d.elements.size()==8&&start==EffectStart::FirstFrame)||
            (id==38&&d.elements.size()==3&&start==EffectStart::FirstFrame)||
            ((id==22||id==39)&&d.elements.size()==4&&start==EffectStart::FirstFrame)||
            ((id==35||id==36)&&d.elements.size()==1&&start==EffectStart::FirstFrame)||
            (id==106&&d.elements.size()==1)||(id==105&&d.elements.size()==16)||(id==90&&d.elements.size()==1)||(id==91&&d.elements.size()==4)||(id==16&&d.elements.size()==3),
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
                        case 65:require(id==106,"Unrecovered water shape owner");texture=2;break;
                        case 28:texture=4;break;case 31:texture=17;break;
                        //0x10a970 switches shape-1: shape30 calls103938(6),
                        //then1106b8(-.5,-.5), independently of EFD.texture.
                        case 30:require(id==22||id==38||id==39||id==36,"Unrecovered hit-quad owner");texture=6;break;
                        case 33:require(id==35,"Unrecovered numeric geometry owner");texture=12;break;
                        case 59:require(id==33,"Unrecovered effect shape59 owner");texture=25;break;
                        case 35:require(id==14,"Unrecovered segmented-strip owner");texture=9;break;
                        case 7:case 9:
                            require(id==6,"Unrecovered dark-ring owner");texture=0xffffffffu;break;
                        case 3:case 55:
                            require(id==24,"Unrecovered shadow-effect geometry owner");texture=0xffffffffu;break;
                        case 25:case 40:case 32:
                            require(id==14||id==24||id==33||id==115||id==90||id==91,"Unrecovered untextured effect owner");
                            texture=0xffffffffu;break;
                        default:throw std::runtime_error("Unrecovered map effect shape");}
        if(texture==0xffffffffu){e.texture=texture;e.uv={0,0,65535,65535};}
        else set_texture(e,texture);
        if(e.shape==65) {
            e.water=std::make_shared<EffectWaterSurface>();
            //0x10e690 uses i/32, not i/31;0x1122e8 uses texture height+1.
            for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
                e.water->packed_uv[y*32+x]={std::uint16_t(sizes[2][0]*x),std::uint16_t((sizes[2][1]+1)*y)};
        }
        if((id==14||id==24||id==33)&&e.shape==25)e.ring_y.assign(30,-192);
        if(id==14&&e.shape==35) {
            //0x110908 accumulates six float steps before XYZ/UV packing.
            std::array<float,7> along{};const float delta=1.f/6;
            for(unsigned i=1;i<along.size();++i)along[i]=along[i-1]+delta;
            const auto add=[&](unsigned segment,bool top) {
                e.strip_positions.push_back({std::int16_t(std::round(along[segment]*64)),std::int16_t(top?32:-32),0});
                e.strip_tags.push_back(std::uint8_t(segment));
                e.strip_uv.push_back({std::uint16_t(along[segment]*float(sizes[9][0])*32),
                                     std::uint16_t(top?(sizes[9][1]+1)*32:0)});
            };
            add(1,false);add(0,false);add(1,true);add(0,true);
            for(unsigned i=2;i<=6;++i){add(i,false);add(i,true);}
        }
        if(id==26&&e.shape==24)e.cylinder_t.assign(17,0);
        if(((id==24||id==105||id==38||id==39)&&e.shape==31)||((id==22||id==36)&&e.shape==30)) {
            const auto initial=e.shape==30?6u:17u;
            const auto w=std::uint16_t(sizes[initial][0]*32),h=std::uint16_t((sizes[initial][1]+1)*32);
            e.quad_uv={{{0,0},{w,0},{w,h},{0,h}}};
        }
        if(id==35&&e.shape==33) {
            const auto w=std::uint16_t(sizes[12][0]*32),h=std::uint16_t((sizes[12][1]+1)*32);
            for(unsigned q=0;q<3;++q)for(unsigned v=0;v<4;++v)
                e.number_uv[q*4+v]={std::uint16_t(v==1||v==2?w:0),std::uint16_t(v>=2?h:0)};
        }
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
void MapEffect::rebind_water_texture(unsigned element,std::uint32_t texture,
                                     std::uint32_t width,std::uint32_t height) {
    require(id==106&&element<elements.size(),"Water texture rebind owner/element extent");
    auto& e=elements[element];
    require(e.shape==65&&bool(e.water),"Water texture rebind requires shape65 geometry");
    require(texture!=0xffffffffu,"Water texture rebind requires a native textured slot");
    require(width>=1&&width<=4096&&height>=1&&height<=4096,"Water texture descriptor dimensions");
    require(e.water->active<2,"Water texture rebind height-grid selector");
    //Prepare COW storage before publishing metadata; no RNG or clock mutation.
    if(e.water->initialized&&e.water.use_count()!=1)
        e.water=std::make_shared<EffectWaterSurface>(*e.water);
    e.texture=texture;
    e.uv={0,0,std::uint16_t((width&0xfffu)-1u),std::uint16_t((height&0xfffu)-1u)};
    if(!e.water->initialized)return; //10c248: source element+7c is still null.
    auto& water=*e.water;
    const int right=std::bit_cast<std::int16_t>(e.uv[2]),bottom=std::bit_cast<std::int16_t>(e.uv[3]);
    for(int y=0;y<32;++y)for(int x=0;x<32;++x)
        water.coordinates[y*32+x]={right*x/31,bottom-bottom*y/31};
    pack_water(water,0,water.heights[water.active]);
    pack_water(water,1,water.heights[water.active]);
}
void MapEffect::set_numeric_parameter(std::int32_t value) {
    Rounding rounding;parameters.fill(float(value));
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
        const Float4& camera,float far_clip,const SineTable& sine,const Float4* light_tint,EffectDrawPass pass,const EffectTextureSizeResolver& texture_size) {
    if(effect.flags&0x4300)return {};
    require((effect.id==25&&effect.elements.size()==8)||((effect.id==17||effect.id==19||effect.id==20)&&effect.elements.size()==9)||
            (effect.id==106&&effect.elements.size()==1)||(effect.id==105&&effect.elements.size()==16)||(effect.id==90&&effect.elements.size()==1)||(effect.id==91&&effect.elements.size()==4)||(effect.id==16&&effect.elements.size()==3)||
            (effect.id==33&&effect.elements.size()==9)||(effect.id==115&&effect.elements.size()==5)||
            (effect.id==26&&effect.elements.size()==2)||(effect.id==6&&effect.elements.size()==4)||
            (effect.id==24&&effect.elements.size()==8)||(effect.id==14&&effect.elements.size()==7)||
            (effect.id==38&&effect.elements.size()==3)||((effect.id==22||effect.id==39)&&effect.elements.size()==4)||
            ((effect.id==35||effect.id==36)&&effect.elements.size()==1),"Unrecovered map effect geometry template");
    require(bool(effect.flags&0x400)==(pass==EffectDrawPass::Deferred),"Effect submitted to the wrong draw pass");
    require(pass==EffectDrawPass::Ordinary||effect.id==22||effect.id==35||effect.id==36||effect.id==38||effect.id==39||effect.id==6||effect.id==14||effect.id==16||effect.id==33||effect.id==106||effect.id==115,"Unrecovered deferred effect geometry");
    require(effect.id!=16||pass==EffectDrawPass::Deferred,"Flame16 requires the deferred pass");
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
        require(effect.id==22?(e.primitive_flags==0x120||e.primitive_flags==0x100):effect.id==35?e.primitive_flags==0x40:effect.id==36?e.primitive_flags==0x140:(effect.id==38||effect.id==39)?(e.primitive_flags==0||e.primitive_flags==0x100):effect.id==106?e.primitive_flags==0x41:effect.id==105?(e.primitive_flags==0x41||e.primitive_flags==0x141||e.primitive_flags==0x341):effect.id==14?(e.primitive_flags==0||e.primitive_flags==0x20||e.primitive_flags==0x300):effect.id==24?(e.primitive_flags&255)==0x21:effect.id==6?e.primitive_flags==0x20:effect.id==16?e.primitive_flags==8:effect.id==115?e.primitive_flags==0x40:
                (e.primitive_flags&255)==1||((effect.id==17||effect.id==19||effect.id==20||effect.id==26)&&e.primitive_flags==0x41),
                "Unrecovered map effect material");
        require(!(e.flags&0x400000)||light_tint,"Effect requires owned field light tint");
        //0x105a40 sets render+0x58 bit0 when switching programs. It records
        //the captured channel transition;0x102cf8 does not test that bit.
        const unsigned allowed_render_flags=effect.id==24||effect.id==33||effect.id==115?1u:0u;
        require(e.uv[0]==0&&e.uv[1]==0&&!(e.render_flags&~allowed_render_flags),"Unrecovered map effect texture region");
        auto rotation=e.rendered[1];
        switch(e.flags&15){case 0:break;case 1:
            rotation[0]=(1-camera[0])-effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];rotation[3]=camera[3];break;
            case 5:require(effect.id==115||effect.id==35||effect.id==36,"Unrecovered camera-space effect owner");break;
            case 2:
            require(((effect.id==90||effect.id==91)&&e.shape==40)||(effect.id==14&&(e.shape==25||e.shape==40)),"Unrecovered quarter-turn billboard owner");
            rotation[0]=((1-camera[0])+.25f)-effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];rotation[3]=camera[3];break;
            case 8:
            require((effect.id==91||effect.id==16)&&e.shape==28,"Unrecovered horizontal billboard owner");
            rotation[0]-=effect.rotation[0];
            rotation[1]=(1-camera[1])-effect.rotation[1];break;
            default:throw std::runtime_error("Unrecovered map effect orientation");}
        auto matrix=effect_matrix(e.rendered[0],rotation,e.rendered[2],sine,true);
        if((e.flags&15)!=5)matrix=compose_pose(job,std::span(&matrix,1))[0];
        if(!(e.flags&4))matrix=compose_pose(view,std::span(&matrix,1))[0];
        Float4 color{};for(unsigned i=0;i<4;++i){
            color[i]=e.rendered[3][i]*effect.color[i];
            if(e.flags&0x400000)color[i]*=(*light_tint)[i];
        }
        color[3]*=fade;
        //16's TEST=0x53001 uses alpha-test NEVER with RGB_ONLY failure.
        // Its RGB blend is additive; the additive Metal path preserves target
        // alpha and disables depth writes, as required for that failure mode.
        MapEffectDraw draw{matrix,color,e.texture,e.uv,{},(e.primitive_flags&255)==1||e.primitive_flags==8||
            ((effect.id==14||effect.id==22||effect.id==38||effect.id==39)&&(e.primitive_flags&255)==0)};
        draw.depth_test=effect.id!=22&&effect.id!=35&&effect.id!=36&&effect.id!=115&&effect.id!=6&&effect.id!=14&&effect.id!=38&&effect.id!=39;
        draw.subtractive=effect.id==6||effect.id==24||((effect.id==14||effect.id==22)&&(e.primitive_flags&255)==0x20);
        draw.clamp_mode=effect.clamp_mode;
        const Float4 white{255,255,255,255},clear{255,255,255,0};
        if(effect.id==35&&e.shape==33) {
            //110528: three quads tagged1..3, X starts-1.5 and advances1.
            require(e.texture==12,"Unrecovered numeric atlas binding");
            for(unsigned q=0;q<3;++q) {
                const float left=float(q)-1.5f;
                const std::array<Float4,4> points={Float4{left,-.5f,0,1},Float4{left+1,-.5f,0,1},
                    Float4{left+1,.5f,0,1},Float4{left,.5f,0,1}};
                for(unsigned v:{1u,0u,2u,0u,2u,3u}) {
                    const auto& uv=e.number_uv[q*4+v];
                    draw.vertices.push_back({points[v],white,{uv[0]/32768.f,uv[1]/32768.f,0,0}});
                }
            }
            result.push_back(std::move(draw));continue;
        }
        if(e.shape==65) {
            require(effect.id==106&&e.water&&e.texture!=0xffffffffu,"Missing native water geometry or texture binding");
            for(unsigned y=0;y<31;++y)for(unsigned x=0;x<31;++x) {
                const std::array<unsigned,4> corners={y*32+x,y*32+x+1,(y+1)*32+x+1,(y+1)*32+x};
                // Factory0x112d20 emits B,A,C,D with the A-C diagonal.
                for(unsigned corner:{1u,0u,2u,0u,2u,3u}) {
                    const auto i=corners[corner];const auto& uv=e.water->packed_uv[i];
                    draw.vertices.push_back({{float(int(i%32)*2-32)/64,e.water->packed_y[i]/64.f,
                        float(int(i/32)*2-32)/64,1},white,{uv[0]/32768.f,uv[1]/32768.f,0,0}});
                }
            }
            result.push_back(std::move(draw));continue;
        }
        if(effect.id==6) {
            require((e.shape==7||e.shape==9)&&e.texture==0xffffffffu,"Unrecovered dark-ring geometry");
            //0x10faa0: independent B,A,C,D strips between two scaled circles.
            // Shapes7/9 use20/40 segments and distinct radial color ramps.
            const unsigned count=e.shape==7?20:40;
            const auto point=[&](unsigned i,bool outer) {
                const auto a=angle(float(i%count)/float(count));
                const float radius=e.shape==7?(outer?1.25f:.3f):(outer?1.5f:.8f);
                const float y=e.shape==7&&outer?-.95f:0;
                return Float4{std::round((sine[(a+4096)&16383]*radius)*64)/64,
                    std::round(y*64)/64,std::round((sine[a]*radius)*64)/64,1};
            };
            const Float4 inner=e.shape==7?clear:Float4{255,255,255,172};
            const Float4 outer=e.shape==7?white:Float4{32,96,32,0};
            for(unsigned i=0;i<count;++i) {
                const std::array<Float4,4> p={point(i,true),point(i,false),point(i+1,false),point(i+1,true)};
                for(unsigned v:{1u,0u,2u,0u,2u,3u})
                    draw.vertices.push_back({p[v],v==1||v==2?inner:outer,{}});
            }
            result.push_back(std::move(draw));continue;
        }
        if(effect.id==91||effect.id==16||e.shape==40) {
            if(e.shape==28) {
                require(e.texture==4,"Unrecovered flame texture");
                // Stored geometry already has original strip order B,A,C,D.
                for(unsigned v:{0u,1u,2u,1u,2u,3u}) {
                    const auto& p=e.quad_positions[v];const auto& uv=e.quad_uv[v];
                    draw.vertices.push_back({{p[0]/64.f,p[1]/64.f,p[2]/64.f,1},white,
                                             {uv[0]/32768.f,uv[1]/32768.f,0,0}});
                }
            } else if(e.shape==40&&(effect.id==90||effect.id==91||effect.id==33||effect.id==14)) {
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
        if(effect.id==14&&e.shape==35) {
            require(e.texture==9&&e.strip_positions.size()==14&&e.strip_uv.size()==14,"Unrecovered segmented-strip drawing");
            // Six original B,A,C,D strips share deduplicated endpoint pairs.
            const auto bottom=[](unsigned i){return i==0?1u:i==1?0u:i*2;};
            const auto top=[](unsigned i){return i==0?3u:i==1?2u:i*2+1;};
            for(unsigned i=0;i<6;++i) {
                const std::array<unsigned,4> corners={bottom(i+1),bottom(i),top(i+1),top(i)};
                for(unsigned corner:{0u,1u,2u,1u,2u,3u}) {
                    const auto v=corners[corner];const auto& p=e.strip_positions[v];const auto& uv=e.strip_uv[v];
                    draw.vertices.push_back({{p[0]/64.f,p[1]/64.f,p[2]/64.f,1},white,
                                             {uv[0]/32768.f,uv[1]/32768.f,0,0}});
                }
            }
            result.push_back(std::move(draw));continue;
        }
        const bool textured=e.texture!=0xffffffffu;
        const auto base=effect.texture_sizes().at(e.shape==24?20:e.shape==31?17:e.shape==30?6:e.shape==59?25:5);
        const bool replace=(e.program>>30)==1;
        //0x1122e8 constructs T using height+1.0x1055d8 rescales the
        // original packed UVs with truncated5-bit fixed ratios on replacement.
        auto uv=[&](float u,float v){
            if(!textured)return Float4{};
            int s=int(u*float(base[0])*32),t=int(v*float(base[1]+1)*32);
            require(s>=0&&s<=32767&&t>=0&&t<=32767,"Unrecovered effect UV packing range");
            if(replace){
                const auto current=texture_size?texture_size(e.texture):effect.texture_sizes().at(e.texture);
                require(current[0]&&current[1]&&current[0]<=4096&&current[1]<=4096,
                        "Invalid effect replacement texture dimensions");
                s=(s*int((current[0]<<5)/base[0]))>>5;t=(t*int((current[1]<<5)/base[1]))>>5;
            }
            return Float4{float(std::uint16_t(s))/32768,float(std::uint16_t(t))/32768,0,0};
        };
        auto quad=[&](const std::array<Float4,4>& p,const std::array<Float4,4>& c,const std::array<Float4,4>& uv) {
            // Original strip order B,A,C,D, hence the shared A-C diagonal.
            for(unsigned i:{1u,0u,2u,0u,2u,3u})draw.vertices.push_back({p[i],c[i],uv[i]});
        };
        if(effect.id==24&&(e.shape==3||e.shape==55)) {
            require(!textured,"Unrecovered textured shadow shape");
            if(e.shape==3)draw.vertices=make_shadow_effect_shape(3,sine,{});
            else {
                require(effect.shape55_geometry&&effect.shape55_geometry->size()==240,"Missing native shape55 template geometry");
                draw.vertices=*effect.shape55_geometry;
            }
        } else if(e.shape==25&&(effect.id==14||effect.id==24||effect.id==33)) {
            require(!textured&&e.ring_y.size()==30,"Unrecovered summoning ring");
            const auto point=[&](unsigned i,bool lower){
                const auto a=angle(float(i%30)/30);
                return Float4{std::round(sine[(a+4096)&16383]*64)/64,
                    lower?e.ring_y[i%30]/64.f:0,std::round(sine[a]*64)/64,1};};
            for(unsigned i=0;i<30;++i)
                quad({point(i,true),point(i,false),point(i+1,false),point(i+1,true)},
                     {clear,white,white,clear},{Float4{},Float4{},Float4{},Float4{}});
        } else if(e.shape==59&&effect.id==33) {
            require(e.texture==25,"Unrecovered summoning spiral texture");
            //0x110db8 constructs five turns of ten quads. Keep the original
            //single-precision accumulation before packing positions and UVs.
            const float delta=-(1.f/50);float phase=0;
            const auto point=[&](unsigned i,float y,float radius){
                const auto a=angle(float(i%10)/10);
                const float x=sine[(a+4096)&16383]*radius,z=sine[a]*radius;
                return Float4{std::round(x*64)/64,std::round(y*64)/64,std::round(z*64)/64,1};};
            for(unsigned turn=0;turn<5;++turn) {
                const float r0=sine[angle(float(turn)/5)]*.5f+1;
                const float r1=sine[angle(float(turn+1)/5)]*.5f+1;
                for(unsigned i=0;i<10;++i) {
                    const float next=phase+delta,a=float(i)/10,b=float(i+1)/10;
                    const float ra=(1-a)*r0+a*r1,rb=(1-b)*r0+b*r1;
                    quad({point(i,phase-delta*10,ra),point(i,phase,ra),
                          point(i+1,next,rb),point(i+1,phase-delta*9,rb)},
                         {white,white,white,white},
                         {uv(-phase,0),uv(-phase,1),uv(-next,1),uv(-next,0)});
                    phase=next;
                }
            }
        } else if(e.shape==24) {
            if(effect.id==26)require(e.cylinder_t.size()==17,"Missing animated cylinder UVs");
            const auto lower_uv=[&](unsigned i){
                auto value=uv(float(i)/16,0);
                if(effect.id==26)value[1]=float(e.cylinder_t[i])/32768;
                return value;
            };
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
                    {lower_uv(i),uv(a,1),uv(b,1),lower_uv(i+1)});
            }
        } else if(e.shape==29||((effect.id==17||effect.id==19||effect.id==20||effect.id==24||effect.id==105||effect.id==115||effect.id==38||effect.id==39)&&e.shape==31)||
                  (effect.id==115&&e.shape==32)||((effect.id==22||effect.id==38||effect.id==39||effect.id==36)&&e.shape==30)) {
            const float bottom=e.shape==29?-1:-.5f,top=e.shape==29?0:.5f;
            const auto quad_uv=[&](unsigned i,float u,float v){return (effect.id==24||effect.id==105||((effect.id==38||effect.id==39)&&e.shape==31)||((effect.id==22||effect.id==36)&&e.shape==30))?
                Float4{float(e.quad_uv[i][0])/32768,float(e.quad_uv[i][1])/32768,0,0}:uv(u,v);};
            quad({Float4{-.5f,bottom,0,1},Float4{.5f,bottom,0,1},Float4{.5f,top,0,1},Float4{-.5f,top,0,1}},
                 {white,white,white,white},
                 {quad_uv(0,0,0),quad_uv(1,1,0),quad_uv(2,1,1),quad_uv(3,0,1)});
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
        result[i]=((base[i]+ambient[i])+light[i])*std::bit_cast<float>(0x3eaaaa3bu);
        maximum=std::max(maximum,result[i]);
    }
    require(std::isfinite(maximum)&&maximum>0,"Unrecovered degenerate effect lighting");
    for(unsigned i=0;i<3;++i)result[i]/=maximum;
    result[3]=1;return result;
}
void MapEffect::tick(const std::function<std::uint32_t()>& random,bool clock_enabled,const Float4* eye,
                     const EffectProjectionContext* projection) {
    if(!clock_enabled)return;
    require(!(flags&0x100),"Cannot update a removed map effect");
    require(id==22||id==35||id==36||id==38||id==39||id==6||id==14||id==24||id==25||id==26||id==17||id==19||id==20||id==33||id==105||id==106||id==115||id==90||id==91||id==16,"Unrecovered map effect update program");
    if(id==35||id==36) {
        require(projection,"Numeric effect requires live projection context");
        require(std::isfinite(projection->focal)&&projection->focal>0,"Invalid numeric effect focal length");
        for(const auto& column:projection->view)for(float v:column)require(std::isfinite(v),"Invalid numeric effect view");
        for(const auto& record:projection->screen)for(float v:record)require(std::isfinite(v),"Invalid numeric effect projection");
    }
    if(id==90||id==91){require(eye,"Glow effect requires the current camera position");
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
            require(!(e.flags&0x800000),"Unrecovered map effect texture mode");
            if(mode==3) {
                const auto source=std::find_if(elements.begin(),elements.end(),[&](const auto& v){return float(v.id)==e.interval[2];});
                require(source!=elements.end()&&source->id<e.id&&
                        (((id==24||id==105)&&source->shape==31&&e.shape==31)||(id==14&&source->shape==35&&e.shape==35)),
                        "Unrecovered shared effect texture geometry");
                //0x105e78 shares geometry and copies the source texture/UV
                //rectangle after its first-update replacement. Mutable shape35
                //coordinates are refreshed from that source on every update.
                e.texture=source->texture;e.uv=source->uv;
                e.quad_uv=source->quad_uv;
                if(id==14)e.strip_source_id=source->id;
            } else if(mode) {
                const auto old=((((id==24||id==105||id==38||id==39)&&e.shape==31)||((id==22||id==36)&&e.shape==30))&&mode==1)?texture_sizes_.at(e.texture):std::array<std::uint32_t,2>{};
                set_texture(e,unsigned(e.interval[2]));
                if((((id==24||id==105||id==38||id==39)&&e.shape==31)||((id==22||id==36)&&e.shape==30))&&mode==1) {
                    const auto next=texture_sizes_.at(e.texture);
                    for(auto& pair:e.quad_uv)for(unsigned axis=0;axis<2;++axis)
                        pair[axis]=std::uint16_t((unsigned(pair[axis])*((next[axis]<<5)/old[axis]))>>5);
                }
            }
            e.interval[2]=0;
        }
        if((e.flags&0x2000)&&e.interval[2]<t)program&=0xc0ffffff;
        const auto timing=(program>>24)&63;
        require(timing==0||timing==1||timing==5||timing==6||timing==7||timing==12||timing==17||timing==20||timing==26||timing==27||timing==32,"Unrecovered map effect time program");
        if(timing==26||timing==27) {
            require((timing==26&&id==35&&e.shape==33)||(timing==27&&id==36&&e.shape==30),
                    "Unrecovered numeric atlas owner");
            const auto atlas=texture_sizes_.at(e.texture);
            const auto original=texture_sizes_.at(timing==26?12:6);
            const auto remap=[&](unsigned corner,int x,int width,int height,
                                 const std::array<std::uint16_t,2>& previous) {
                const int dx=int(atlas[0]*32)/width,dy=int(atlas[1]*32)/height;
                std::array<std::uint16_t,2> result{};
                //104c60..68/104e34..3c: a zero integer step skips the entire UV update.
                if(dx==0||dy==0)return previous;
                for(unsigned axis=0;axis<2;++axis) {
                    const bool far=axis?corner>=2:corner==1||corner==2;
                    const auto packed=std::uint16_t(far?(original[axis]+(axis?1:0))*32:0);
                    const auto signed_uv=std::bit_cast<std::int16_t>(packed);
                    const auto scaled=(std::int32_t(signed_uv)*4096)/(axis?dy:dx);
                    result[axis]=std::uint16_t((scaled>>7)+(axis?0:x*32));
                }
                return result;
            };
            if(timing==26) {
                require(std::isfinite(parameters[3])&&parameters[3]>=-2147483648.f&&parameters[3]<2147483648.f,
                        "Numeric effect parameter conversion outside signed32");
                auto number=std::int32_t(parameters[3]);
                if(number<0) {
                    number=std::bit_cast<std::int32_t>(0u-std::bit_cast<std::uint32_t>(number));
                    e.color[2]=.2f;e.color[0]=.2f;
                }
                for(unsigned digit=0;digit<3;++digit) {
                    const int cell=number==0&&digit!=0?11:number%10;
                    number/=10;
                    //106960: tags3,2,1, rightmost units to leftmost hundreds.
                    for(unsigned corner=0;corner<4;++corner)
                        e.number_uv[(2-digit)*4+corner]=remap(corner,cell*16,16,16,e.number_uv[(2-digit)*4+corner]);
                }
            } else for(unsigned corner=0;corner<4;++corner)
                e.quad_uv[corner]=remap(corner,192,172,4,e.quad_uv[corner]); //10699c..1069b4
        }
        if(timing==32) {
            require(id==106&&e.shape==65&&e.water,"Unrecovered water program owner");
            if(e.water.use_count()!=1)e.water=std::make_shared<EffectWaterSurface>(*e.water);
            if(!e.water->initialized){clamp_mode=3;near_clip=10;}
            update_water(*e.water,e,parameters);
        }
        if(timing==12) {
            require(id==14&&e.shape==35&&e.strip_positions.size()==14,"Unrecovered segmented-strip deformation");
            if(!(flags&0x20)) {
                //0x105200 rotates original template geometry before the first
                //deformation. No captured positions or extra RNG draws.
                const auto matrix=effect_matrix({}, {0,0,.75f,0},{1,1,1,1},sine_,true);
                for(auto& point:e.strip_positions) {
                    const auto source=point;
                    for(unsigned c=0;c<3;++c) {
                        float value=matrix[0][c]*float(source[0]);value+=matrix[1][c]*float(source[1]);
                        value+=matrix[2][c]*float(source[2]);value+=matrix[3][c];
                        point[c]=std::int16_t(int(value));
                    }
                }
            }
            if(!(flags&0x20)||e.rendered[0][3]+.04f<t) {
                for(unsigned vertex=0;vertex<e.strip_positions.size();++vertex) {
                    const auto tag=e.strip_tags[vertex];if(!tag)continue;
                    const auto draw=random();
                    const std::array<int,3> jitter={int(draw%80)-40,int((draw>>10)%2)-1,int((draw>>20)%80)-40};
                    for(unsigned c=0;c<3;++c) {
                        const float value=e.rendered[0][c]*float(6-tag)+parameters[c]*float(tag);
                        e.strip_positions[vertex][c]=std::int16_t(int(value)+jitter[c]);
                    }
                }
                e.rendered[0][3]=(flags&0x20)?t:0;
            }
        }
        if(timing==20) {
            require(id==26&&e.shape==24&&e.cylinder_t.size()==17&&e.interval[3]==1,
                    "Unrecovered animated cylinder UV mode");
            //0x105790 mode1 counts all34 UVs, then advances only at tag2.
            // Accumulate the original float step, including the seam entry.
            const float step=1.f/(34.f*.5f);float local=0;
            for(auto& value:e.cylinder_t) {
                value=std::uint16_t(int(sine_[angle(local+t)]*300.f)+0x131);
                local+=step;if(local>=1)local-=1;
            }
        }
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
            require((id==14||id==24||id==33)&&e.shape==25&&e.ring_y.size()==30,"Unrecovered effect deformation geometry");
            for(auto& y:e.ring_y)y=std::int16_t(std::min(0,int(random()%50)-25-64));
        }
        if(timing==17) {
            //0x105518 links geometry to the element with the preceding ID.
            // Templates17/20's shape31 packets are identical immutable quads.
            const auto previous=std::find_if(elements.begin(),elements.end(),[&](const auto& p){return p.id+1==e.id;});
            require(previous!=elements.end()&&previous->texture==e.texture&&
                    (((id==17||id==20)&&previous->shape==31&&e.shape==31)||
                     (id==26&&previous->shape==24&&e.shape==24&&previous->cylinder_t.size()==17)),
                    "Unrecovered effect geometry sharing");
            if(id==26)e.cylinder_source_id=previous->id;
        }
        if(timing==7){t=t*3.0f+e.interval[3];t-=float(int(t));}
        if(timing==6){t=t*3.0f+e.interval[3];t=t>3?1:t-float(int(t));}
        if((e.flags&0x1000)&&e.interval[2]<t)program&=0xfffc0fff;
        auto& p=e.rendered[0];auto& r=e.rendered[1];auto& s=e.rendered[2];auto& c=e.rendered[3];
        switch((program>>12)&63) {
        case 0:break;
        case 1:
            for(unsigned i=0;i<3;++i)p[i]=float(int(random()%40)-20)*parameters[i]+e.position[i];
            break;
        case 2:
            //106cb4..106d44: independent XYZ draws; only Y adds positionW.
            p[0]=float(int(random()%40)-20)*e.position[0];
            p[1]=float(int(random()%40)-20)*e.position[1]+e.position[3];
            p[2]=float(int(random()%40)-20)*e.position[2];
            break;
        case 17:p[1]=e.position[1]+e.position[3]*t;break;
        case 23:
            //0x107558: quadratic Y fall and linear X from authored position-W.
            p[1]=e.position[1]-(t*t)*e.position[3];
            p[0]=e.position[0]+t*e.position[3];break;
        case 28:p[1]=(t*t)*e.position[3]+e.position[1];break;
        case 26:p[1]=((t*4.f)*(1.f-t))*e.position[3]+e.position[1];break;
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
        case 50: {
            require((id==35||id==36)&&projection,"Unrecovered projected numeric owner");
            if(!(flags&0x20))e.position[0]=float(int(random()%100)-50)*.01f;
            Float4 camera_point{},clip{};
            for(unsigned lane=0;lane<4;++lane) {
                float value=projection->view[0][lane]*position[0];
                value+=projection->view[1][lane]*position[1];
                value+=projection->view[2][lane]*position[2];
                camera_point[lane]=value+projection->view[3][lane]*1.f;
            }
            const auto& screen=projection->screen;
            for(unsigned lane=0;lane<4;++lane)
                clip[lane]=screen[0][lane]*camera_point[lane<2?lane:2]+screen[1][lane];
            require(std::isfinite(clip[3])&&clip[3]!=0,"Numeric projection zero/nonfinite reciprocal is unrecovered");
            const float q=1.f/clip[3];
            for(unsigned lane=0;lane<4;++lane) {
                float value=screen[4][lane]*clip[lane==3?2:3];
                value+=clip[lane]*screen[5][lane];
                value*=q;
                require(std::isfinite(value),"Numeric projection overflow is unrecovered");
                p[lane]=std::max(value,0.f);
            }
            p[3]=std::min(p[3],255.f);
            if(p[2]<=0) {p[1]=100.f;p[2]=-1.f;}
            else {
                p[0]=(p[0]-2048.f)*.025f+e.position[0];
                const float quotient=t==0?-std::numeric_limits<float>::infinity():-.15f/t;
                const float lift=quotient < -1.f?0.f:quotient+1.f;
                const float y=(p[1]-2048.f)*std::bit_cast<float>(0x3d59df52u);
                const float rise=lift*e.position[1];
                p[2]=projection->focal*.025f;p[1]=y+rise;
            }
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
        case 7:if(t<e.interval[2])r[2]=float(random()%360+360)*.00278f;break;
        case 1:{const float cycle=t*e.rotation[3];r[1]=(cycle-float(int(cycle)))+e.rotation[1];break;}
        case 4:r[2]=float(random()%360+360)*.00278f;break;case 11:r[1]=float(random()%360+360)*.00278f;break;
        case 8: {
            require(std::isfinite(e.rotation[3])&&e.rotation[3]>=-2147483648.f&&e.rotation[3]<2147483648.f,
                    "Effect rotation reference outside signed32");
            const auto selected=std::int32_t(e.rotation[3]);
            const auto source=std::find_if(elements.begin(),elements.end(),[&](const auto& v){
                return std::bit_cast<std::int32_t>(v.id)==selected;
            });
            require(source!=elements.end(),"Missing referenced effect rotation");
            for(unsigned axis=0;axis<3;++axis)r[axis]=source->rendered[1][axis]+e.rotation[axis];
            break;
        }
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
        case 4:{const float local=t<0?0:t>e.scale[3]?1:t/e.scale[3];
            const float factor=std::sqrt(local);for(unsigned i=0;i<3;++i)s[i]=factor*e.scale[i];break;}
        case 16:s[1]+=e.scale[3];break;
        case 15:s[1]=((t*4.f)*(1.f-t))*e.scale[1];break;
        case 20:{const float x=e.scale[0],cycle=t*30.0f;
            const float size=((x*.75f+x)-(t*.75f)*x)+((1.0f-(cycle-float(int(cycle))))*x)*.15f;
            s[0]=size;s[1]=size;s[2]=size;break;}
        case 6:{const float q=-e.scale[3]/t;const float factor=q < -1?0:q+1;
            for(unsigned i=0;i<3;++i)s[i]=factor*e.scale[i];break;}
        case 13:s[0]=t*e.scale[0];s[1]=(1.0f-t)*e.scale[1];s[2]=t*e.scale[2];break;
        case 10:{const float envelope=t>.5f?1.0f-(t-.5f)/.5f:t<.2f?t/.2f:1;
            s[0]=envelope*e.scale[0];s[1]=t*e.scale[1];s[2]=envelope*e.scale[2];break;}
        case 23: {
            //0x1098a4..1098c4: multiply local time by3, then authored X;
            //store Z, X, Y. W and the authored Y/Z do not participate.
            const float size=(t*3.f)*e.scale[0];
            s[2]=size;s[0]=size;s[1]=size;break;
        }
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
        case 2:c[3]=(t==0||t==1)?0:(1.f-t)*e.color[3];break;
        case 6:c[3]=(t==0||t==1)?0:(t>.7f?(1.f-(t-.7f)/(1.f-.7f))*e.color[3]:e.color[3]);break;
        case 9:c[3]=t<.6f?(1.f-t/.6f)*e.color[3]:e.color[3]*0.f;break;
        case 24:c[3]=t==0?0:1.f-t;break;
        case 32:c[3]=(t==0||t==1)?0:e.color[3];break;
        case 3:
            c[3]=(t==0||t==1)?0:(t>.1f?(1.0f-(t-.1f)/(1.0f-.1f))*e.color[3]:e.color[3]);break;
        case 5:
            c[3]=(t==0||t==1)?0:(t>.6f?(1.0f-(t-.6f)/(1.0f-.6f))*e.color[3]:e.color[3]);break;
        case 13: {
            const float cycle=t*e.interval[3],local=cycle-float(int(cycle));
            const float envelope=local>.6f?1.0f-(local-.6f)/.39999998f:local<.4f?local/.4f:1;
            c[3]=e.color[3]-(envelope*e.color[3])*.25f;break;
        }
        case 30: {
            //10a2b8..10a368: equality only zerosalpha, retaining prior RGB.
            if(t==1.f){c[3]=0;break;}
            const float tail=t>.35f?(t-.35f)/.65f:0.f;
            const float fade=1.f-tail*.8f;
            c[0]=fade*e.color[0];c[1]=fade*e.color[1];
            c[2]=(1.f-e.color[2])*tail+e.color[2];
            c[3]=(1.f-tail)*e.color[3];break;
        }
        case 34: {
            //0x10a410: source alpha minus the authored interval-W modulation.
            const float envelope=t>.6f?1.f-(t-.6f)/.39999998f:t<.4f?t/.4f:1;
            c[3]=e.color[3]-(envelope*e.color[3])*e.interval[3];break;
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
        if(e.cylinder_source_id) {
            const auto source=std::find_if(elements.begin(),elements.end(),[&](const auto& p){return p.id==*e.cylinder_source_id;});
            require(source!=elements.end()&&source->id<e.id&&source->cylinder_t.size()==17,
                    "Invalid cylinder geometry source");
            e.cylinder_t=source->cylinder_t;
        }
        if(e.strip_source_id) {
            const auto source=std::find_if(elements.begin(),elements.end(),[&](const auto& p){return p.id==*e.strip_source_id;});
            require(source!=elements.end()&&source->id<e.id&&source->strip_positions.size()==14,"Invalid segmented-strip geometry source");
            e.strip_positions=source->strip_positions;e.strip_tags=source->strip_tags;e.strip_uv=source->strip_uv;
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
void RetiringSceneEffects::mark() {
    for(unsigned i=0;i<84;++i) {
        require(!(map[i]&&attached[i]),"Retiring effect slot has two owners");
        if(map[i])map[i]->flags|=0x100;
        if(attached[i])attached[i]->animation.flags|=0x100;
    }
}
void RetiringSceneEffects::tick() {
    for(unsigned i=0;i<84;++i) {
        require(!(map[i]&&attached[i]),"Retiring effect slot has two owners");
        if(!map[i]&&!attached[i])continue;
        auto& f=map[i]?map[i]->flags:attached[i]->animation.flags;
        require(f&0x100,"Unmarked retiring effect");
        if(f&0x200){map[i].reset();attached[i].reset();}
        else f|=0x200;
    }
}
std::array<std::uint32_t,84> RetiringSceneEffects::flags() const {
    std::array<std::uint32_t,84> result{};
    for(unsigned i=0;i<84;++i)
        if(map[i])result[i]=map[i]->flags;
        else if(attached[i])result[i]=attached[i]->animation.flags;
    return result;
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
