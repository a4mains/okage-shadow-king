#include "okage/collision.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
using Data=std::vector<std::uint8_t>;
void check(bool b,unsigned at=__builtin_LINE()){
    if(!b)throw std::runtime_error("Collision test failed at line "+std::to_string(at));
}
void put(Data& b,unsigned at,std::uint32_t v,unsigned size=4){for(unsigned i=0;i<size;++i)b.at(at+i)=v>>(i*8);}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Invalid XCL accepted");}
int main(){try {
    okage::CollisionMask filter;
    filter.event(0,2);check(filter.bits==4);filter.event(1,2);check(filter.bits==0);
    filter.event(0,-1);check(filter.bits==0x80000000);filter.event(3,63);check(filter.bits==0);
    filter.bits=0x108;filter.event(0x100,0);check(filter.bits==0x101);
    filter.event(0x100,0);check(filter.bits==0x102);
    filter.bits=0x80000000;filter.event(0x100,31);check(filter.bits==0x80000000);
    Data b(164);
    for(unsigned i=0;i<3;++i){put(b,i*4,8);put(b,12+i*4,3);put(b,48+i*4,1);}
    put(b,60,64);put(b,64,68);put(b,68,72);put(b,72,76);
    put(b,76,3,2);put(b,78,1,2);put(b,80,96);put(b,84,144);
    for(unsigned i=0;i<3;++i)put(b,96+i*16+12,__builtin_bit_cast(std::uint32_t, (1.f)));
    put(b,144+4,0xc000,2);put(b,144+10,0,2);put(b,144+12,1,2);put(b,144+14,2,2);
    // Absolute slot, contiguous run, byte skip, contiguous run, unused tag.
    b.insert(b.end(),{0xc4,60,0,0,0,2,0,0,0,164,0,0,0});
    const auto m=okage::read_xcl(b);check(m.chunks.size()==1&&m.chunks[0].vertices.size()==3&&m.pointer_locations.size()==6);
    const auto rounding=std::fegetround();check(m.query({1,1,1,0},{2,2,2,0})==std::vector<std::uint32_t>{0});
    check(std::fegetround()==rounding);check(m.query({-1,-1,-1,0},{-1,-1,-1,0}).empty());
    check(m.query({2,2,2,0},{1,1,1,0},1)==std::vector<std::uint32_t>{0});
    auto wide=b;wide.resize(164);
    // Same relocation sequence, using the two-byte skip encoding.
    wide.insert(wide.end(),{0xc8,60,0,0,0,2,0,0,0,0,164,0,0,0});
    check(okage::read_xcl(wide).pointer_locations==m.pointer_locations);
    auto grid=m;grid.header[12]=grid.header[13]=grid.header[14]=2;
    grid.cells={0,1,2,3,4,-1,0,5};
    check(grid.query({0,0,0,0},{8,8,8,0},0)==std::vector<std::uint32_t>({0,1,2,3,4,0,5}));
    check(grid.query({0,0,0,0},{8,8,8,0},1)==std::vector<std::uint32_t>({1,0,3,2,4,5,0}));
    check(grid.query({8,8,8,0},{0,0,0,0},2)==grid.query({0,0,0,0},{8,8,8,0},0));
    grid.header[12]=grid.header[14]=1;grid.header[13]=65;grid.cells.assign(65,0);
    rejects([&]{grid.query({0,0,0,0},{0,512,0,0});});
    rejects([&]{m.query({__builtin_bit_cast(float, (0x7fc00000u)),0,0,0},{},0);});
    check(std::fegetround()==rounding);
    rejects([&]{m.query({}, {},3);});
    auto triangle=b;put(triangle,112,__builtin_bit_cast(std::uint32_t, (4.f)));
    put(triangle,136,__builtin_bit_cast(std::uint32_t, (4.f)));put(triangle,146,0x123,2);
    const auto floor=okage::read_xcl(triangle);
    const auto contact=floor.vertical_contact({1,0,1,1},{1,0,1,1},false,4,8190);
    check(contact.hit&&contact.material==0x123&&contact.normal[1]==-1&&contact.correction[1]==-.001f);
    check(!floor.vertical_contact({3,0,3,1},{3,0,3,1},false,4,8190).hit);
    check(!floor.vertical_contact({1,0,1,1},{1,0,1,1},false,4,8190,1).hit);
    filter.bits=0;filter.event(0,0);
    check(!floor.vertical_contact({1,0,1,1},{1,0,1,1},false,4,8190,filter.bits).hit);
    filter.event(1,0);check(floor.vertical_contact({1,0,1,1},{1,0,1,1},false,4,8190,filter.bits).hit);
    check(!floor.vertical_contact({1,0,1,1},{1,0,1,1},false,4,16384).hit);
    put(triangle,148,0x4000,2);put(triangle,156,2,2);put(triangle,158,1,2);
    const auto ceiling=okage::read_xcl(triangle);
    const auto above=ceiling.vertical_contact({1,-1,1,1},{1,-1,1,1},true,4,8190);
    check(above.hit&&above.normal[1]==1&&above.correction[1]>1&&above.correction[1]<1.002f);
    rejects([&]{floor.vertical_contact({},{},false,-1,8190);});
    rejects([&]{floor.vertical_contact({},{},false,4,16385);});
    auto wall=m;
    wall.chunks[0].vertices={{4,0,0,1},{4,0,8,1},{4,8,8,1},{4,8,0,1}};
    wall.chunks[0].polygons={{{3,11,0,0xc000,0,0,1,2,3,0}}};
    const auto ray=wall.segment_contact({2,4,4,1},{6,4,4,1});
    check(ray.hit&&ray.material==11&&ray.normal[0]==-1&&ray.correction[0]<-2);
    check(!wall.segment_contact({6,4,4,1},{2,4,4,1}).hit);
    check(!wall.segment_contact({2,4,4,1},{6,4,4,1},2).hit);
    const auto sphere=wall.sphere_wall_contact({2,4,4,1},{3,4,4,1},{0,0,0,1},1.9f,8190);
    check(sphere.hit&&sphere.material==11&&sphere.correction[0]<-.9f&&sphere.correction[3]==1);
    const auto sphere_edge=wall.sphere_wall_contact({2,4,-1,1},{3,4,-1,1},{0,0,0,1},1.9f,8190);
    check(sphere_edge.hit&&sphere_edge.correction[0]<0&&sphere_edge.correction[2]<0);
    auto zero_normal=wall;
    zero_normal.chunks[0].polygons[0].words[3]=0;
    check(!zero_normal.sphere_wall_contact({2,4,4,1},{3,4,4,1},{0,0,0,1},1.9f,8190).hit);
    check(!zero_normal.sphere_wall_contact({3,4,4,1},{3,4,4,1},{0,0,0,1},1.9f,8190).hit);
    // Reject only the failed plane, retaining the next valid wall candidate.
    zero_normal.chunks[0].polygons.push_back(wall.chunks[0].polygons[0]);
    const auto after_zero=zero_normal.sphere_wall_contact({2,4,4,1},{3,4,4,1},{0,0,0,1},1.9f,8190);
    check(after_zero.hit&&after_zero.polygon==1&&after_zero.correction==sphere.correction);
    rejects([&]{wall.sphere_wall_contact({},{},{},-1,8190);});
    const okage::CollisionCapsule capsule{{0,2.5f,0,1},{0,5,0,1},1.9f};
    auto capsule_zero=wall;
    capsule_zero.chunks[0].polygons[0].words[3]=0;
    rejects([&]{capsule_zero.capsule_wall_contact({0,0,4,1},{1,0,4,1},capsule,8190);});
    capsule_zero.chunks[0].polygons.insert(capsule_zero.chunks[0].polygons.begin(),wall.chunks[0].polygons[0]);
    check(!capsule_zero.capsule_wall_contact({0,0,4,1},{1,0,4,1},capsule,8190).hit);
    const auto face=wall.capsule_wall_contact({2,0,4,1},{2.5f,0,4,1},capsule,8190);
    check(face.hit&&face.material==11&&face.correction[0]<-.4009f&&face.correction[0]>-.4011f);
    check(face.correction[1]==0&&face.correction[2]==0&&face.correction[3]==0&&face.normal[0]==-1);
    const auto edge=wall.capsule_wall_contact({2.5f,0,-1,1},{3,0,-1,1},capsule,8190);
    check(edge.hit&&edge.correction[0]<0&&edge.correction[2]<0);
    check(!wall.capsule_wall_contact({2.5f,0,4,1},{2.5f,0,4,1},capsule,8190).hit);
    check(!wall.capsule_wall_contact({3,0,4,1},{2.5f,0,4,1},capsule,8190).hit);
    check(!wall.capsule_wall_contact({2,0,4,1},{2.5f,0,4,1},capsule,8190,2).hit);
    auto later=wall.chunks[0].polygons.front();later.words[1]=12;
    wall.chunks[0].polygons.push_back(later);
    check(wall.capsule_wall_contact({2,0,4,1},{2.5f,0,4,1},capsule,8190).material==11);
    rejects([&]{wall.capsule_wall_contact({},{},{{},{},1},8190);});
    rejects([&]{wall.capsule_wall_contact({},{},{{},{0,1,0,0},-1},8190);});
    rejects([&]{wall.capsule_wall_contact({},{},capsule,16385);});
    wall.chunks[0].vertices.insert(wall.chunks[0].vertices.end(),{{0,6,0,1},{8,6,0,1},{8,6,8,1},{0,6,8,1}});
    wall.chunks[0].polygons.push_back({{1,13,0xc000,0,0,4,5,6,7,4}});
    const std::array<okage::CollisionShape,2> shapes{{{8,{{0,0,0,1},{},0}},
                                                   {0x72,{{0,-2.5f,0,1},{0,-5,0,1},1.9f}}}};
    const auto move=wall.move_contact({2,6,4,1},{7,6,4,1},shapes,{});
    check(move.hit&&move.flags==5&&move.materials[0]==13&&move.materials[2]==11);
    check(move.correction[0]>-4.902f&&move.correction[0]<-4.900f&&move.correction[3]==1);
    check(move.correction[1]<-.0009f&&move.correction[1]>-.0011f);
    auto settings=okage::MapCollisionSettings{};settings.wall_iterations=0;
    const auto no_wall=wall.move_contact({2,6,4,1},{7,6,4,1},shapes,settings);
    // Original 0x187270 has normalization/accumulation drift even with walls
    // disabled. Verified by the independent room fixture in verify_map_contacts.
    check(no_wall.flags==1&&__builtin_bit_cast(std::uint32_t, (no_wall.correction[0]))==0xb6800000);
    settings.step=0;rejects([&]{wall.move_contact({},{},shapes,settings);});
    auto bad_shapes=shapes;bad_shapes[1].flags=3;
    rejects([&]{wall.move_contact({},{},bad_shapes,{});});
    bad_shapes=shapes;bad_shapes[0].flags=0x20;
    rejects([&]{wall.move_contact({},{},bad_shapes,{});});
    const auto actor_capsule=shapes[1].geometry;
    const auto side=okage::capsule_pair_contact({-4,0,0,1},{-3,0,0,1},actor_capsule,{0,0,0,1},actor_capsule,4,.5f);
    check(side.hit&&side.flags==0&&side.correction[0]<-.8009f&&side.correction[0]>-.8011f);
    check(!okage::capsule_pair_contact({-6,0,0,1},{-5,0,0,1},actor_capsule,{0,0,0,1},actor_capsule,4,.5f).hit);
    rejects([&]{okage::capsule_pair_contact({},{},actor_capsule,{},actor_capsule,-1,.5f);});
    auto empty=wall;empty.chunks[0].polygons.clear();
    std::array<okage::CollisionBody,2> bodies;
    for(unsigned i=0;i<2;++i) {
        bodies[i].shapes.assign(shapes.begin(),shapes.end());bodies[i].id=i;
        bodies[i].broad_radius=3.8f;bodies[i].priority=10;
        bodies[i].previous=bodies[i].position={i==0?-4.f:0.f,0,0,1};
    }
    auto destination=okage::Float4{-3,0,0,1};
    check(okage::resolve_collision(empty,bodies,0,destination,{},.5f));
    check(bodies[0].contact.flags==8&&bodies[0].other_body==1&&destination[0]<-3.8f);
    check(bodies[1].position[0]==0&&bodies[0].position[0]==-4);
    bodies[1].priority=20;destination={-3,0,0,1};
    check(okage::resolve_collision(empty,bodies,0,destination,{},.5f));
    check(destination[0]==-3&&bodies[1].position[0]>.8009f&&bodies[1].position[0]<.8011f&&bodies[1].id==1);
    bodies[1].flags|=2;destination={-3,0,0,1};
    check(!okage::resolve_collision(empty,bodies,0,destination,{},.5f));
    check(bodies[0].contact.flags==0&&bodies[0].other_body==-1);
    rejects([&]{okage::resolve_collision(empty,bodies,2,destination,{},.5f);});
    check(std::fegetround()==rounding);
    for(unsigned n=0;n<69;++n)rejects([&]{okage::read_xcl(okage::Bytes(b).first(n));});
    for(auto [at,value]:{std::pair{12u,31u},{48u,0xffffffffu},{80u,164u},{96u,0x7f800000u}}) {
        auto bad=b;put(bad,at,value);rejects([&]{okage::read_xcl(bad);});
    }
    auto bad=b;put(bad,158,3,2);rejects([&]{okage::read_xcl(bad);});
    bad=b;put(bad,160,3,2);rejects([&]{okage::read_xcl(bad);});
    bad=b;put(bad,165,61);rejects([&]{okage::read_xcl(bad);});
    bad=b;put(bad,bad.size()-4,0xffffffff);rejects([&]{okage::read_xcl(bad);});
    bad=b;bad[164]=0;rejects([&]{okage::read_xcl(bad);});
    std::cout<<"XCL grid/geometry, relocation bounds, finite queries and malformed input checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
