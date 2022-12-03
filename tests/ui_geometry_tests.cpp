#include "okage/ui_geometry.hpp"
#include "okage/font.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace okage;
void check(bool v){if(!v)throw std::runtime_error("UI geometry test failure");}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught);}
Matrix4 identity(){return {{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};}
UiProjectionParameters projection(){return {{{1,1,1,0},{0,0,0,1},{1,1,1,0},{0,0,0,0},
    {2048,2048,64,0},{320,112,32,128},{1728,1936,-1,0},{2368,2160,1,1}}};}
UiPacketOperation operation(UiPacketOperationKind kind,unsigned helper,std::array<std::uint64_t,4> args={},std::vector<Float4> values={}){
    return {kind,helper,args,std::move(values),std::nullopt};
}
std::shared_ptr<UiNativePacket> packet(bool textured=true){
    auto p=std::make_shared<UiNativePacket>();p->fonts=std::make_shared<const std::vector<Font>>(1);
    auto pos=operation(UiPacketOperationKind::Positions,0x1ae940,{0,4},{{-.5f,-.5f,0,1},{.5f,-.5f,0,1},{.5f,.5f,0,1},{-.5f,.5f,0,1}});
    pos.first_positions_upload=true;p->operations.push_back(pos);
    p->operations.push_back(operation(UiPacketOperationKind::Colors,0x1aeb48,{2,1},{{.5f,.25f,2,1}}));
    // Unpacked XY only: synthetic incidental Z/W must not become STQ Z.
    p->operations.push_back(operation(UiPacketOperationKind::UV,0x1aed70,{4,1},{{-0.f,.75f,std::numeric_limits<float>::quiet_NaN(),99}}));
    if(textured)p->operations.push_back(operation(UiPacketOperationKind::Texture,0x1aedd8,{6,0,0,0x20000006a8000000ull}));
    p->operations.push_back(operation(UiPacketOperationKind::Register,0x1aedd8,{0x47,0x30003}));
    p->operations.push_back(operation(UiPacketOperationKind::Primitive,0x1af3e8,{textured?0x58u:0x48u}));
    for(unsigned i=0;i<4;++i)p->operations.push_back(operation(UiPacketOperationKind::IndexedVertex,0x1af908,{i,1,2,4}));
    return p;
}
UiDrawReplay replay(std::shared_ptr<const UiNativePacket> p){return {UiDrawPacketFamily::Glyph,0,0,0,0,0,0,std::move(p)};}
std::vector<UiDrawCommand> setup(){return {UiDrawControl::Begin,UiDrawProjection{projection()},UiDrawMatrix{identity()},UiDrawControl::CullModeZero,
    UiDrawRegister{0x47,0x5000d},UiDrawCurrentFramebuffer{},UiDrawVectors{UiDrawVectors::Kind::Auxiliary,1,{{128,128,128,64}}}};}
std::vector<UiResolvedPrimitive> draws(const UiGeometryList& list){std::vector<UiResolvedPrimitive> out;
    for(const auto& c:list.commands)if(const auto* p=std::get_if<UiResolvedPrimitive>(&c))out.push_back(*p);return out;}
void basic(){
    auto p=packet();auto commands=setup();commands.push_back(replay(p));commands.push_back(UiDrawControl::End);
    const auto result=resolve_ui_geometry(commands);const auto d=draws(result);check(d.size()==1&&d[0].prim==0x5d);
    const auto& v=d[0].quad.vertices[0];check(v.xyzf==std::array<std::uint32_t,4>{30208,31872,1024,2048});
    check(v.rgba==std::array<std::uint32_t,4>{0x4b000040,0x4b000020,0x4b0000ff,0x4b000040});
    check(__builtin_bit_cast(std::uint32_t, (v.stq[0]))==0x80000000u&&v.stq[1]==.75f&&v.stq[2]==1);
    check(d[0].texture&&d[0].texture->fonts==p->fonts&&d[0].source.packet==p);
    std::vector<std::uint64_t> tests;for(const auto& c:result.commands)if(const auto* r=std::get_if<UiDrawRegister>(&c))tests.push_back(r->value);
    check(tests==std::vector<std::uint64_t>{0x5000d,0x30003});
    check(std::holds_alternative<UiDrawCurrentFramebuffer>(result.commands[3]));
    check(std::get<UiDrawControl>(result.commands.back())==UiDrawControl::End);
    auto no=packet(false);no->fonts.reset();commands=setup();commands.push_back(replay(no));
    const auto untextured=draws(resolve_ui_geometry(commands));check(untextured.size()==1&&untextured[0].prim==0x4d&&!untextured[0].texture);
}
void cache_and_lifetime(){
    auto full=packet();auto uploaded=std::make_shared<UiNativePacket>(*full),primitive=std::make_shared<UiNativePacket>();
    primitive->operations.assign(uploaded->operations.end()-5,uploaded->operations.end());uploaded->operations.erase(uploaded->operations.end()-5,uploaded->operations.end());
    auto c=setup();c.push_back(replay(uploaded));auto moved=identity();moved[3][0]=.25f;c.push_back(UiDrawMatrix{moved});c.push_back(replay(primitive));
    const auto first=draws(resolve_ui_geometry(c));check(first[0].quad.vertices[0].screen[0]==1888); // Matrix captured at upload, not draw.
    c.push_back(UiDrawVectors{UiDrawVectors::Kind::Colors,2,{{1,1,1,.5f}}});c.push_back(replay(primitive));
    const auto second=draws(resolve_ui_geometry(c));check(second.size()==2&&second[1].quad.vertices[0].rgba[3]==0x4b000020);
    auto retained=second[1].texture->fonts;uploaded.reset();full.reset();c.clear();check(retained&&retained->size()==1);
    // Sprite header owns texture; element body owns the positions/primitive.
    auto sprite=packet();sprite->window_texture=std::make_shared<int>(42);
    sprite->elements[3]=sprite->operations;sprite->operations={operation(UiPacketOperationKind::Texture,0x1aedd8,{6,1,0xffffffffu,0x20000006a8000000ull})};
    auto& ops=*sprite->elements[3];ops.erase(ops.begin()+3);
    c=setup();auto header=replay(sprite);header.family=UiDrawPacketFamily::SpriteHeader;header.element=3;c.push_back(header);
    auto element=header;element.family=UiDrawPacketFamily::SpriteElement;c.push_back(element);
    const auto s=draws(resolve_ui_geometry(c));check(s.size()==1&&s[0].texture->window==sprite->window_texture);
}
void compiled_upload_metadata(){
    auto p=packet();for(auto& v:p->operations[0].values)v[2]=.25f;
    auto pr=projection();pr[0][2]=2;
    auto c=setup();c[1]=UiDrawProjection{pr};c.push_back(replay(p));
    const auto first=draws(resolve_ui_geometry(c));check(first[0].quad.vertices[0].clip[2]==.5f);
    //0xcac is the IBNE delay-slot upper instruction: column2 is transformed
    //even when first-upload initializes the aggregate clipping registers.
    p->operations[0].first_positions_upload=false;
    rejects([&]{resolve_ui_geometry(c);});
    // A replayed compiler reset never overrides explicit compiled metadata.
    p->operations.insert(p->operations.begin(),operation(UiPacketOperationKind::CompilerBoundary,0x1af978));
    rejects([&]{resolve_ui_geometry(c);});
}
void errors_and_rejection(){
    const auto bad=[](auto change){auto p=packet();change(*p);auto c=setup();c.push_back(replay(p));rejects([&]{resolve_ui_geometry(c);});};
    bad([](auto& p){p.operations[0].first_positions_upload.reset();});
    bad([](auto& p){p.operations[0].arguments[0]=96;});
    bad([](auto& p){p.operations[0].arguments[1]=5;});
    bad([](auto& p){p.operations.pop_back();});
    bad([](auto& p){p.operations.back().arguments[1]=2;});
    bad([](auto& p){p.operations.back().arguments[0]=97;});
    bad([](auto& p){p.operations[p.operations.size()-5].source_helper=0x1af848;});
    bad([](auto& p){p.fonts.reset();});
    bad([](auto& p){p.operations[0].values[0][2]=-2;}); // Depth clipping remains explicit failure.
    auto p=packet();for(auto& v:p->operations[0].values)v[0]=2;
    auto c=setup();c.push_back(replay(p));const auto rejected=resolve_ui_geometry(c);check(draws(rejected).empty());
    // Common rejection does not drop state changes such as cached TEST.
    check(std::get<UiDrawRegister>(rejected.commands.back()).value==0x30003);
    auto entire=packet();auto upload=std::make_shared<UiNativePacket>(*entire);auto body=std::make_shared<UiNativePacket>();
    body->operations.assign(upload->operations.end()-5,upload->operations.end());upload->operations.erase(upload->operations.end()-5,upload->operations.end());
    c=setup();c.push_back(replay(upload));auto pr=projection();pr[6][3]=1;c.push_back(UiDrawProjection{pr});c.push_back(replay(body));rejects([&]{resolve_ui_geometry(c);});
    // Different upload batches in one quad cannot silently borrow one matrix.
    c=setup();c.push_back(replay(upload));auto overwrite=std::make_shared<UiNativePacket>();overwrite->operations={upload->operations[0]};
    overwrite->operations[0].values.resize(1);overwrite->operations[0].arguments[1]=1;c.push_back(replay(overwrite));c.push_back(replay(body));rejects([&]{resolve_ui_geometry(c);});
}
}
int main(){try{basic();cache_and_lifetime();compiled_upload_metadata();errors_and_rejection();std::cout<<"UI geometry tests passed\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
