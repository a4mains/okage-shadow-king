#include "okage/ui_draw.hpp"
#include "okage/ui_matrix.hpp"
#include <bit>
#include <cfenv>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
std::int32_t si(std::uint32_t v){return __builtin_bit_cast(std::int32_t, (v));}
float normal(float f){const auto bits=__builtin_bit_cast(std::uint32_t, (f)),exp=bits&0x7f800000;
    check(exp!=0x7f800000&&(exp!=0||(bits&0x7fffff)==0),"UI draw exceptional float");return f;}
float value(const UiWindow& w,unsigned at){return normal(__builtin_bit_cast(float, (w.word(at))));}
float value(const UiWindowBuffer& w,unsigned at){return normal(__builtin_bit_cast(float, (w.word(at))));}
struct Rounding {
    std::fenv_t saved;
    Rounding(){check(std::fegetenv(&saved)==0,"UI draw save rounding");
        if(std::fesetround(FE_TOWARDZERO)){std::fesetenv(&saved);throw std::runtime_error("UI draw rounding");}}
    ~Rounding(){std::fesetenv(&saved);}
};
float arithmetic(float a,float b,char kind){
    volatile float x=normal(a),y=normal(b),out;
    check(std::feclearexcept(FE_OVERFLOW|FE_UNDERFLOW)==0,"UI draw clear float flags");
    if(kind=='+')out=x+y;else if(kind=='-')out=x-y;else out=x*y;
    check(!std::fetestexcept(FE_OVERFLOW|FE_UNDERFLOW),"UI draw float overflow/underflow");return normal(out);
}
std::array<float,3> xyz(const UiWindow& w,unsigned at){return {value(w,at),value(w,at+4),value(w,at+8)};}
bool duplicate_history(const UiWindow& w,unsigned h){
    for(unsigned at:{0x3f0u,0x3f4u,0x3f8u,0x2f0u,0x2f4u,0x2f8u})
        if(value(w,at+h*16)!=value(w,at))return false;
    return true;
}
}
struct UiDrawBuilder {
    UiDrawList result;
    template<class T> void emit(T value){result.commands_.emplace_back(std::move(value));}
    void vectors(UiDrawVectors::Kind kind,unsigned first,std::vector<Float4> values){
        emit(UiDrawVectors{kind,first,std::move(values)});
    }
    void matrix(const Matrix4& m){emit(UiDrawMatrix{m});}
    UiDrawList build(std::span<const std::shared_ptr<UiWindow>,8> registry,
        UiDrawState& state,const ShadowScreenParameters& projection,std::span<const float> sine){
        Rounding rounding;
        // EE ADDIU wraps before signed remainder. C++ remainder truncates toward0.
        state.background_alpha_top=si(std::uint32_t(state.background_alpha_top)+1)%128;
        state.background_alpha_bottom=(state.background_alpha_top+128)%256;
        emit(UiDrawControl::Begin);emit(UiDrawProjection{projection});
        emit(UiDrawControl::CullModeZero);emit(UiDrawControl::BeginGif);
        for(auto [r,v]:std::initializer_list<std::pair<unsigned,std::uint64_t>>{
            {0x49,0},{0x47,0x5000d},{0x3b,0x100008080ull},{0x4a,0},{0x42,0x48},
            {0x40,0xe0000002800000ull},{0x14,0x60},{8,5}})emit(UiDrawRegister{r,v});
        unsigned glyph_pass_counter=0;
        for(unsigned slot=0;slot<7;++slot){
            if(!registry[slot])continue;
            auto& w=*registry[slot];
            if(value(w,0x27c)==0&&value(w,0x270)==0)continue;
            const unsigned glyph_passes=value(w,0x54c)>0?2:1;
            const auto flags=w.word(0x4f8);const bool extra=(flags&0x40)!=0;
            check(w.word(0x220)==0xffffffffu,"UI draw alternate framebuffer is unrecovered");
            emit(UiDrawCurrentFramebuffer{});
            if(w.word(0x4f4)==1)w.put(0x4f4,0);
            unsigned histories=1;
            if(flags&1){
                const auto last=si(w.word(0x4f0));
                check(last>=0&&last<16,"UI draw history bounds");
                unsigned duplicates=0;
                for(unsigned h=1;h<=unsigned(last);++h)if(duplicate_history(w,h))++duplicates;
                if(duplicates!=unsigned(last))histories=unsigned(last)+1;
            }
            // Copy each used cache at most once; every replay shares that immutable snapshot.
            std::map<std::pair<unsigned,unsigned>,std::shared_ptr<const UiNativePacket>> glyphs,sprites;
            const auto snapshot=[](auto& cache,const UiNativePacket* packet,unsigned group,unsigned side){
                const auto key=std::pair{group,side};auto it=cache.find(key);
                if(it==cache.end())it=cache.emplace(key,std::make_shared<const UiNativePacket>(*packet)).first;
                return it->second;
            };
            for(unsigned h=0;h<histories;++h){
                if(h&&duplicate_history(w,h))continue;
                if(((w.word(0x25c)>>4)&3)==0)continue;
                const Float4 auxiliary{128,128,128,value(w,0x270)};
                vectors(UiDrawVectors::Kind::Auxiliary,0,{auxiliary});
                const auto position=xyz(w,0x2f0+h*16);
                const auto rotation=(flags&8)?xyz(w,0x3f0+h*16):std::array<float,3>{};
                const auto parent=ui_sprite_parent_matrix(position,rotation,(flags&8)!=0,sine);
                matrix(parent);
                const auto sprite_count=si(w.word(0xcc4));
                check(sprite_count<=si(w.word(0x3c04)),"UI draw sprite cache count exceeds allocation");
                const unsigned sprite_side=w.word(0x2690)!=0;
                for(int g=0;g<sprite_count;++g){
                    if(w.word(0x3c08+unsigned(g)*4)&8)continue;
                    const auto* packet=w.sprite_packet(unsigned(g),sprite_side);
                    check(packet,"UI draw active sprite cache has no native packet");
                    for(unsigned element=0;element<16;++element){
                        if(!packet->elements[element])continue;
                        const auto offset=unsigned(g)*256+element*16;
                        const auto alpha=value(w,0x3e4c+offset);if(alpha==0)continue;
                        const auto owner=snapshot(sprites,packet,unsigned(g),sprite_side);
                        for(unsigned pass=0;pass<(extra?3u:1u);++pass){
                            emit(UiDrawReplay{UiDrawPacketFamily::SpriteHeader,slot,unsigned(g),sprite_side,element,h,pass,owner});
                            auto a=alpha;if(pass)a=arithmetic(a,__builtin_bit_cast(float, (0x3ed31dd5u)),'*');
                            vectors(UiDrawVectors::Kind::Colors,5+element,
                                {{value(w,0x3e40+offset),value(w,0x3e44+offset),value(w,0x3e48+offset),a}});
                            const auto& positions=w.sprite_buffer(0);
                            const auto x=value(positions,offset);auto y=value(positions,offset+4);
                            float z=0;
                            if(pass){y=arithmetic(y,pass==2?1.f:0.f,'-');
                                y=arithmetic(y,pass==1?1.f:0.f,'+');z=-.0128f;}
                            matrix(ui_sprite_element_matrix(parent,{x,y,z},sine));
                            emit(UiDrawReplay{UiDrawPacketFamily::SpriteElement,slot,unsigned(g),sprite_side,element,h,pass,owner});
                        }
                    }
                }
                vectors(UiDrawVectors::Kind::Auxiliary,0,{auxiliary,auxiliary});
                auto glyph_position=position;
                const float factor=glyph_pass_counter==1&&glyph_passes==2?1.f:0.f;
                for(unsigned j=0;j<3;++j)glyph_position[j]=arithmetic(position[j],arithmetic(value(w,0x530+j*4),factor,'*'),'+');
                matrix(ui_glyph_parent_matrix(glyph_position,rotation,(flags&8)!=0,sine));
                const unsigned glyph_side=w.word(0x268c)!=0;
                for(unsigned g=0;g<w.word(0x3c00);++g){
                    if(si(w.word(0xcc0))<=0||(w.word(0x3c08+g*4)&4))continue;
                    const auto* packet=w.glyph_packet(g,glyph_side);
                    check(packet,"UI draw active glyph cache has no native packet");
                    const auto owner=snapshot(glyphs,packet,g,glyph_side);
                    for(glyph_pass_counter=0;glyph_pass_counter<glyph_passes;++glyph_pass_counter)
                        emit(UiDrawReplay{UiDrawPacketFamily::Glyph,slot,g,glyph_side,0,h,glyph_pass_counter,owner});
                    // Source computes stack colors here but never uploads them.
                }
            }
        }
        emit(UiDrawCurrentFramebuffer{});emit(UiDrawControl::EndGif);
        emit(UiDrawControl::FlushVu);emit(UiDrawControl::End);emit(UiDrawControl::Queue);
        emit(UiDrawControl::QueueTextState);
        return std::move(result);
    }
};
UiDrawList build_ui_draw_list(std::span<const std::shared_ptr<UiWindow>,8> registry,
    UiDrawState& state,const ShadowScreenParameters& projection,std::span<const float> sine){
    return UiDrawBuilder{}.build(registry,state,projection,sine);
}
}
