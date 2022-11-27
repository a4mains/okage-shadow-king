#include "okage/ui_geometry.hpp"
#include "okage/font.hpp"
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
constexpr unsigned capacity=97; // VU26/87/e8/149/1aa/20b, each61 vectors.
struct PositionUpload {
    UiProjectionParameters projection;
    Matrix4 matrix;
    bool first;
};
struct Position { Float4 value; std::shared_ptr<const PositionUpload> upload; };
bool same_projection(const UiProjectionParameters& a,const UiProjectionParameters& b){
    for(unsigned i=0;i<a.size();++i)for(unsigned j=0;j<4;++j)
        if(__builtin_bit_cast(std::uint32_t, (a[i][j]))!=__builtin_bit_cast(std::uint32_t, (b[i][j])))return false;
    return true;
}
struct Resolver {
    UiGeometryList result;
    std::optional<UiProjectionParameters> projection;
    std::optional<Matrix4> matrix;
    std::array<std::optional<Position>,capacity> positions;
    std::array<std::optional<Float4>,capacity> colors,auxiliary;
    std::array<std::optional<std::array<float,2>>,capacity> uv;
    std::optional<UiGeometryTexture> texture;
    bool cull_zero=false;
    template<class T> void emit(T v){result.commands.emplace_back(std::move(v));}
    static unsigned bounded(std::uint64_t n){check(n<capacity,"UI geometry cache index bounds");return unsigned(n);}
    static unsigned upload_first(std::uint64_t first,std::size_t count){
        check(first<=capacity&&count<=capacity-first,"UI geometry cache upload bounds");return unsigned(first);
    }
    void vectors(UiDrawVectors::Kind kind,unsigned first,const std::vector<Float4>& values){
        upload_first(first,values.size());
        check(kind==UiDrawVectors::Kind::Colors||kind==UiDrawVectors::Kind::Auxiliary,"UI geometry vector kind");
        auto& cache=kind==UiDrawVectors::Kind::Colors?colors:auxiliary;
        for(unsigned i=0;i<values.size();++i)cache[first+i]=values[i]; // Direct UNPACK bit copies.
    }
    void packet(const UiDrawReplay& source){
        check(bool(source.packet),"UI geometry missing packet owner");
        check(source.window<7&&source.side<2&&source.element<16,"UI geometry replay metadata bounds");
        const auto& packet=*source.packet;
        const auto* ops=&packet.operations;
        if(source.family==UiDrawPacketFamily::SpriteElement){
            check(packet.elements[source.element].has_value(),"UI geometry missing sprite element");
            ops=&*packet.elements[source.element];
        }else check(source.family==UiDrawPacketFamily::Glyph||source.family==UiDrawPacketFamily::SpriteHeader,"UI geometry packet family");
        std::optional<unsigned> primitive;
        std::array<std::array<unsigned,4>,4> indices{};
        unsigned pending=0;
        const auto finish=[&]{check(pending==0,"UI geometry incomplete primitive");primitive.reset();};
        for(const auto& op:*ops){
            const auto& a=op.arguments;
            check(op.kind==UiPacketOperationKind::Positions||!op.first_positions_upload.has_value(),"UI geometry position metadata on non-position operation");
            if(op.kind==UiPacketOperationKind::IndexedVertex){
                check(op.source_helper==0x1af908&&primitive.has_value()&&op.values.empty(),"UI geometry indexed vertex without primitive");
                for(unsigned i=0;i<4;++i)indices[pending][i]=bounded(a[i]);
                if(++pending==4){quad(source,*primitive,indices);pending=0;}
                continue;
            }
            finish();
            switch(op.kind){
            case UiPacketOperationKind::Positions: {
                check(op.source_helper==0x1ae940&&op.first_positions_upload.has_value(),"UI geometry missing compiled position metadata");
                check(projection.has_value()&&matrix.has_value(),"UI geometry position upload without projection/matrix");
                check(a[1]==op.values.size()&&a[2]==0&&a[3]==0,"UI geometry position upload extent");
                const auto first=upload_first(a[0],op.values.size());
                const auto owner=std::make_shared<const PositionUpload>(PositionUpload{*projection,*matrix,*op.first_positions_upload});
                for(unsigned i=0;i<op.values.size();++i)positions[first+i]=Position{op.values[i],owner};
                break;}
            case UiPacketOperationKind::Colors:
            case UiPacketOperationKind::AuxiliaryVectors: {
                const bool color=op.kind==UiPacketOperationKind::Colors;
                check(op.source_helper==(color?0x1aeb48u:0x1aeb10u),"UI geometry unsupported vector upload helper");
                check(a[1]==op.values.size()&&a[2]==0&&a[3]==0,"UI geometry vector upload extent");
                vectors(color?UiDrawVectors::Kind::Colors:UiDrawVectors::Kind::Auxiliary,upload_first(a[0],op.values.size()),op.values);break;}
            case UiPacketOperationKind::UV: {
                check(op.source_helper==0x1aed70&&a[1]==op.values.size()&&a[2]==0&&a[3]==0,"UI geometry UV upload extent/helper");
                const auto first=upload_first(a[0],op.values.size());
                for(unsigned i=0;i<op.values.size();++i)uv[first+i]=std::array<float,2>{op.values[i][0],op.values[i][1]};
                break;}
            case UiPacketOperationKind::Register:
                check(op.source_helper==0x1aedd8&&a[0]<=0x7f&&a[2]==0&&a[3]==0&&op.values.empty(),"UI geometry register schema");
                // TEX0 requires retained host ownership, supplied by Texture.
                check(a[0]!=6&&a[0]!=7&&a[0]!=0x4c&&a[0]!=0x4d,"UI geometry raw owned GS register");
                emit(UiDrawRegister{unsigned(a[0]),a[1]});break;
            case UiPacketOperationKind::Texture: {
                check(op.source_helper==0x1aedd8&&a[0]==6&&a[1]<=1&&op.values.empty(),"UI geometry texture schema");
                UiGeometryTexture t;t.tex0=a[3];
                check(t.tex0==0x20000006a8000000ull,"UI geometry unsupported TEX0 layout");
                if(a[1]){check(bool(packet.window_texture)&&a[2]==0xffffffffu,"UI geometry missing window texture owner");t.kind=UiGeometryTexture::Kind::Window;t.window=packet.window_texture;}
                else{check(packet.fonts&&a[2]<packet.fonts->size(),"UI geometry missing font texture owner");t.kind=UiGeometryTexture::Kind::Font;t.font_index=unsigned(a[2]);t.fonts=packet.fonts;}
                texture=t;emit(std::move(t));break;}
            case UiPacketOperationKind::Primitive:
                check(op.values.empty()&&a[2]==0&&a[3]==0,"UI geometry primitive schema");
                check(op.source_helper!=0x1af848,"UI geometry line-strip projection is unrecovered");
                check(op.source_helper==0x1af3e8&&a[1]==0&&(a[0]==0x48||a[0]==0x58),"UI geometry unsupported primitive");
                primitive=unsigned(a[0])|5u;break;
            case UiPacketOperationKind::CompilerBoundary:
                check(op.values.empty()&&a[1]==0&&a[2]==0&&a[3]==0,"UI geometry compiler control schema");
                if(op.source_helper==0x1ad268){check(a[0]==0,"UI geometry unsupported cull mode");cull_zero=true;}
                else check(a[0]==0&&(op.source_helper==0x1ad1f8||op.source_helper==0x1af978||op.source_helper==0x1afa00||op.source_helper==0x1ad240),"UI geometry unsupported compiler boundary");
                // Compiler-only state was captured at compilation, not replayed.
                break;
            default: throw std::runtime_error("UI geometry unknown packet operation");
            }
        }
        finish();
    }
    void quad(const UiDrawReplay& source,unsigned prim,const std::array<std::array<unsigned,4>,4>& indices){
        check(cull_zero&&projection.has_value(),"UI geometry missing cull/projection state");
        std::array<UiProjectionVertex,4> inputs;
        std::shared_ptr<const PositionUpload> upload;
        const bool textured=(prim&0x10)!=0;
        for(unsigned i=0;i<4;++i){
            const auto& ix=indices[i];check(positions[ix[0]].has_value(),"UI geometry uninitialized position cache");
            const auto& pos=*positions[ix[0]];
            if(!upload)upload=pos.upload;
            check(upload==pos.upload,"UI geometry mixed position uploads are unrecovered");
            check(colors[ix[2]].has_value()&&auxiliary[ix[1]].has_value(),"UI geometry uninitialized color/auxiliary cache");
            inputs[i]={pos.value,*colors[ix[2]],*auxiliary[ix[1]],{}};
            if(textured){check(uv[ix[3]].has_value(),"UI geometry uninitialized UV cache");inputs[i].uv=*uv[ix[3]];}
        }
        check(same_projection(*projection,upload->projection),"UI geometry projection changed after position upload");
        if(textured)check(texture.has_value(),"UI geometry textured primitive without texture owner");
        auto projected=ui_project_quad(upload->projection,upload->matrix,inputs,textured,upload->first);
        if(projected.emitted)emit(UiResolvedPrimitive{source,prim,std::move(projected),textured?texture:std::nullopt});
    }
    UiGeometryList run(std::span<const UiDrawCommand> commands){
        for(const auto& c:commands)std::visit([&](const auto& v){consume(v);},c);
        return std::move(result);
    }
    void consume(UiDrawControl c){if(c==UiDrawControl::CullModeZero)cull_zero=true;emit(c);}
    void consume(const UiDrawRegister& r){check(r.index<=0x7f&&r.index!=6&&r.index!=7&&r.index!=0x4c&&r.index!=0x4d,"UI geometry raw owned GS register");emit(r);}
    void consume(UiDrawCurrentFramebuffer r){emit(r);}
    void consume(const UiDrawProjection& p){projection=p.value;}
    void consume(const UiDrawMatrix& m){matrix=m.value;}
    void consume(const UiDrawVectors& v){vectors(v.kind,v.first,v.values);}
    void consume(const UiDrawReplay& r){packet(r);}
};
}
UiGeometryList resolve_ui_geometry(std::span<const UiDrawCommand> commands){return Resolver{}.run(commands);}
UiGeometryList resolve_ui_geometry(const UiDrawList& list){return resolve_ui_geometry(list.commands());}
}
