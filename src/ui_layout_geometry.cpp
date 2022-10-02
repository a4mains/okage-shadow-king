#include "okage/ui_layout_geometry.hpp"
#include <bit>
#include <cfenv>
#include <optional>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
using I=std::int32_t;
I si(std::uint32_t x){return __builtin_bit_cast(I, (x));}
I add(I a,I b){return si(std::uint32_t(a)+std::uint32_t(b));}
I sub(I a,I b){return si(std::uint32_t(a)-std::uint32_t(b));}
I magnitude(I x){return x<0?sub(0,x):x;}
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
float finite(float x) {
    auto bits=__builtin_bit_cast(std::uint32_t, (x)),exponent=bits&0x7f800000u;
    check(exponent!=0x7f800000u&&(exponent!=0||(bits&0x7fffffu)==0),"UI geometry exceptional float");return x;
}
struct Rounding {
    std::fenv_t saved;
    Rounding(){check(std::fegetenv(&saved)==0,"UI geometry save fenv");if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI geometry rounding");}}
    ~Rounding(){std::fesetenv(&saved);}
};
float number(I x){volatile float out=static_cast<float>(x);return finite(out);}
float ratio(I a,I b) {
    check(b!=0,"UI geometry zero texture dimension");
    volatile float numerator=number(a),denominator=number(b),out=numerator/denominator;
    return finite(out);
}
float multiply(float a,float b){
    volatile float x=finite(a),y=finite(b);
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI geometry clear exceptions");
    volatile float out=x*y;
    check(!(std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW)&(FE_OVERFLOW|FE_UNDERFLOW)),"UI geometry float overflow/underflow");return finite(out);
}
float absolute(float f){return f<0?-f:f;}
struct Rect {I u,v,w,h,index;};
struct Generator {
    const RuwResource& resource;I y_offset;UiLayoutSpriteBatch result;
    Rect rect(I index) const {
        check(index>=0&&std::size_t(index)<resource.styles().size(),"UI geometry style index");
        const auto& s=resource.styles()[unsigned(index)];return {si(s[0]),si(s[1]),si(s[2]),si(s[3]),index};
    }
    I default_texture() const {
        unsigned left=4;
        for(const auto& group:resource.templates()) {
            if(left<group.size())return si(group[left].words[7]);left-=group.size();
        }
        throw std::runtime_error("UI geometry missing default template record4");
    }
    void scales(float x,float y){result.final_style.scale_x=x;result.final_style.scale_y=y;}
    void color(std::uint32_t rgba,bool convert){result.final_style.color={rgba,convert?UiLayoutColorConversion::Convert19bdf8:UiLayoutColorConversion::Direct};}
    void emit(I x,I y,const Rect& t,unsigned piece=0) {
        result.sprites.push_back({x,sub(y,y_offset),t.w,t.h,t.u,t.v,t.index,piece,result.final_style});
    }
};
}
std::uint32_t ui_layout_color_rgba(UiLayoutColor color) {
    if(color.conversion==UiLayoutColorConversion::Direct)return color.rgba;
    check(color.conversion==UiLayoutColorConversion::Convert19bdf8,"UI geometry color conversion");
    const auto rgb=color.rgba&0xffffffu,alpha=color.rgba>>24;
    //19be14 ADDU and19be1c SLTU form one packed interval, not per-byte tests.
    if(rgb-0xf8f8f8u<=0x70707u)return ((alpha?alpha-1:0)<<24)|0x808080u;
    std::uint32_t out=color.rgba&0xff000000u;
    for(unsigned shift=0;shift<24;shift+=8) {
        const auto c=(color.rgba>>shift)&255u;out|=((c>16?c-16:0)>>1)<<shift;
    }
    return out;
}
UiLayoutSpriteBatch layout_ruw_sprites(const RuwResource& resource,const RuwWidget& widget,
                                     I offset,UiLayoutSpriteStyle incoming) {
    Generator g{resource,offset,{{},incoming}};
    const auto& raw=widget.words;
    if(!(raw[4]&1))return g.result;
    const I type=si(raw[5]);check(type==0||type==2||type==3,"UI geometry non-sprite subtype");
    const I fallback=g.default_texture();
    const auto* runtime=widget.runtime?&widget.runtime->words:nullptr;
    const auto field=[&](unsigned raw_index,unsigned runtime_index){return runtime?(*runtime)[runtime_index]:raw[raw_index];};
    I x=si(field(11,6)),y=si(field(12,7)),w=si(field(13,8)),h=si(field(14,9));
    const auto primary=field(2,4),secondary=field(3,5);
    Rounding rounding;
    if(type==0) { //1a1d18..1a1fb0
        const I selector=si(field(7,0));if(selector==-1)return g.result;
        auto texture=g.rect(selector<-1?fallback:selector);
        g.color(primary,true);
        float sx=finite(__builtin_bit_cast(float, (field(0,2)))),sy=finite(__builtin_bit_cast(float, (field(1,3))));
        if(selector<-1) {
            const auto target=g.rect(magnitude(add(selector,2)));
            sx=multiply(sx,ratio(target.w,texture.w));sy=multiply(sy,ratio(target.h,texture.h));
        }
        g.scales(sx,sy);g.emit(x,y,texture);return g.result;
    }
    if(type==2) {
        const auto t=g.rect(fallback);g.color(primary,false);
        g.scales(ratio(w?w:1,t.w),ratio(w?2:h,t.h));
        g.emit(sub(x,14),sub(y,1),t);g.scales(1,1);return g.result;
    }
    const I split=si(raw[6]),d=magnitude(split);const bool horizontal=split>=0;
    g.color(primary,true);
    const I group=si(raw[7]);
    check(group>=0&&std::size_t(group)<resource.templates().size(),"UI geometry template group");
    const auto& children=resource.templates()[unsigned(group)];
    if(!(raw[4]&0x80)) {
        std::array<std::optional<Rect>,4> corners;
        const auto corner=[&](unsigned i)->const Rect& {check(bool(corners[i]),"UI geometry uninitialized template corner");return *corners[i];};
        for(unsigned i=0;i<children.size();++i) {
            const auto t=g.rect(si(children[i].words[7]));
            if(children[i].words[5]!=0)continue;
            I px=x,py=y;g.scales(1,1);
            switch(i) {
            case 0:corners[0]=t;break;
            case 1:corners[1]=t;px=sub(add(x,w),t.w);break;
            case 2:corners[2]=t;py=sub(add(y,h),t.h);break;
            case 3:corners[3]=t;px=sub(add(x,w),t.w);py=sub(add(y,h),t.h);break;
            case 4:g.scales(absolute(ratio(sub(w,add(corner(0).w,corner(1).w)),t.w)),absolute(ratio(h,t.h)));px=add(x,corner(0).w);break;
            case 5:g.scales(absolute(ratio(corner(0).w,t.w)),absolute(ratio(sub(h,add(corner(0).h,corner(2).h)),t.h)));py=add(y,corner(0).h);break;
            case 6:g.scales(absolute(ratio(corner(1).w,t.w)),absolute(ratio(sub(h,add(corner(1).h,corner(3).h)),t.h)));px=sub(add(x,w),corner(1).w);py=add(y,corner(0).h);break;
            default:px=py=-3000;
            }
            if(px!=-3000||py!=-3000)g.emit(px,py,t,i);
        }
        g.scales(1,1);return g.result;
    }
    //1a22b0..1a2468: gather first five raw selectors without child type/show gates.
    check(children.size()>=5,"UI geometry split panel needs five template records");
    std::array<Rect,5> t;
    for(unsigned i=0;i<5;++i)t[i]=g.rect(si(children[i].words[7]));
    I q=d==0?0:horizontal?(d==w?0:1):(d==h?0:2);
    unsigned count=7;
    if(q==1) {
        count=8;if(d<=t[0].w){q=3;count=11;}if(d>=sub(w,t[1].w)){q=4;count=11;}
    } else if(q==2) {
        count=8;if(d<=t[1].h){q=5;count=11;}if(d>=sub(h,t[3].h)){q=6;count=11;}
    }
    for(unsigned i=0;i<count;++i) {
        if(h<=add(t[0].h,t[2].h)&&i>1)break;
        auto texture=t[i<4?i:4];I px=x,py=y;
        const auto one=[&]{g.scales(1,1);};
        const auto first=[&]{g.color(primary,true);};const auto second=[&]{g.color(secondary,true);};
        if(!horizontal) {
            switch(i) {
            case 0:one();first();break;
            case 1:px=sub(add(x,w),texture.w);one();second();break;
            case 2:py=sub(add(y,h),texture.h);one();first();break;
            case 3:px=sub(add(x,w),texture.w);py=sub(add(y,h),texture.h);one();second();break;
            case 4:
                g.scales(ratio(d,texture.w),ratio(sub(h,sub(add(y,t[0].h),t[2].h)),texture.h));
                py=add(y,t[0].h);first();break;
            default:px=py=-3000;
            }
        } else switch(i) {
        case 0:
            first();
            if(h>add(t[0].h,t[2].h)){if(q==3)texture.w=d;one();if(q==0&&d==0)second();}
            else {texture=t[4];g.scales(ratio(d,texture.w),ratio(h,texture.h));}
            break;
        case 1:
            second();
            if(h>add(t[0].h,t[2].h)) {
                px=sub(add(x,w),texture.w);one();
                if(q==4){texture.w=sub(w,d);px=add(x,d);texture.u=sub(add(t[1].u,t[1].w),texture.w);}
                if(q==0&&d==w)first();
            } else {texture=t[4];g.scales(ratio(sub(w,d),texture.w),ratio(h,texture.h));px=add(x,d);}
            break;
        case 2:py=sub(add(y,h),texture.h);if(q==3)texture.w=d;one();first();if(q==0&&d==0)second();break;
        case 3:
            second();one();py=sub(add(y,h),texture.h);px=sub(add(x,w),texture.w);
            // Original uses T1's U/width here while retaining T3's V/height.
            if(q==4){texture.w=sub(w,d);px=add(x,d);texture.u=sub(add(t[1].u,t[1].w),texture.w);}
            if(q==0&&d==w)first();break;
        case 4:
            px=add(x,t[0].w);
            if(q==0){g.scales(ratio(sub(sub(w,t[0].w),t[1].w),texture.w),ratio(h,texture.h));if(d==w)first();else second();}
            else {I span=t[0].w;if(t[0].w<d)span=sub(w,t[1].w)<d?sub(sub(w,t[0].w),t[1].w):sub(d,t[0].w);
                g.scales(ratio(span,texture.w),ratio(h,texture.h));first();}
            break;
        case 5:
            if(q==0){px=sub(add(x,w),t[1].w);py=add(y,t[0].h);g.scales(ratio(t[1].w,texture.w),ratio(sub(sub(h,t[0].h),t[2].h),texture.h));if(d==w)first();else second();}
            else {I span;
                if(t[0].w<d){if(sub(w,t[1].w)<d){span=sub(sub(w,sub(w,t[1].w)),t[1].w);px=sub(add(x,w),t[1].w);}else{px=add(x,d);span=sub(sub(w,d),t[1].w);}}
                else{span=sub(sub(w,t[0].w),t[1].w);px=add(x,t[0].w);}
                g.scales(ratio(span,texture.w),ratio(h,texture.h));second();}
            break;
        case 6:
            py=add(y,t[0].h);g.scales(ratio(t[0].w,texture.w),ratio(sub(sub(h,t[0].h),t[2].h),texture.h));
            if(q==0){if(d==w)first();else second();}else{first();if(q==3)g.result.final_style.scale_x=ratio(d,texture.w);}
            break;
        case 7:
            px=sub(add(x,w),t[1].w);py=add(y,t[0].h);
            g.scales(ratio(t[1].w,texture.w),ratio(sub(sub(h,t[0].h),t[2].h),texture.h));second();
            if(q==4){first();g.result.final_style.scale_x=ratio(sub(t[1].w,sub(w,d)),texture.w);}break;
        case 8:case 9:
            if(q==3){texture=t[i==8?0:2];px=add(x,d);if(i==9)py=sub(add(y,h),texture.h);texture.w=sub(texture.w,d);texture.u=add(texture.u,d);one();second();}
            else if(q==4){texture=t[i==8?1:3];if(i==9)py=sub(add(y,h),texture.h);px=sub(add(x,w),texture.w);texture.w=sub(texture.w,sub(w,d));one();first();}
            else px=py=-3000;
            break;
        case 10:
            if(q==3){
                //1a2d0c writes the initial scale before overwriting it.
                g.scales(ratio(t[0].w,texture.w),ratio(sub(sub(h,t[0].h),t[2].h),texture.h));second();
                g.result.final_style.scale_x=ratio(sub(t[0].w,d),texture.w);px=add(x,d);py=add(y,t[0].h);
            } else if(q==4){
                const float ignored=ratio(t[1].w,texture.w);(void)ignored;
                g.scales(ratio(sub(w,d),texture.w),ratio(sub(sub(h,t[0].h),t[2].h),texture.h));second();px=add(x,d);py=add(y,t[0].h);
            }else px=py=-3000;
            break;
        default:throw std::runtime_error("UI geometry split recipe extent");
        }
        if(px!=-3000||py!=-3000)g.emit(px,py,texture,i);
    }
    g.scales(1,1);return g.result;
}
}
