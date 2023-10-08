#include "okage/ui_window.hpp"
#include "okage/font.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
using I=std::int32_t;using V=std::array<float,4>;using Ops=std::vector<UiPacketOperation>;
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
I si(std::uint32_t v){return std::bit_cast<I>(v);}
I wrap_sub(I a,I b){return si(std::uint32_t(a)-std::uint32_t(b));}
float ordinary(float f){auto v=std::bit_cast<std::uint32_t>(f),e=v&0x7f800000;check(e!=0x7f800000&&(e!=0||(v&0x7fffff)==0),"UI packet exceptional float");return f;}
struct Rounding{std::fenv_t saved;Rounding(){check(std::fegetenv(&saved)==0,"UI packet save fenv");if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI packet rounding");}}~Rounding(){std::fesetenv(&saved);}};
float num(I v){volatile float f=static_cast<float>(v);return f;}
I integer(float f){ordinary(f);check(double(f)>=-2147483648.&&double(f)<2147483648.,"UI packet CVT overflow");return static_cast<I>(f);}
float calc(float a,float b,char op){volatile float x=ordinary(a),y=ordinary(b),z;check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI packet clear exceptions");if(op=='+')z=x+y;else if(op=='-')z=x-y;else if(op=='*')z=x*y;else{check(y!=0,"UI packet zero divisor");z=x/y;}check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)&(FE_OVERFLOW|FE_UNDERFLOW)),"UI packet overflow/underflow");return ordinary(z);}
float add(float a,float b){return calc(a,b,'+');}float sub(float a,float b){return calc(a,b,'-');}float mul(float a,float b){return calc(a,b,'*');}float div(float a,float b){return calc(a,b,'/');}
float value(const UiWindow& w,unsigned at){return ordinary(std::bit_cast<float>(w.word(at)));}
float value(const UiWindowBuffer& b,unsigned at){return ordinary(std::bit_cast<float>(b.word(at)));}
float uv(I v,bool half=true){return mul(half?add(num(v),.5f):num(v),1.f/1024.f);} //19c320/348.
void op(Ops& o,UiPacketOperationKind k,unsigned helper,std::array<std::uint64_t,4> a={}){o.push_back({k,helper,a,{}});}
void control(Ops& o,unsigned helper,std::uint64_t a=0){op(o,UiPacketOperationKind::CompilerBoundary,helper,{a});}
void reg(Ops& o,unsigned r,std::uint64_t v){op(o,UiPacketOperationKind::Register,0x1aedd8,{r,v});}
void texture(Ops& o,bool window,I font=-1){
    // args: GS register6, native binding kind(0font/1window), font index,
    // fixed source TEX0 bits. Texture/CLUT VRAM bases remain host bindings.
    op(o,UiPacketOperationKind::Texture,0x1aedd8,{6,window?1u:0u,std::uint32_t(font),0x20000006a8000000ull});
}
void primitive(Ops& o,unsigned helper,unsigned p,unsigned count=0){op(o,UiPacketOperationKind::Primitive,helper,{p,count});}
void vertex(Ops& o,unsigned p,unsigned c,unsigned t){op(o,UiPacketOperationKind::IndexedVertex,0x1af908,{p,0,c,t});}
void array(Ops& o,UiPacketOperationKind k,unsigned helper,unsigned count,std::vector<V> data){check(data.size()==count,"UI packet array count");o.push_back({k,helper,{0,count},std::move(data)});}
void quad_uv(std::vector<V>& out,I x0,I y0,I x1,I y1){out.push_back({uv(x0),uv(y0),0,0});out.push_back({uv(x1),uv(y0),0,0});out.push_back({uv(x1),uv(y1,false),0,0});out.push_back({uv(x0),uv(y1,false),0,0});}
const FontGlyph* metric(const UiPacketBuildContext& c,I font,I code,bool encoded){
    check(c.fonts&&font>=0&&std::size_t(font)<c.fonts->size(),"UI packet font owner/handle");const auto& f=(*c.fonts)[font];
    if(encoded&&code>=256){const I index=(code/256)-1;check(index>=0&&std::size_t(index)<f.glyphs.size(),"UI packet encoded metric index");return &f.glyphs[index];}
    return f.find(std::uint32_t(code)); // Source null lookup has zero geometry/UV.
}
}
void UiWindow::reset_glyph_packets(){for(unsigned i=0;i<word(0x3c00);++i)put(0x3c08+i*4,word(0x3c08+i*4)|4);put(0xcc0,0);}
void UiWindow::reset_sprite_packets(){for(unsigned i=0;i<word(0x3c04);++i)put(0x3c08+i*4,word(0x3c08+i*4)|8);put(0xcc4,0);}
const UiNativePacket* UiWindow::glyph_packet(unsigned g,unsigned s)const{check(g<word(0x3c00)&&s<2,"UI glyph packet owner bounds");const auto i=glyph_packets_.find({g,s});return i==glyph_packets_.end()?nullptr:&i->second;}
const UiNativePacket* UiWindow::sprite_packet(unsigned g,unsigned s)const{check(g<word(0x3c04)&&s<2,"UI sprite packet owner bounds");const auto i=sprite_packets_.find({g,s});return i==sprite_packets_.end()?nullptr:&i->second;}
int UiWindow::build_glyph_packet(const UiPacketBuildContext& c){
    const I capacity=si(word(0x3c00)),cursor=si(word(0x3e24));check(capacity>0&&capacity<=33&&cursor>=0,"UI glyph packet cursor");
    unsigned group=unsigned(cursor%capacity);
    if(!(word(0x3c08+group*4)&4))for(unsigned i=0;i<unsigned(capacity);++i)if(word(0x3c08+i*4)&4){group=i;break;}
    const unsigned base=group*256;Rounding rounding;const bool encoded=(word(0x4f8)&4)!=0;
    unsigned hidden=0;
    for(unsigned i=0;i<16;++i){const unsigned at=base+i*16;const auto y=value(glyph_[0],at+4);if(y<8){++hidden;continue;}const auto code=integer(value(glyph_[1],at+8));if(encoded?code==-1:code<33)++hidden;}
    if(hidden==16){put(0x3e24,group);put(0x3c08+group*4,word(0x3c08+group*4)|4);put(0x3e24,group+1);return 0;}
    UiNativePacket packet;packet.fonts=c.fonts;auto& operations=packet.operations;
    control(operations,0x1ad1f8);control(operations,0x1af978);
    std::array<bool,16> skipped{};std::array<const FontGlyph*,16> metrics{};
    std::vector<V> positions,colors,coordinates;positions.reserve(64);colors.reserve(32);coordinates.reserve(64);
    for(unsigned i=0;i<16;++i){
        const unsigned at=base+i*16;const auto z=sub(value(glyph_[0],at+8),value(*this,0x568));
        const auto y=value(glyph_[0],at+4);
        if(y<8||integer(value(glyph_[1],at+8))<33){skipped[i]=true;positions.insert(positions.end(),{{0,0,0,0},{1,0,0,0},{0,1,0,0},{0,0,1,0}});continue;}
        const auto code=integer(value(glyph_[1],at+8)),font=integer(value(glyph_[1],at+12));
        metrics[i]=metric(c,font,code,encoded);I x0=0,x1=0,y0=0,y1=0;
        if(metrics[i]){const auto x=value(glyph_[0],at);x0=integer(sub(x,value(*this,0x560)));x1=integer(sub(add(x,value(glyph_[1],at)),value(*this,0x560)));y0=integer(sub(y,value(*this,0x564)));y1=integer(sub(add(y,value(glyph_[1],at+4)),value(*this,0x564)));}
        check(!(word(0x860)!=0&&word(0xcf0)==group&&word(0xcec)==i),"UI glyph deformation owner unrecovered");
        // Non-deformer source reuses86c for all four Y offsets (not87c/88c/89c).
        for(unsigned corner=0;corner<4;++corner){const auto off=0x868+corner*16;positions.push_back({add(num(corner==0||corner==3?x0:x1),value(*this,off)),add(num(corner<2?y0:y1),value(*this,0x86c)),add(z,value(*this,off+8)),1});}
    }
    array(operations,UiPacketOperationKind::Positions,0x1ae940,64,std::move(positions));
    //1a5990: ad1f8/ad1e0 and af978 set333be4=1 before ae940 consumes it.
    operations.back().first_positions_upload=true;
    const auto phase=si(word(0xcd0)),period=si(word(0xcd4));const auto magnitude=period<0?wrap_sub(0,period):period;
    const auto half=mul(num(magnitude),.5f);float reveal_first,reveal_second;
    if(phase<period/2){reveal_first=div(num(phase),half);reveal_second=0;}else{reveal_first=1;reveal_second=div(sub(num(phase),half),half);}
    if(phase==0){reveal_first=.001f;reveal_second=.001f;}if(period<0||(word(0x1904)&1)){reveal_first=1;reveal_second=1;}
    const auto rem=integer(value(*this,0x264))%24;const auto scroll=mul(mul(div(mul(num(24-rem),128.f),24.f),1.f/128.f),.5f);
    const auto scroll_first=rem<12?scroll:0.f,scroll_second=rem<12?1.f:add(scroll,.5f);
    for(unsigned i=0;i<16;++i){
        if(skipped[i]){colors.push_back({1,0,0,0});colors.push_back({0,1,0,0});continue;}
        const unsigned at=base+i*16;const auto packed_float=value(glyph_[0],at+12);check(packed_float>=0&&double(packed_float)<=4294967295.,"UI glyph packed color conversion");const auto packed=static_cast<std::uint32_t>(packed_float);
        V color{mul(num((packed&63)<<2),1.f/128.f),mul(num((packed>>4)&252),1.f/128.f),mul(num((packed>>10)&252),1.f/128.f),mul(num((packed>>16)&252),1.f/128.f)};
        float first=1,second=1;if(sub(value(glyph_[0],at+4),16.f)<0){first=scroll_first;second=scroll_second;}else if((word(0x1900)&6)==4&&word(0xcec)==i&&word(0xcf0)==group){first=reveal_first;second=reveal_second;}
        auto top=color,bottom=color;top[3]=mul(color[3],first);bottom[3]=mul(color[3],second);colors.push_back(top);colors.push_back(bottom);
    }
    array(operations,UiPacketOperationKind::Colors,0x1aeb48,32,std::move(colors));
    for(unsigned i=0;i<16;++i){
        if(skipped[i]){coordinates.insert(coordinates.end(),{{1,0,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}});continue;}
        I x0=0,x1=0,y0=0,y1=0;if(metrics[i]){const auto& m=metrics[i]->metrics;
            x0=integer(mul(num(m[4]>>2),.25f));x1=integer(mul(num(m[6]>>2),.25f));
            y0=integer(mul(num(wrap_sub(m[5],8)>>2),.25f));y1=integer(mul(num(wrap_sub(m[7],8)>>2),.25f));}
        quad_uv(coordinates,x0,y0,x1,y1);
    }
    array(operations,UiPacketOperationKind::UV,0x1aed70,64,std::move(coordinates));control(operations,0x1ad268,0);reg(operations,0x47,0x30003);
    I font=-1;
    for(unsigned i=0;i<16;++i){if(skipped[i])continue;const unsigned at=base+i*16;const auto candidate=value(glyph_[1],at+12);if(candidate!=num(font)&&candidate>=0&&candidate<16)font=integer(candidate);
        check(c.fonts&&font>=0&&std::size_t(font)<c.fonts->size(),"UI glyph texture binding");
        reg(operations,0x3f,0);texture(operations,false,font);reg(operations,0x14,0x60);reg(operations,8,5);primitive(operations,0x1af3e8,0x58);
        const auto p=i*4,col=i*2;
        if(sub(value(glyph_[0],at+4),16.f)<0){vertex(operations,p,col,p);vertex(operations,p+1,col,p+1);vertex(operations,p+2,col+1,p+2);vertex(operations,p+3,col+1,p+3);}
        else if((word(0x1900)&6)==4&&word(0xcec)==i&&word(0xcf0)==group){vertex(operations,p,col,p);vertex(operations,p+1,col+1,p+1);vertex(operations,p+2,col+1,p+2);vertex(operations,p+3,col,p+3);}
        else for(unsigned j=0;j<4;++j)vertex(operations,p+j,col,p+j);
    }
    control(operations,0x1afa00);control(operations,0x1ad240);
    const unsigned side=word(0x268c)!=0;glyph_packets_[{group,side}]=std::move(packet);
    put(0x3e24,group);put(0x3c08+group*4,(word(0x3c08+group*4)&~4u)|2);put(0x3e24,group+1);return 1;
}
void UiWindow::build_sprite_packet(const UiPacketBuildContext& c){
    const auto group=word(0x3e28);check(group<word(0x3c04)&&group<sprite_cache_.size(),"UI sprite packet group has no cache owner");
    check(bool(texture_),"UI sprite packet texture owner");const unsigned side=word(0x2690)!=0,base=group*256;
    Rounding rounding;UiNativePacket packet;packet.window_texture=texture_;auto& header=packet.operations;
    control(header,0x1ad1f8);control(header,0x1af978);texture(header,true);reg(header,0x47,0x30003);control(header,0x1afa00);
    std::vector<V> colors{{255,255,255,0}};
    for(unsigned i=0;i<4;++i){const auto color=word(0x2e0+i*4);colors.push_back({mul(num(color&255),1.f/128.f),mul(num((color>>8)&255),1.f/128.f),mul(num((color>>16)&255),1.f/128.f),mul(num(i<2?c.background_alpha_top:c.background_alpha_bottom),1.f/128.f)});}
    array(header,UiPacketOperationKind::Colors,0x1aeb48,5,std::move(colors));std::vector<V> coordinates;
    for(unsigned i=0;i<16;++i){const unsigned at=base+i*16;const auto selector=std::uint32_t(integer(value(sprite_[1],at+8)));
        if(selector<32){const auto index=selector*4;const auto x0=num(si(word(0xc+index)));auto y0=num(si(word(0x8c+index)));const auto x1=add(x0,num(si(word(0x10c+index)))),y1=add(y0,num(si(word(0x18c+index))));
            if(value(sprite_[0],at+8)!=0&&value(sprite_[0],at+4)<24)y0=add(y0,sub(24.f,num(integer(value(sprite_[0],at+4))%24)));
            quad_uv(coordinates,integer(x0),integer(y0),integer(x1),integer(y1));
        }else if(selector==34){const auto packed=std::uint32_t(integer(value(sprite_[1],at+12))),dim=std::uint32_t(integer(value(sprite_[0],at+8)));
            const auto x=num(si(packed)>>16),y=num(packed&65535);quad_uv(coordinates,integer(x),integer(y),integer(add(x,num(si(dim)>>16))),integer(add(y,num(dim&65535))));
        }else for(unsigned j=0;j<4;++j)coordinates.push_back({uv(0,false),uv(0,false),0,0});
    }
    array(header,UiPacketOperationKind::UV,0x1aed70,64,std::move(coordinates));control(header,0x1ad240);
    for(unsigned i=0;i<16;++i){const unsigned at=base+i*16;const auto selector_float=value(sprite_[1],at+8);if(selector_float==-1.f)continue;
        const auto selector=std::uint32_t(integer(selector_float));auto& o=packet.elements[i].emplace();control(o,0x1ad1f8);std::vector<V> positions;
        const auto ox=value(*this,0x560),oy=value(*this,0x564),oz=value(*this,0x568);
        float x0,y0,x1,y1,z0,z1;bool ordinary_quad=true;
        if(selector<32){const auto depth=value(sprite_[0],at+8);x0=sub(0,ox);y0=sub(depth!=0?24.f:0.f,oy);x1=sub(value(sprite_[1],at),ox);y1=sub(value(sprite_[1],at+4),oy);z0=sub(depth,oz);z1=z0;}
        else if(selector==33){x0=-ox;y0=-oy;x1=sub(value(sprite_[1],at),ox);y1=sub(value(sprite_[1],at+4),oy);z0=sub(value(sprite_[0],at+8),oz);z1=z0;}
        else if(selector==34){const auto dim=std::uint32_t(integer(value(sprite_[0],at+8)));x0=-ox;y0=-oy;z0=-oz;z1=z0;x1=add(x0,mul(num(si(dim)>>16),value(sprite_[1],at)));y1=add(y0,mul(num(dim&65535),value(sprite_[1],at+4)));}
        else if(selector==32){x0=sub(value(sprite_[0],at),ox);y0=sub(value(sprite_[0],at+4),oy);z0=sub(value(sprite_[0],at+8),oz);x1=sub(value(sprite_[1],at),ox);y1=sub(value(sprite_[1],at+4),oy);z1=sub(value(sprite_[1],at+12),oz);ordinary_quad=false;positions={{x0,y0,z0,1},{x0,y0,z0,1},{x1,y1,z1,1},{x1,y1,z1,1}};}
        else {ordinary_quad=false;positions.assign(4,{0,0,0,0});}
        if(ordinary_quad)positions={{x0,y0,z0,1},{x1,y0,z0,1},{x1,y1,z0,1},{x0,y1,z0,1}};
        array(o,UiPacketOperationKind::Positions,0x1ae940,4,std::move(positions));
        //1a6908: each subpacket ad1f8/ad1e0 sets333be4=1 before ae940.
        o.back().first_positions_upload=true;control(o,0x1af978);const unsigned color=5+i,p=i*4;
        if(selector<32||selector==34){primitive(o,0x1af3e8,0x58);for(unsigned j=0;j<4;++j)vertex(o,j,color,p+j);}
        else if(selector==33){primitive(o,0x1af3e8,0x48);vertex(o,0,1,0);vertex(o,1,1,0);vertex(o,2,2,0);vertex(o,3,2,0);}
        else if(selector==32){primitive(o,0x1af848,0x48,4);for(unsigned j=0;j<4;++j)vertex(o,j,color,0);
            //1a73d0 jumps to finalization after the fourth vertex.
        }else{primitive(o,0x1af848,0x48,4);vertex(o,1,color,0);vertex(o,1,color,0);vertex(o,2,color,0);vertex(o,2,color,0);}
        control(o,0x1afa00);control(o,0x1ad240);
    }
    sprite_packets_[{group,side}]=std::move(packet);put(0x3c08+group*4,word(0x3c08+group*4)&~8u);
    const auto& stored=sprite_packets_.at({group,side});for(unsigned i=0;i<16;++i){const auto at=0x5ec0+side*0x800+group*0x40+i*4;
        if(stored.elements[i])known_.reset(at/4);else put(at,0xffffffff);}
    put(0x3e28,group+1);
}
}
