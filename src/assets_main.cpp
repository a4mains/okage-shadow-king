#include "okage/assets.hpp"
#include "okage/model.hpp"
#include "okage/pose.hpp"
#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include "okage/render.hpp"
#include "okage/map.hpp"
#include "okage/map_objects.hpp"
#include "okage/enemy.hpp"
#include <cfenv>
#include "okage/event.hpp"
#include "okage/audio.hpp"
#include "okage/sequence.hpp"
#include "okage/bank.hpp"
#include "okage/music.hpp"
#include "okage/stream_bank.hpp"
#include "okage/text.hpp"
#include "okage/font.hpp"
#include "okage/dialogue.hpp"
#include "okage/camera.hpp"
#include "okage/field.hpp"
#include "okage/effect.hpp"
#include "okage/collision.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > 128 * 1024 * 1024) throw std::runtime_error("Input exceeds 128 MiB limit");
    std::vector<std::uint8_t> bytes(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}
void write(const fs::path& path, okage::Bytes bytes) {
    if (fs::is_symlink(path)) throw std::runtime_error("Refusing symlink output");
    if (fs::exists(path)) {
        const auto old = read(path);
        if (old.size() != bytes.size() || !std::equal(old.begin(), old.end(), bytes.begin()))
            throw std::runtime_error("Refusing different existing output: " + path.string());
        return;
    }
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot write " + path.string());
}
void model_report(const fs::path& path) {
    const auto model = okage::read_xmd(read(path));
    std::cout << "file\t" << path.string() << '\t' << model.textures.size() << '\t' << model.models.size() << '\n';
    for (const auto& part : model.models) {
        std::cout << "model\t" << part.offset << '\t' << part.end << '\t' << part.bones.size() << '\t'
                  << part.hierarchy.size() << '\t' << part.names.size() << '\t' << part.blocks.size() << '\n';
        for (const auto& block : part.blocks) {
            std::size_t triangles=0, quads=0;
            for (const auto& polygon : block.polygons) (polygon.corners.size()==3 ? triangles : quads)++;
            std::cout << "block\t" << block.offset << '\t' << block.end << '\t' << block.draw_offset << '\t'
                      << block.texture_command << '\t' << block.material_command;
            for (const auto value : block.attribute_extents) std::cout << '\t' << value;
            std::cout << '\t' << block.commands.size() << '\t' << triangles << '\t' << quads << '\n';
        }
    }
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("Usage: okage-assets list ARCHIVE | extract ARCHIVE OUTDIR | decode PACKED OUTFILE | texture XIM PAMFILE");
        const std::string command = argv[1];
        if(command=="xcl-probe"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto queries=read(argv[3]);
            if(queries.size()%28)throw std::runtime_error("XCL query fixture extent");
            std::vector<std::uint8_t> out;
            auto append=[&](std::uint32_t n){for(unsigned i=0;i<4;++i)out.push_back(n>>(i*8));};
            append(map.payload.size());auto payload=map.payload;
            for(auto at:map.pointer_locations) {
                std::uint32_t n;std::memcpy(&n,payload.data()+at,4);n+=0x900000;std::memcpy(payload.data()+at,&n,4);
            }
            out.insert(out.end(),payload.begin(),payload.end());
            for(std::size_t at=0;at<queries.size();at+=28) {
                okage::Float4 first{},second{};std::uint32_t mode;
                std::memcpy(first.data(),queries.data()+at,12);std::memcpy(second.data(),queries.data()+at+12,12);std::memcpy(&mode,queries.data()+at+24,4);
                const auto candidates=map.query(first,second,mode);append(candidates.size());
                for(auto index:candidates)append(map.chunks[index].source_offset);
            }
            write(argv[4],out);
            std::size_t vertices=0,polygons=0;for(const auto& c:map.chunks){vertices+=c.vertices.size();polygons+=c.polygons.size();}
            std::cout<<"XCL chunks="<<map.chunks.size()<<" vertices="<<vertices<<" polygons="<<polygons<<" relocations="<<map.pointer_locations.size()<<'\n';return 0;
        }
        if(command=="xcl-vertical"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto queries=read(argv[3]);
            if(queries.size()%48)throw std::runtime_error("XCL vertical fixture extent");
            std::vector<std::uint8_t> out;
            for(std::size_t at=0;at<queries.size();at+=48) {
                okage::Float4 previous{},desired{};float reach;std::uint32_t threshold,mask,kind;
                std::memcpy(previous.data(),queries.data()+at,16);std::memcpy(desired.data(),queries.data()+at+16,16);
                std::memcpy(&reach,queries.data()+at+32,4);std::memcpy(&threshold,queries.data()+at+36,4);
                std::memcpy(&mask,queries.data()+at+40,4);std::memcpy(&kind,queries.data()+at+44,4);
                if(kind>1)throw std::runtime_error("XCL vertical fixture kind");
                const auto contact=map.vertical_contact(previous,desired,kind,reach,threshold,mask);
                std::array<std::uint32_t,16> row{};
                row[0]=contact.hit;row[1]=contact.hit?(kind?2:1):0;
                if(contact.hit) {
                    const auto& chunk=map.chunks.at(contact.chunk);
                    row[2]=contact.material;row[3]=chunk.source_offset;row[4]=contact.polygon;row[5]=contact.candidate;
                    for(unsigned j=0;j<4;++j)row[6+j]=__builtin_bit_cast(std::uint32_t, (contact.correction[j]));
                    for(unsigned j=0;j<3;++j)row[10+j]=__builtin_bit_cast(std::uint32_t, (contact.normal[j]));
                    std::memcpy(&row[13],map.payload.data()+chunk.source_offset+4,8);row[15]=chunk.polygons.size();
                }
                for(auto n:row)for(unsigned j=0;j<4;++j)out.push_back(n>>(j*8));
            }
            write(argv[4],out);return 0;
        }
        if(command=="xcl-wall"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto queries=read(argv[3]);
            if(queries.size()%76)throw std::runtime_error("XCL wall fixture extent");
            std::vector<std::uint8_t> out;
            for(std::size_t at=0;at<queries.size();at+=76) {
                okage::Float4 previous{},desired{};okage::CollisionCapsule capsule;std::uint32_t threshold,mask;
                std::memcpy(previous.data(),queries.data()+at,16);std::memcpy(desired.data(),queries.data()+at+16,16);
                std::memcpy(capsule.first.data(),queries.data()+at+32,16);std::memcpy(capsule.second.data(),queries.data()+at+48,16);
                std::memcpy(&capsule.radius,queries.data()+at+64,4);std::memcpy(&threshold,queries.data()+at+68,4);std::memcpy(&mask,queries.data()+at+72,4);
                if(threshold>16384)throw std::runtime_error("XCL wall threshold");
                const auto contact=map.capsule_wall_contact(previous,desired,capsule,threshold,mask);
                std::array<std::uint32_t,10> row{};row[0]=contact.hit;row[1]=contact.hit?4:0;
                if(contact.hit) {
                    row[2]=contact.material;
                    for(unsigned j=0;j<4;++j)row[3+j]=__builtin_bit_cast(std::uint32_t, (contact.correction[j]));
                    for(unsigned j=0;j<3;++j)row[7+j]=__builtin_bit_cast(std::uint32_t, (contact.normal[j]));
                }
                for(auto n:row)for(unsigned j=0;j<4;++j)out.push_back(n>>(j*8));
            }
            write(argv[4],out);return 0;
        }
        if(command=="xcl-camera"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto data=read(argv[3]);
            if(data.empty()||data.size()%48||data.size()>48*10000)throw std::runtime_error("Camera collision fixture bounds");
            std::vector<std::uint32_t> output;
            for(unsigned at=0;at<data.size();at+=48) {
                auto word=[&](unsigned n){std::uint32_t v;std::memcpy(&v,data.data()+at+4*n,4);return v;};
                auto point=[&](unsigned n){okage::Float4 v;for(unsigned k=0;k<4;++k)v[k]=__builtin_bit_cast(float, (word(n+k)));return v;};
                const auto first=point(0);auto second=point(4);const auto mode=word(8),mask=word(10);const float radius=__builtin_bit_cast(float, (word(9)));
                if(mode>2||word(11)>16384||(mode==2&&(radius!=4.5f||word(11)!=8190)))throw std::runtime_error("Camera collision fixture settings");
                okage::MapContact contact;
                if(mode==2)contact=map.camera_move(first,second,mask);
                else {
                    const auto surface=mode?map.sphere_wall_contact(first,second,{0,0,0,1},radius,word(11),mask):map.segment_contact(first,second,mask);
                    if(surface.hit) {
                        const float threshold=__builtin_bit_cast(float, (0x3efff17du));
                        const unsigned kind=mode?2:(surface.normal[1]< -threshold?0:(surface.normal[1]>threshold?1:2));
                        contact.hit=true;contact.flags=1u<<kind;contact.materials[kind]=surface.material;
                        contact.correction=surface.correction;contact.normals[kind]=surface.normal;
                    }
                }
                std::array<std::uint32_t,22> row{};row[0]=contact.hit;row[1]=contact.flags;
                for(unsigned i=0;i<3;++i)row[2+i]=contact.materials[i];
                if(contact.hit)for(unsigned i=0;i<4;++i)row[5+i]=__builtin_bit_cast(std::uint32_t, (contact.correction[i]));
                for(unsigned kind=0;kind<3;++kind)if(contact.flags&(1u<<kind))
                    for(unsigned i=0;i<3;++i)row[9+kind*3+i]=__builtin_bit_cast(std::uint32_t, (contact.normals[kind][i]));
                for(unsigned i=0;i<4;++i)row[18+i]=__builtin_bit_cast(std::uint32_t, (second[i]));
                output.insert(output.end(),row.begin(),row.end());
            }
            write(argv[4],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="xcl-move"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto queries=read(argv[3]);
            if(queries.size()%112)throw std::runtime_error("XCL movement fixture extent");
            std::vector<std::uint8_t> out;
            for(std::size_t at=0;at<queries.size();at+=112) {
                okage::Float4 previous{},desired{};std::array<okage::CollisionShape,2> shapes{};okage::MapCollisionSettings settings;
                auto copy=[&](auto& value,unsigned offset,unsigned size){std::memcpy(&value,queries.data()+at+offset,size);};
                copy(previous,0,16);copy(desired,16,16);copy(shapes[1].geometry.first,32,16);copy(shapes[1].geometry.second,48,16);
                copy(shapes[1].geometry.radius,64,4);copy(settings.normal_threshold,68,4);copy(settings.disabled_mask,72,4);
                copy(shapes[0].geometry.first,76,16);copy(settings.floor_reach,92,4);copy(settings.step,96,4);
                copy(settings.wall_iterations,100,4);copy(shapes[0].flags,104,4);copy(shapes[1].flags,108,4);
                const auto contact=map.move_contact(previous,desired,shapes,settings);
                std::array<std::uint32_t,18> row{};row[0]=contact.hit;row[1]=contact.flags;
                for(unsigned j=0;j<3;++j)row[2+j]=contact.materials[j];
                if(contact.hit)for(unsigned j=0;j<4;++j)row[5+j]=__builtin_bit_cast(std::uint32_t, (contact.correction[j]));
                for(unsigned kind=0;kind<3;++kind)if(contact.flags&(1u<<kind))
                    for(unsigned j=0;j<3;++j)row[9+kind*3+j]=__builtin_bit_cast(std::uint32_t, (contact.normals[kind][j]));
                for(auto n:row)for(unsigned j=0;j<4;++j)out.push_back(n>>(j*8));
            }
            write(argv[4],out);return 0;
        }
        if(command=="capsule-pair"&&argc==4) {
            const auto queries=read(argv[2]);if(queries.size()%128)throw std::runtime_error("Capsule pair fixture extent");
            std::vector<std::uint8_t> out;
            for(std::size_t at=0;at<queries.size();at+=128) {
                okage::Float4 previous{},desired{},other_position{};okage::CollisionCapsule moving,other;float reach,threshold;
                auto copy=[&](auto& value,unsigned offset,unsigned size){std::memcpy(&value,queries.data()+at+offset,size);};
                copy(previous,0,16);copy(desired,16,16);copy(other_position,32,16);
                copy(moving.first,48,16);copy(moving.second,64,16);copy(moving.radius,80,4);
                copy(other.first,84,16);copy(other.second,100,16);copy(other.radius,116,4);copy(reach,120,4);copy(threshold,124,4);
                const auto contact=okage::capsule_pair_contact(previous,desired,moving,other_position,other,reach,threshold);
                std::array<std::uint32_t,14> row{};row[0]=contact.hit;row[1]=contact.flags;
                if(contact.hit)for(unsigned j=0;j<3;++j)row[2+j]=__builtin_bit_cast(std::uint32_t, (contact.correction[j]));
                for(unsigned kind=0;kind<3;++kind)if(contact.flags&(1u<<kind))
                    for(unsigned j=0;j<3;++j)row[5+kind*3+j]=__builtin_bit_cast(std::uint32_t, (contact.normals[kind][j]));
                for(auto n:row)for(unsigned j=0;j<4;++j)out.push_back(n>>(j*8));
            }
            write(argv[3],out);return 0;
        }
        if(command=="collision-world"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto queries=read(argv[3]);
            if(queries.size()%396)throw std::runtime_error("Collision world fixture extent");
            std::vector<std::uint8_t> out;
            for(std::size_t at=0;at<queries.size();at+=396) {
                auto word=[&](unsigned off){std::uint32_t n;std::memcpy(&n,queries.data()+at+off,4);return n;};
                auto scalar=[&](unsigned off){return __builtin_bit_cast(float, (word(off)));};
                auto vector=[&](unsigned off){return okage::Float4{scalar(off),scalar(off+4),scalar(off+8),scalar(off+12)};};
                if(word(4)!=3)throw std::runtime_error("Collision world body count");
                okage::MapCollisionSettings settings;settings.disabled_mask=word(8);settings.normal_threshold=word(12);
                settings.wall_iterations=word(16);settings.step=scalar(24);auto desired=vector(32);
                std::array<okage::CollisionBody,3> bodies;
                for(unsigned i=0;i<3;++i) {
                    auto& b=bodies[i];const unsigned off=48+i*116;b.previous=vector(off);b.position=vector(off+16);
                    b.shapes={{word(off+104),{vector(off+32),{},0}},
                              {word(off+108),{vector(off+48),vector(off+64),scalar(off+80)}}};
                    b.broad_radius=scalar(off+84);b.floor_reach=scalar(off+88);b.flags=word(off+92);
                    b.priority=word(off+96);b.id=word(off+100);b.registered=word(off+112)!=0;
                }
                const auto hit=okage::resolve_collision(map,bodies,word(0),desired,settings,scalar(28),__builtin_bit_cast(std::int32_t, (word(20))));
                std::vector<std::uint32_t> row{std::uint32_t(hit)};
                auto floats=[&](const okage::Float4& v,unsigned count){for(unsigned j=0;j<count;++j)row.push_back(__builtin_bit_cast(std::uint32_t, (v[j])));};
                floats(desired,4);
                for(const auto& b:bodies) {
                    floats(b.position,4);row.push_back(b.contact.flags);
                    for(auto id:b.contact.materials)row.push_back(id);
                    row.push_back(b.other_body+1);
                    floats((b.contact.flags&15)?b.contact.correction:okage::Float4{},4);
                    for(unsigned kind=0;kind<3;++kind)floats((b.contact.flags&(1u<<kind))?b.contact.normals[kind]:okage::Float4{},3);
                }
                for(auto n:row)for(unsigned j=0;j<4;++j)out.push_back(n>>(j*8));
            }
            write(argv[4],out);return 0;
        }
        if(command=="event-write" && argc==4) {
            const auto input=read(argv[2]);
            if(input.empty()||input.size()%8||input.size()>1024)throw std::runtime_error("Event write fixture bounds");
            std::vector<std::uint8_t> variables(0x1000,0xa5),out;
            for(std::size_t at=0;at<input.size();at+=8) {
                std::uint32_t selector;std::int32_t value;
                std::memcpy(&selector,input.data()+at,4);std::memcpy(&value,input.data()+at+4,4);
                if(selector==0x40006)throw std::runtime_error("Field controller side effect is unrecovered");
                okage::set_event_variable(variables,selector&0xffff,selector>>16,value);
                out.insert(out.end(),{4,0,0,0});out.insert(out.end(),variables.begin(),variables.end());
            }
            write(argv[3],out);return 0;
        }
        if(command=="comparison-options"&&argc==4) {
            const auto input=read(argv[2]);std::vector<std::uint32_t> output;
            if(input.empty()||input.size()%16||input.size()>16*512)throw std::runtime_error("Comparison fixture extent");
            const std::array<const char*,4> operators={"<",">","=","!="};
            for(unsigned at=0;at<input.size();at+=16) {
                std::uint32_t row[4];std::memcpy(row,input.data()+at,16);
                if(row[0]>3||(row[1]!=0&&row[1]!=1&&row[1]!=4))throw std::runtime_error("Comparison fixture scope");
                okage::MessageBank bank;
                const auto option="CMP(0/"+std::to_string(row[1])+"/"+std::to_string(__builtin_bit_cast(std::int32_t, (row[3])))+")"+operators[row[0]];
                bank.strings={"system","42","\\o["+option+"]\\v","True\\e","False\\e"};
                bank.nodes={{0,0},{0,1},{1,2},{2,3},{2,4}};
                okage::DialogueText text(bank,42);okage::DialogueController controller;
                std::array<std::uint8_t,0x1000> variables{};okage::set_event_variable(variables,0,row[1],__builtin_bit_cast(std::int32_t, (row[2])));
                controller.get_variable=[&](unsigned index,unsigned storage){return okage::event_variable(variables,(storage<<16)|index);};
                text.state.flags=0x8001;text.step();const auto effect=controller.update(text,{});
                if(!effect.reset_text_clock||effect.resume_calls!=1||controller.advance_count!=0||text.state.delay)
                    throw std::runtime_error("Comparison fixture did not select/resume");
                output.push_back(14u|(unsigned(text.choice_result)<<16));
            }
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="relationship"&&argc==4) {
            const auto input=read(argv[2]);std::vector<std::uint32_t> output;
            if(input.empty()||input.size()%40||input.size()>40*256)throw std::runtime_error("Relationship fixture extent");
            for(unsigned at=0;at<input.size();at+=40) {
                std::uint32_t row[10];std::memcpy(row,input.data()+at,40);
                okage::DialogueText text({""});okage::DialogueController controller;
                controller.window_type=1;controller.advance_count=controller.layer_advance=4;
                text.choice_result=__builtin_bit_cast(std::int32_t, (row[0]));
                text.state.flags=0x8910;text.state.phase=9;text.state.period=0xfffffffe;
                text.state.delay=7;text.state.progress=5;
                text.state.window="LA("+std::to_string(row[1])+"/"+std::to_string(row[2])+"/"+std::to_string(row[3])+")";
                controller.get_variable=[&](unsigned index,unsigned bank){
                    if(bank!=4||index<16||index>21)throw std::runtime_error("Relationship read outside fixture");
                    return __builtin_bit_cast(std::int32_t, (row[4+index-16]));
                };
                controller.set_variable=[&](unsigned index,unsigned bank,std::int32_t value){
                    if(bank!=4||index<16||index>21)throw std::runtime_error("Relationship write outside fixture");
                    row[4+index-16]=__builtin_bit_cast(std::uint32_t, (value));
                };
                controller.update(text,{});
                output.insert(output.end(),row+4,row+10);
                output.insert(output.end(),{text.state.flags,text.state.phase,text.state.period,text.state.delay,text.state.progress,
                    controller.advance_count,controller.layer_flags,controller.stage,
                    std::uint32_t(static_cast<unsigned char>(text.state.window[0])),std::uint32_t(text.choice_result)});
            }
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="sky-follow"&&argc==5) {
            const auto factors=okage::field_sky_follow(read(argv[2]));const auto input=read(argv[3]);
            if(input.empty()||input.size()%16)throw std::runtime_error("Sky-follow camera extent");
            std::vector<std::uint8_t> output;
            for(unsigned at=0;at<input.size();at+=16){okage::Float4 camera;std::memcpy(camera.data(),input.data()+at,16);
                const auto position=okage::sky_follow_position(camera,factors);
                const auto* first=reinterpret_cast<const std::uint8_t*>(position.data());output.insert(output.end(),first,first+16);}
            write(argv[4],output);return 0;
        }
        if(command=="instance-visibility"&&argc==4) {
            const auto input=read(argv[2]);
            if(input.empty()||input.size()%32)throw std::runtime_error("Visibility fixture extent");
            std::vector<std::uint32_t> output;
            for(std::size_t at=0;at<input.size();at+=32) {
                std::array<std::uint32_t,8> row{};std::memcpy(row.data(),input.data()+at,32);
                if(row[0]>=2048||!(row[1]&256)||row[2]>65535||row[3]>=4)
                    throw std::runtime_error("Visibility fixture range");
                std::array<okage::CollisionBody,4> bodies;
                for(unsigned i=0;i<4;++i)bodies[i].flags=row[4+i];
                bool visible=(row[2]&0x800)!=0;
                okage::set_instance_visibility(visible,row[3],bodies,(row[1]&255)!=0);
                output.push_back(visible);
                for(const auto& body:bodies)output.push_back(body.flags);
                output.push_back(0x900800); // No additional event payload consumed.
            }
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="map-collision"&&argc==4) {
            const auto bindings=okage::map_collision_bodies(okage::read_xmp(read(argv[2])));
            std::vector<std::uint32_t> output;
            auto scalar=[&](float f){output.push_back(__builtin_bit_cast(std::uint32_t, (f)));};
            auto vector=[&](const okage::Float4& v){for(float f:v)scalar(f);};
            for(const auto& binding:bindings) {
                const auto& b=binding.body;
                output.insert(output.end(),{unsigned(binding.placement),unsigned(binding.instance),b.id,b.flags,b.priority});
                scalar(b.broad_radius);scalar(b.floor_reach);vector(b.position);
                if(b.shapes.size()!=2)throw std::runtime_error("Map body shape count");
                for(const auto& shape:b.shapes){output.push_back(shape.flags);vector(shape.geometry.first);vector(shape.geometry.second);scalar(shape.geometry.radius);}
            }
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="map-visibility"&&argc==4) {
            const auto map=okage::read_xmp(read(argv[2]));const auto ids=okage::map_instance_ids(map);
            okage::MapObjects objects(argv[2]);std::vector<std::uint32_t> output;
            for(std::size_t i=0;i<ids.size();++i)if(ids[i]>=0) {
                const bool before=objects.visible(i);
                if(!objects.set_visible(ids[i],false))throw std::runtime_error("Map instance missing");
                const bool hidden=objects.visible(i);
                if(!objects.set_visible(ids[i],true))throw std::runtime_error("Map instance missing");
                const bool shown=objects.visible(i);objects.set_visible(ids[i],before);
                output.insert(output.end(),{unsigned(i),unsigned(ids[i]),unsigned(before),unsigned(hidden),
                    unsigned(shown),unsigned(objects.visible(i)),unsigned(objects.placement(i)!=nullptr)});
            }
            if(objects.set_visible(-1,true)||objects.set_visible(2048,true))throw std::runtime_error("Invalid map ID accepted");
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="scene-entry-fade"&&argc==3) {
            std::vector<std::uint32_t> output;
            for(float initial:{0.f,.5f,1.f}) {
                okage::SceneFade fade;fade.current=initial;fade.color=0x123456;fade.request(1.f);
                for(unsigned frame=0;frame<=60;++frame) {
                    if(frame)fade.tick();
                    output.insert(output.end(),{unsigned(fade.enabled),unsigned(fade.hold),
                        __builtin_bit_cast(std::uint32_t, (fade.current)),__builtin_bit_cast(std::uint32_t, (fade.target)),fade.color});
                }
            }
            write(argv[2],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="scene-fade"&&argc==4) {
            const auto input=read(argv[2]);
            if(input.empty()||input.size()%16)throw std::runtime_error("Scene fade fixture extent");
            std::vector<std::uint32_t> output;
            for(std::size_t at=0;at<input.size();at+=16) {
                std::array<std::uint32_t,4> row{};std::memcpy(row.data(),input.data()+at,16);
                okage::SceneFade fade;fade.color=row[2];fade.current=__builtin_bit_cast(float, (row[3]));
                fade.script_request(row[0],__builtin_bit_cast(std::int32_t, (row[1])));
                output.insert(output.end(),{unsigned(fade.enabled),unsigned(fade.hold),
                    __builtin_bit_cast(std::uint32_t, (fade.current)),__builtin_bit_cast(std::uint32_t, (fade.target)),
                    fade.color,(fade.alpha()<<24)|fade.color});
            }
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="field-message-init"&&argc==5) {
            const auto bank=okage::read_pme(read(argv[2]));
            okage::DialogueText text(bank,std::stoi(argv[3]));
            okage::TextClock clock;okage::DialogueController controller;
            okage::DialogueOpacity body;okage::DialogueClosure closure;okage::DialogueOpening opening;
            opening.initialize(text,clock,controller,body,closure);
            std::vector<std::uint32_t> output;
            const auto capture=[&]{
                const auto& s=text.state;
                output.insert(output.end(),{controller.stage,0x50,controller.layer_flags,
                    __builtin_bit_cast(std::uint32_t, (controller.window_type)),controller.advance_count,
                    s.flags,s.font,s.line_height,s.color,s.phase,s.period,s.delay,s.progress,s.x,s.y,s.line_origin,
                    clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll_target)),
                    __builtin_bit_cast(std::uint32_t, (clock.scroll)),__builtin_bit_cast(std::uint32_t, (clock.scroll_step)),
                    __builtin_bit_cast(std::uint32_t, (clock.scroll_base_step)),clock.glyph_flags});
                for(const auto& o:{body,closure.decoration})for(float f:{o.value,o.upper,o.lower,o.delta})
                    output.push_back(__builtin_bit_cast(std::uint32_t, (f)));
            };
            capture();body.tick();closure.decoration.tick();opening.bootstrap(text,clock);capture();
            opening.update(text,clock,controller,body,closure);capture();
            write(argv[4],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="effect-tint"&&argc==4) {
            const auto input=read(argv[2]);std::vector<std::uint8_t> output;
            if(input.empty()||input.size()%48||input.size()>48*64)throw std::runtime_error("Effect tint fixture extent");
            for(unsigned at=0;at<input.size();at+=48){
                std::array<okage::Float4,3> v;std::memcpy(v.data(),input.data()+at,48);
                const auto tint=okage::effect_light_tint(v[0],v[1],v[2]);
                const auto* bytes=reinterpret_cast<const std::uint8_t*>(tint.data());output.insert(output.end(),bytes,bytes+16);
            }
            write(argv[3],output);return 0;
        }
        if(command=="map-effect-geometry"&&argc==6) {
            const auto bank=okage::read_efd(read(argv[2]));const auto input=read(argv[4]);
            constexpr unsigned stride=198*4;
            if(input.empty()||input.size()%stride||input.size()>stride*64)throw std::runtime_error("Map geometry fixture bounds");
            const auto found=std::find_if(bank.begin(),bank.end(),[](const auto& d){return d.id==25;});
            if(found==bank.end())throw std::runtime_error("Missing map effect25");
            okage::EffectTextureSizes sizes{};
            for(unsigned i=0;i<sizes.size();++i){const auto image=okage::decode_xim(read(fs::path(argv[3])/okage::effect_texture_names[i]));sizes[i]={image.width,image.height};}
            std::vector<std::uint32_t> out,geometry;const auto sine=okage::make_sine_table();
            auto append=[](auto& dest,const auto& v){for(float f:v)dest.push_back(__builtin_bit_cast(std::uint32_t, (f)));};
            for(unsigned at=0;at<input.size();at+=stride){
                std::uint32_t identity;std::memcpy(&identity,input.data()+at,4);
                std::array<okage::Float4,5> channels;std::memcpy(channels.data(),input.data()+at+4,80);
                okage::Matrix4 view;std::memcpy(view.data(),input.data()+at+84,64);
                float far;std::memcpy(&far,input.data()+at+148,4);
                okage::MapEffect effect(*found,channels[0],channels[1],channels[2],sizes,[]{return 1;});
                effect.color=channels[3];
                for(unsigned i=0;i<8;++i){
                    auto& e=effect.elements[i];const auto source=input.data()+at+152+i*80;
                    std::array<std::uint32_t,4> meta;std::memcpy(meta.data(),source+64,16);
                    if(e.shape!=meta[0]||e.flags!=meta[1]||e.primitive_flags!=meta[2]||meta[3]>=sizes.size())
                        throw std::runtime_error("Native geometry fixture metadata mismatch");
                    std::memcpy(e.rendered.data(),source,64);e.texture=meta[3];
                    e.uv={0,0,std::uint16_t(sizes[e.texture][0]-1),std::uint16_t(sizes[e.texture][1]-1)};
                }
                out.push_back(identity);
                const auto draws=okage::map_effect_draws(effect,view,channels[4],far,sine);
                if(draws.size()!=8)throw std::runtime_error("Missing map effect geometry");
                for(unsigned i=0;i<draws.size();++i){
                    const auto& d=draws[i];for(const auto& v:d.matrix)append(out,v);append(out,d.color);
                    if(at==0){geometry.insert(geometry.end(),{i,unsigned(d.vertices.size()),d.texture});
                        for(const auto& v:d.vertices){append(geometry,v.position);append(geometry,v.color);append(geometry,v.uv);}}
                }
            }
            write(argv[5],{reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4});
            write(std::string(argv[5])+".vertices.bin",{reinterpret_cast<const std::uint8_t*>(geometry.data()),geometry.size()*4});return 0;
        }
        if(command=="map-effect-probe"&&argc==7) {
            const auto bank=okage::read_efd(read(argv[2]));const auto input=read(argv[4]);
            if(input.size()!=80)throw std::runtime_error("Map effect request requires80 bytes");
            std::array<std::uint32_t,4> request;std::memcpy(request.data(),input.data(),16);
            std::array<okage::Float4,4> vectors;std::memcpy(vectors.data(),input.data()+16,64);
            if(request[3]!=0x100)throw std::runtime_error("Map effect probe requires direct handle0");
            const auto found=std::find_if(bank.begin(),bank.end(),[&](const auto& d){return d.id==request[0];});
            if(found==bank.end())throw std::runtime_error("Missing map effect definition");
            const auto frames=std::stoul(argv[5]);if(frames>1000)throw std::runtime_error("Map effect frame bounds");
            okage::EffectTextureSizes sizes{};
            for(unsigned i=0;i<sizes.size();++i){const auto image=okage::decode_xim(read(fs::path(argv[3])/okage::effect_texture_names[i]));sizes[i]={image.width,image.height};}
            okage::GameRandom rng{request[1]};const auto random=[&]{return rng.next();};
            const bool dust=request[0]==17;
            okage::MapEffect effect(*found,vectors[0],dust?okage::Float4{}:vectors[1],
                dust?okage::Float4{1,1,1,1}:vectors[2],sizes,random,
                dust?okage::EffectStart::FirstFrame:okage::EffectStart::RandomPhase);
            std::vector<std::uint32_t> out;
            const auto vector=[&](const okage::Float4& v){for(float f:v)out.push_back(__builtin_bit_cast(std::uint32_t, (f)));};
            const auto capture=[&](unsigned step){
                out.insert(out.end(),{step,0,effect.id,effect.flags,unsigned(effect.elements.size()),effect.duration,effect.phase,rng.state});
                for(const auto& v:{effect.position,effect.rotation,effect.scale,effect.color,effect.parameters})vector(v);
                out.push_back(__builtin_bit_cast(std::uint32_t, (effect.inverse_duration)));
                for(const auto& e:effect.elements){
                    for(const auto& v:{e.position,e.rotation,e.scale,e.color,e.interval})vector(v);
                    out.insert(out.end(),{e.flags,e.program,__builtin_bit_cast(std::uint32_t, (e.change_time)),e.next_program});
                    for(const auto& v:e.rendered)vector(v);
                    out.insert(out.end(),{e.texture,e.uv[0]|(std::uint32_t(e.uv[1])<<16),e.uv[2]|(std::uint32_t(e.uv[3])<<16),e.render_flags});
                }
            };
            capture(0);
            if(dust||request[0]==19) {
                const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
                const okage::Float4 tint{1,1,1,1};std::vector<std::uint32_t> geometry;
                unsigned element=0;
                for(const auto& draw:okage::map_effect_draws(effect,identity,{},450,okage::make_sine_table(),&tint)) {
                    geometry.insert(geometry.end(),{element++,unsigned(draw.vertices.size()),draw.texture});
                    for(const auto& vertex:draw.vertices)for(const auto& v:{vertex.position,vertex.color,vertex.uv})
                        for(float f:v)geometry.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                }
                write(std::string(argv[6])+".vertices.bin",{reinterpret_cast<const std::uint8_t*>(geometry.data()),geometry.size()*4});
            }
            if(!dust)effect.set_parameter(request[2],vectors[3]);capture(1);
            for(unsigned i=0;i<frames;++i){effect.tick(random);capture(i+2);}
            write(argv[6],{reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4});return 0;
        }
        if(command=="effect-probe"&&argc==7) {
            const auto bank=okage::read_efd(read(argv[2]));const auto fixture=read(argv[3]);
            if(fixture.size()!=16)throw std::runtime_error("Effect camera fixture requires float4");
            okage::Float4 camera;std::memcpy(camera.data(),fixture.data(),16);
            const auto found=std::find_if(bank.begin(),bank.end(),[](const auto& d){return d.id==30;});
            if(found==bank.end())throw std::runtime_error("Missing speech-bubble definition");
            okage::BubbleEffect effect(*found,std::stoi(argv[4]));const auto frames=std::stoul(argv[5]);
            if(frames>1000)throw std::runtime_error("Effect probe frame bounds");
            std::vector<std::uint32_t> out;
            for(float f:camera)out.push_back(__builtin_bit_cast(std::uint32_t, (f)));
            for(unsigned frame=0;frame<=frames;++frame) {
                if(frame)effect.tick(camera);
                out.push_back(effect.phase);out.push_back(effect.flags);
                for(const auto& s:effect.sprites) {
                    for(const auto& v:{s.position,s.rotation})for(unsigned j=0;j<3;++j)out.push_back(__builtin_bit_cast(std::uint32_t, (v[j])));
                    for(const auto& v:{s.scale,s.color})for(float f:v)out.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                    out.push_back(__builtin_bit_cast(std::uint32_t, (s.damping)));out.push_back(__builtin_bit_cast(std::uint32_t, (s.previous_time)));
                    out.push_back(s.program);out.push_back(s.texture);
                    out.push_back(s.uv[0]|(std::uint32_t(s.uv[1])<<16));out.push_back(s.uv[2]|(std::uint32_t(s.uv[3])<<16));
                }
            }
            write(argv[6],{reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4});return 0;
        }
        if(command=="stream-bank" && argc==4) {
            const auto bank=okage::read_stream_bank(read(argv[2]));
            std::vector<std::uint32_t> out(bank.chunks.begin(),bank.chunks.end());
            out.push_back(bank.body_size);out.push_back(bank.records.size());
            for(unsigned i=0;i<bank.records.size();++i){out.push_back(bank.records[i]);out.push_back(bank.record(i).size());}
            out.push_back(bank.record(-1).size());out.push_back(bank.record(bank.records.size()).size());
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="stream-prepare" && argc==5) {
            const auto bank=okage::read_stream_bank(read(argv[2]));
            const auto tone=okage::stream_tone(bank,std::stoi(argv[3]));
            const auto sample=okage::prepare_stream_sample(okage::Bytes(bank.bytes).subspan(tone.offset,tone.size));
            write(argv[4],sample);return 0;
        }
        if(command=="stream-cue" && argc==8) {
            const auto bank=okage::read_stream_bank(read(argv[2]));const auto index=std::stoi(argv[3]);
            const auto speed=std::stof(argv[4]);const auto gain=std::stoul(argv[5]);const auto frames=std::stoul(argv[6]);
            if(frames>480000 || gain>128)throw std::runtime_error("Stream cue render budget/gain");
            const auto tone=okage::stream_tone(bank,index,speed,gain);okage::StreamPlayer player(bank);player.play(index,speed,gain);
            std::vector<std::int32_t> mix(frames*2);player.mix(mix);std::vector<std::int16_t> pcm;
            for(const auto value:mix)pcm.push_back(std::clamp(value,-32768,32767));
            write(argv[7],okage::Bytes(reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2));
            std::cout<<tone.offset<<' '<<tone.size<<' '<<tone.sample_rate<<' '<<tone.pitch<<' '<<tone.left<<' '<<tone.right<<' '<<tone.adsr1<<' '<<tone.adsr2<<' '<<player.active()<<'\n';return 0;
        }
        if((command=="music-channels"||command=="iop-music-channels")&&argc==4) {
            const auto input=read(argv[2]);
            if(input.empty()||input.size()%16||input.size()>16*512)throw std::runtime_error("Music channel fixture bounds");
            std::vector<std::uint32_t> out;okage::MusicChannelLevels levels;
            for(unsigned at=0;at<input.size();at+=16) {
                std::array<std::uint32_t,4> r;std::memcpy(r.data(),input.data()+at,16);
                if(command=="music-channels") {
                    std::optional<std::array<std::uint32_t,16>> sent;
                    if(r[0]==0)levels.request(int(r[1]),int(r[2]),r[3]);
                    else if(r[0]==1&&r[1]<=1&&!r[2]&&!r[3])sent=levels.take(r[1]);
                    else if(r[0]==2&&!r[1]&&!r[2]&&!r[3])levels.cancel();
                    else throw std::runtime_error("Music channel fixture operation");
                    out.push_back(levels.dirty);out.insert(out.end(),levels.values.begin(),levels.values.end());
                    out.push_back(sent?16:0);
                    for(unsigned i=0;i<16;++i)out.push_back(sent?(*sent)[i]|((*sent)[i]<<16):0xffffffff);
                } else {
                    if(r[0]>1||r[1]>15)throw std::runtime_error("IOP channel fixture scope");
                    std::array<okage::MusicChannel,16> channels;
                    auto capture=[&]{for(const auto& c:channels){out.push_back(c.dirty);out.push_back(c.left|(std::uint32_t(c.right)<<16));}};
                    capture();if(r[0])channels[r[1]].gain(r[2],r[3]);capture();
                }
            }
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4});return 0;
        }
        if(command=="music-fade" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%16)throw std::runtime_error("Music fade fixture bounds");
            std::vector<std::uint32_t> out;
            for(std::size_t at=0;at<input.size();at+=16){
                std::array<std::uint32_t,4> r;std::memcpy(r.data(),input.data()+at,16);
                if(r[0]>0x800000 || r[3]>10000)throw std::runtime_error("Music fade fixture range");
                okage::MusicVolumeRamp ramp;ramp.current=r[0];ramp.request(r[1],r[2]);
                for(unsigned i=0;i<r[3];++i){
                    const bool send=ramp.current!=ramp.target;ramp.tick();const auto gain=send?ramp.level():0xffffffffu;
                    out.insert(out.end(),{unsigned(ramp.current),unsigned(ramp.target),unsigned(ramp.step),unsigned(ramp.stopped),unsigned(send),gain,gain});
                }
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="music-dry" && (argc==6||argc==7)) {
            const auto ticks=std::stoul(argv[4]);if(!ticks || ticks>12000)throw std::runtime_error("Dry music tick budget");
            std::optional<okage::MusicBank> second;
            if(argc==7)second=okage::read_music_bank(read(argv[6]));
            okage::DryMusicPlayer player(read(argv[2]),okage::read_music_bank(read(argv[3])),std::move(second));
            std::vector<std::int16_t> pcm(ticks*800*2);player.render(pcm);
            write(argv[5],okage::Bytes(reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2));
            std::cout<<"Native dry music: ticks="<<player.driver_ticks()<<" peak_voices="<<player.peak_voices()<<" stolen="<<player.stolen_voices()<<" sample_loops="<<player.sample_loops()<<"; reverb/SFX/device pending\n";return 0;
        }
        if(command=="voice-table" && argc==3) {
            const auto& t=okage::gaussian_table();write(argv[2],okage::Bytes(reinterpret_cast<const std::uint8_t*>(t.data()),sizeof(t)));return 0;
        }
        if(command=="voice-envelope" && argc==7) {
            okage::VoiceEnvelope e(std::stoul(argv[2],nullptr,0),std::stoul(argv[3],nullptr,0));
            const auto release=std::stoul(argv[4]),count=std::stoul(argv[5]);if(count>480000)throw std::runtime_error("Envelope tick budget");std::vector<std::uint32_t> out;
            for(unsigned i=0;i<count;++i){if(i==release)e.release();e.tick();out.insert(out.end(),{std::uint32_t(e.level()),std::uint32_t(e.phase()),e.counter()});}
            write(argv[6],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="voice-sample" && argc==10) {
            const auto b=read(argv[2]);okage::SampleVoice v(b,std::stoul(argv[3],nullptr,0),std::stoul(argv[4],nullptr,0));
            v.parameters(std::stoul(argv[5]),std::stoi(argv[6]),std::stoi(argv[6]));
            const auto release=std::stoul(argv[7]),count=std::stoul(argv[8]);if(count>480000)throw std::runtime_error("Voice sample budget");std::vector<std::int16_t> out;
            for(unsigned i=0;i<count;++i){if(i==release)v.release();const auto p=v.tick();out.push_back(std::clamp(p[0],-32768,32767));out.push_back(std::clamp(p[1],-32768,32767));}
            write(argv[9],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*2));return 0;
        }
        if(command=="music-pitch" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%16)throw std::runtime_error("Pitch fixture extent");
            std::vector<std::uint32_t> out;
            for(std::size_t at=0;at<input.size();at+=16){std::array<std::uint32_t,4> p;std::memcpy(p.data(),input.data()+at,16);out.push_back(okage::music_note_pitch(p[0],p[1],p[2],p[3]));}
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="music-tone" && argc==5) {
            const auto bank=okage::read_music_bank(read(argv[2]));const auto input=read(argv[3]);
            if(input.empty() || input.size()%64)throw std::runtime_error("Tone fixture extent");std::vector<std::uint32_t> out;
            for(std::size_t at=0;at<input.size();at+=64){
                std::array<std::uint32_t,16> row;std::memcpy(row.data(),input.data()+at,64);
                okage::MusicChannel c;const auto* b=input.data()+at+16;
                c.volume=b[2];c.pan=b[3];c.expression=b[8];c.pitch=b[10];c.left=b[20]|(b[21]<<8);c.right=b[22]|(b[23]<<8);
                const auto& t=bank.programs.at(row[0]).at(row[1]);const auto p=okage::music_tone_parameters(bank,t,row[2],row[3],c,row[14],row[15]);
                const auto pitch=std::uint16_t((okage::music_note_pitch(p.center,p.fine,p.key,p.key_fine)*std::int32_t(p.pitch_scale))>>12);
                for(auto v:{t.sample_offset,std::uint32_t(t.adsr1),std::uint32_t(t.adsr2),std::uint32_t(std::uint16_t(p.left)),std::uint32_t(std::uint16_t(p.right)),std::uint32_t(pitch)})out.push_back(v);
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="music-bank" && argc==3) {
            const auto bank=okage::read_music_bank(read(argv[2]));
            for(unsigned i=0;i<bank.programs.size();++i)for(const auto& t:bank.programs[i])
                std::cout<<i<<'\t'<<unsigned(t.key_low)<<'\t'<<unsigned(t.key_high)<<'\t'<<t.sample_offset<<'\t'<<t.adsr1<<'\t'<<t.adsr2<<'\t'<<unsigned(t.center)<<'\t'<<unsigned(t.fine)<<'\t'<<t.pitch_scale<<'\n';
            return 0;
        }
        if(command=="music-sequence" && argc==5) {
            const auto ticks=std::stoul(argv[3]);if(!ticks || ticks>12000)throw std::runtime_error("Music tick budget");
            okage::MusicSequence sequence(read(argv[2]));std::vector<std::uint32_t> output;
            for(unsigned tick=1;tick<=ticks && !sequence.ended();++tick)sequence.tick([&](unsigned phase,const auto& s){
                const std::array<std::uint32_t,10> row={tick,phase,s.cursor(),__builtin_bit_cast(std::uint32_t, (s.remaining())),
                    s.tempo(),s.delta_scale(),s.clock_step(),s.status(),phase==1 ? s.next_status() : 0u,std::uint32_t(s.ended())};
                output.insert(output.end(),row.begin(),row.end());
                for(const auto& channel:s.channels){const auto words=channel.words();output.insert(output.end(),words.begin(),words.end());}
            });
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="field-projection" && argc==4) {
            const auto p=okage::field_projection(read(argv[2]));
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(p.data()),sizeof(p)));return 0;
        }
        if(command=="camera-angles" && argc==3) {
            const auto table=okage::make_atan_table();write(argv[2],okage::Bytes(reinterpret_cast<const std::uint8_t*>(table.data()),sizeof(table)));return 0;
        }
        if(command=="camera-anchored"&&argc==5) {
            const auto settings=okage::field_anchored_camera(read(argv[2]));const auto input=read(argv[3]);
            if(input.size()%16)throw std::runtime_error("Anchored camera position fixture extent");
            const auto angles=okage::make_atan_table();const auto sine=okage::make_sine_table();
            std::vector<okage::Float4> out;
            for(std::size_t at=0;at<input.size();at+=16) {
                okage::Float4 player;std::memcpy(player.data(),input.data()+at,16);
                const auto points=okage::anchored_camera(player,settings,angles,sine);
                out.push_back(points.position);out.push_back(points.target);
                out.push_back(okage::camera_look_at(points.position,points.target,angles,sine));
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*16));return 0;
        }
        if(command=="camera-look-at" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%32)throw std::runtime_error("Look-at fixture bounds");
            const auto table=okage::make_atan_table();const auto sine=okage::make_sine_table();std::vector<okage::Float4> out;
            for(std::size_t at=0;at<input.size();at+=32) {
                okage::Float4 p,t;std::memcpy(p.data(),input.data()+at,16);std::memcpy(t.data(),input.data()+at+16,16);
                out.push_back(okage::camera_look_at(p,t,table,sine));
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*16));return 0;
        }
        if(command=="dialogue-replay" && argc==8) {
            okage::DialogueText text(okage::read_pme(read(argv[2])).message(std::stoi(argv[3])));
            std::vector<okage::Font> fonts;
            for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts.push_back(okage::read_fnt(read(fs::path(argv[4])/name)));
            text.fonts=fonts;
            const auto trace=read(argv[5]),control=read(argv[6]);
            if(trace.empty() || trace.size()%160 || control.empty() || control.size()%512)throw std::runtime_error("Dialogue replay record bounds");
            std::array<std::uint32_t,40> r;std::memcpy(r.data(),trace.data(),160);
            if(r[1]!=0x1980f8)throw std::runtime_error("Dialogue replay initial text state");
            text.state.flags=r[10];text.state.progress=r[12];text.state.phase=r[16];text.state.period=r[17];text.state.delay=r[18];
            text.state.x=r[29];text.state.y=r[30];text.state.color=r[31];text.state.font=r[35];text.seek({r[13],r[14],r[15]!=0});
            okage::TextClock clock;clock.display_modes=r[11];clock.revealed=r[19];clock.scroll_target=__builtin_bit_cast(std::int32_t, (r[20]));
            clock.scroll=__builtin_bit_cast(float, (r[21]));clock.scroll_step=__builtin_bit_cast(float, (r[22]));clock.scroll_base_step=__builtin_bit_cast(float, (r[23]));clock.glyph_flags=r[32];
            std::array<std::uint32_t,64> c;std::memcpy(c.data(),control.data(),256);
            okage::DialogueController controller;controller.stage=c[7];controller.normal_period=__builtin_bit_cast(std::int32_t, (c[12]));
            controller.automatic=c[13]!=0;controller.window_type=__builtin_bit_cast(std::int32_t, (c[15]));
            controller.advance_count=c[16];controller.layer_flags=c[17];controller.layer_advance=c[21];
            if(c[18] || c[62])throw std::runtime_error("Dialogue replay choices outside scope");
            if((c[63]&255)==255)text.state.window.assign(1,char(255));
            else if(c[63]&255)throw std::runtime_error("Dialogue initial window command outside scope");
            okage::DialogueOpacity opacity;opacity.set(__builtin_bit_cast(float, (c[40])),__builtin_bit_cast(float, (c[43])),__builtin_bit_cast(float, (c[42])),__builtin_bit_cast(float, (c[41])));
            okage::DialogueWaitState wait{5,1};std::array<std::uint32_t,2> layers={c[22],c[23]};bool released=false;
            std::vector<std::uint32_t> result,wait_result;
            for(std::size_t at=0;at<control.size();at+=512) {
                std::memcpy(c.data(),control.data()+at,256);
                if(c[0]!=at/512 || c[1]!=0x148dc8)throw std::runtime_error("Dialogue input sequence mismatch");
                if(!released) {
                    const auto value=okage::poll_dialogue_wait(wait,2,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});
                    wait_result.insert(wait_result.end(),{wait.countdown,wait.observed,wait.accumulated,__builtin_bit_cast(std::uint32_t, (value))});released=value==0;
                }
                opacity.tick();text.frame(clock,opacity.visible());
                const auto pos=text.position();
                result.insert(result.end(),{text.state.flags,text.state.progress,std::uint32_t(pos.fragment),std::uint32_t(pos.cursor),pos.loaded?1u:0u,
                    text.state.phase,text.state.period,text.state.delay,clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll_target)),__builtin_bit_cast(std::uint32_t, (clock.scroll)),text.state.x,text.state.y});
                const auto effects=controller.update(text,{std::uint16_t(c[8]),std::uint16_t(c[9])});
                if(effects.close)opacity.set(128,-4);
                result.insert(result.end(),{text.state.flags,text.state.phase,text.state.period,text.state.delay,text.state.progress,
                    controller.advance_count,controller.layer_advance,controller.layer_flags,controller.stage,__builtin_bit_cast(std::uint32_t, (opacity.delta))});
            }
            write(fs::path(argv[7])/"frames.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(result.data()),result.size()*4));
            write(fs::path(argv[7])/"wait.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(wait_result.data()),wait_result.size()*4));return 0;
        }
        if(command=="dialogue-close" && argc==5) {
            const auto input=read(argv[2]);
            if(input.size()!=40)throw std::runtime_error("Dialogue closing fixture requires ten words");
            std::array<std::uint32_t,10> row;std::memcpy(row.data(),input.data(),40);
            unsigned stage=row[0];okage::DialogueClosure closure;closure.message_flags=row[1];
            auto seed=[&](okage::DialogueOpacity& o,unsigned at){o.set(__builtin_bit_cast(float, (row[at])),__builtin_bit_cast(float, (row[at+3])),__builtin_bit_cast(float, (row[at+2])),__builtin_bit_cast(float, (row[at+1])));};
            seed(closure.decoration,2);okage::DialogueOpacity body;seed(body,6);
            const auto frames=std::stoul(argv[3]);if(frames>10000)throw std::runtime_error("Dialogue close frame limit");
            std::vector<std::uint32_t> output;
            for(unsigned frame=0;frame<=frames;++frame) {
                output.insert(output.end(),{stage,closure.message_flags});
                for(const auto& opacity:{closure.decoration,body})
                    for(float v:{opacity.value,opacity.upper,opacity.lower,opacity.delta})output.push_back(__builtin_bit_cast(std::uint32_t, (v)));
                if(frame<frames){closure.decoration.tick();body.tick();closure.update(stage);}
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="dialogue-opacity" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%16)throw std::runtime_error("Opacity fixture bounds");
            std::vector<float> out;
            for(std::size_t at=0;at<input.size();at+=16) {
                std::array<float,4> row;std::memcpy(row.data(),input.data()+at,16);
                okage::DialogueOpacity opacity;opacity.set(row[0],row[3],row[2],row[1]);opacity.tick();
                out.insert(out.end(),{opacity.value,opacity.upper,opacity.lower,opacity.delta});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="text-glyphs" && argc==5) {
            std::vector<okage::Font> fonts;
            for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts.push_back(okage::read_fnt(read(fs::path(argv[2])/name)));
            const auto source=read(argv[3]);
            if(source.empty() || source.size()%28)throw std::runtime_error("Glyph fixture bounds");
            std::vector<float> out;
            for(std::size_t at=0;at<source.size();at+=28) {
                std::array<std::uint32_t,7> r;std::memcpy(r.data(),source.data()+at,28);
                if(r[0]<32 || r[0]>=128)throw std::runtime_error("Glyph fixture encoding");
                okage::DialogueText text({std::string(1,char(r[0]))});text.fonts=fonts;
                text.state.flags=0x8001;text.state.period=0;text.state.font=r[1];text.state.color=r[2];text.state.x=r[3];text.state.y=r[4];
                text.seek({0,0,true});okage::TextClock clock;text.frame(clock);
                if(text.glyphs.size()!=1)throw std::runtime_error("Glyph fixture did not emit");
                const auto& g=text.glyphs[0];
                const auto c=g.color;
                const auto packed=((c&255)>>2)+((c>>8&255)>>2)*64+((c>>16&255)>>2)*4096+(c>>26)*262144;
                out.insert(out.end(),{float(g.x),float(g.y),float(packed),float(g.width),float(g.height),float(g.code),float(g.font)});
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if((command=="font" || command=="font-metrics") && argc==4) {
            const auto font=okage::read_fnt(read(argv[2]));
            if(command=="font-metrics") {
                std::vector<std::uint8_t> out;
                for(const auto& g:font.glyphs)for(auto h:g.metrics) {out.push_back(h&255);out.push_back(h>>8);}
                write(argv[3],out);
            } else {
                const auto& t=font.atlas;
                const auto header="P7\nWIDTH "+std::to_string(t.width)+"\nHEIGHT "+std::to_string(t.height)+
                    "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
                std::vector<std::uint8_t> out(header.begin(),header.end());out.insert(out.end(),t.rgba.begin(),t.rgba.end());
                write(argv[3],out);
            }
            std::cout<<unsigned(font.first)<<'\t'<<unsigned(font.last)<<'\t'<<font.atlas.width<<'\t'<<font.atlas.height<<'\n';return 0;
        }
        if(command=="text-clock" && argc==4) {
            const auto source=read(argv[2]);
            if(source.empty() || source.size()%40)throw std::runtime_error("Text clock fixture size");
            std::vector<std::uint32_t> output;
            for(std::size_t at=0;at<source.size();at+=40) {
                std::array<std::uint32_t,10> r;std::memcpy(r.data(),source.data()+at,40);
                okage::TextState state;state.flags=r[0];state.phase=r[2];state.period=r[3];state.delay=r[4];
                okage::TextClock clock;clock.display_modes=r[1];clock.revealed=r[5];
                clock.scroll_target=__builtin_bit_cast(std::int32_t, (r[6]));clock.scroll=__builtin_bit_cast(float, (r[7]));
                clock.scroll_step=__builtin_bit_cast(float, (r[8]));clock.scroll_base_step=__builtin_bit_cast(float, (r[9]));
                okage::tick_text_clock(state,clock);
                output.insert(output.end(),{state.flags,state.phase,state.period,state.delay,clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll))});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if((command=="text-frame-replay" && (argc==6 || argc==8)) || (command=="text-layout-replay" && argc==7)) {
            const bool layout=command=="text-layout-replay";
            okage::DialogueText text(okage::read_pme(read(argv[2])).message(std::stoi(argv[3])));
            std::vector<okage::Font> fonts;
            if(layout) {
                for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts.push_back(okage::read_fnt(read(fs::path(argv[5])/name)));
                text.fonts=fonts;
            }
            const auto input=read(argv[4]);
            if(input.empty() || input.size()%160)throw std::runtime_error("Text trace fixture size");
            okage::TextClock clock;bool initialized=false;
            std::vector<std::uint32_t> output;
            okage::DialogueWaitState wait{5,1};
            std::array<std::uint32_t,2> layers{};
            std::vector<std::uint32_t> wait_output;
            const auto request=argc==8 ? std::stoi(argv[6]) : 0;
            if(argc==8 && request<=0)throw std::runtime_error("Text frame wait needs positive signal count");
            bool released=false;
            for(std::size_t at=0;at<input.size();at+=160) {
                std::array<std::uint32_t,40> r;std::memcpy(r.data(),input.data()+at,160);
                if(!initialized) {
                    if(r[1]!=0x1980f8)throw std::runtime_error("Text replay must start at a clock entry");
                    text.state.flags=r[10];text.state.progress=r[12];text.state.phase=r[16];text.state.period=r[17];text.state.delay=r[18];
                    text.state.x=r[29];text.state.y=r[30];text.state.color=r[31];text.state.font=r[35];
                    text.seek({r[13],r[14],r[15]!=0});
                    clock.display_modes=r[11];clock.revealed=r[19];clock.scroll_target=__builtin_bit_cast(std::int32_t, (r[20]));
                    clock.scroll=__builtin_bit_cast(float, (r[21]));clock.scroll_step=__builtin_bit_cast(float, (r[22]));clock.scroll_base_step=__builtin_bit_cast(float, (r[23]));
                    clock.glyph_flags=r[32];initialized=true;
                    layers={r[7],r[8]};
                }
                if(r[1]==0x1985c0)text.resume();
                else if(r[1]==0x1a51a0)text.state.period=r[4];
                else if(r[1]==0x1980f8) {
                    if(argc==8 && !released) {
                        const auto result=okage::poll_dialogue_wait(wait,request,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});
                        wait_output.insert(wait_output.end(),{wait.countdown,wait.observed,wait.accumulated,__builtin_bit_cast(std::uint32_t, (result))});
                        released=result==0;
                    }
                    text.frame(clock,r[24]!=0 || r[25]!=0);
                    const auto p=text.position();
                    output.insert(output.end(),{text.state.flags,text.state.progress,std::uint32_t(p.fragment),std::uint32_t(p.cursor),p.loaded ? 1u : 0u,
                        text.state.phase,text.state.period,text.state.delay,clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll_target)),__builtin_bit_cast(std::uint32_t, (clock.scroll))});
                    if(layout)output.insert(output.end(),{text.state.x,text.state.y});
                }
            }
            write(argv[layout ? 6 : 5],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));
            if(layout) {
                std::vector<std::uint32_t> glyphs;
                for(const auto& g:text.glyphs)glyphs.insert(glyphs.end(),{g.code,g.font,g.color,
                    __builtin_bit_cast(std::uint32_t, (g.x)),__builtin_bit_cast(std::uint32_t, (g.y)),std::uint32_t(g.width),std::uint32_t(g.height)});
                write(std::string(argv[6])+".glyphs.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(glyphs.data()),glyphs.size()*4));
                const auto t=okage::rasterize_text(fonts,text.glyphs,640,224);
                const std::string header="P7\nWIDTH 640\nHEIGHT 224\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
                std::vector<std::uint8_t> pam(header.begin(),header.end());pam.insert(pam.end(),t.rgba.begin(),t.rgba.end());
                write(std::string(argv[6])+".pam",pam);
            }
            if(argc==8)write(argv[7],okage::Bytes(reinterpret_cast<const std::uint8_t*>(wait_output.data()),wait_output.size()*4));
            return 0;
        }
        if(command=="pad-repeat" && argc==4){
            const auto input=read(argv[2]);if(input.empty() || input.size()%4)throw std::runtime_error("Pad fixture requires held/pressed halfwords");
            okage::DialoguePadRepeat pad;std::vector<std::uint32_t> output;
            for(std::size_t i=0;i<input.size();i+=4){
                const auto repeat=pad.update(input[i]|(input[i+1]<<8),input[i+2]|(input[i+3]<<8));
                output.insert(output.end(),{repeat,pad.previous,pad.ticks});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="pme-branch" && argc==7) {
            const auto bank=okage::read_pme(read(argv[2]));auto path=bank.message_path(std::stoi(argv[3]));
            std::vector<std::uint32_t> output;
            auto snapshot=[&]{output.push_back(path.strings.size());for(unsigned i=0;i<path.strings.size();++i){output.push_back(path.strings[i]);output.push_back(path.nodes[i]);}};
            snapshot();bank.select_branch(path,std::stoul(argv[4]),std::stoul(argv[5]));snapshot();
            write(argv[6],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(((command=="pme-message" || command=="pme-indices") && argc==5) || (command=="pme-name" && argc==6)) {
            auto bank=okage::read_pme(read(argv[2]));
            if(command=="pme-name" && !bank.bind("############",argv[4]))throw std::runtime_error("Missing player-name tag");
            std::vector<std::uint8_t> output;
            for(auto index:bank.message_indices(std::stoi(argv[3]))) {
                if(command=="pme-indices") {
                    for(unsigned i=0;i<4;++i)output.push_back(index>>(8*i));
                    continue;
                }
                const auto& text=bank.strings.at(index);
                const std::uint32_t size=text.size();
                for(unsigned i=0;i<4;++i)output.push_back(size>>(8*i));
                output.insert(output.end(),text.begin(),text.end());
            }
            write(argv[command=="pme-name" ? 5 : 4],output);return 0;
        }
        if(command=="pme-info" && argc==3) {
            const auto bank=okage::read_pme(read(argv[2]));
            std::cout<<bank.strings.size()<<" strings, "<<bank.nodes.size()<<" nodes\n";return 0;
        }
        if(command=="text-wait" && argc==6) {
            okage::DialogueText text(okage::read_pme(read(argv[2])).message(std::stoi(argv[3])));
            const auto request=std::stoi(argv[4]);
            if(request<=0)throw std::runtime_error("Text wait diagnostic requires a positive signal count");
            okage::DialogueWaitState wait{5,1};
            const std::array<std::uint32_t,2> layers{0x40,0x40};
            std::vector<std::uint32_t> output;
            for(unsigned step=0;step<100000;++step) {
                const auto kind=text.step();
                const auto result=okage::poll_dialogue_wait(wait,request,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});
                output.insert(output.end(),{step,std::uint32_t(kind),text.state.progress,__builtin_bit_cast(std::uint32_t, (result)),
                                            wait.countdown,wait.observed,wait.accumulated});
                if(result==0) {
                    write(argv[5],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));
                    std::cout<<"Native text released event wait at semantic step "<<step<<" (not a frame count)\n";return 0;
                }
                if(kind==okage::TextStep::pause)text.resume();
                if(kind==okage::TextStep::end && !wait.countdown && wait.observed==text.state.progress)
                    throw std::runtime_error("Message ended before requested signal count");
            }
            throw std::runtime_error("Text wait diagnostic step limit");
        }
        if(command=="text-controls" && argc==4) {
            const auto bytes=read(argv[2]);
            const std::string text(bytes.begin(),bytes.end());
            okage::TextState state;state.x=5;state.y=7;state.line_origin=3;
            std::vector<std::uint8_t> output;
            auto word=[&](std::uint32_t n){for(unsigned i=0;i<4;++i)output.push_back(n>>(8*i));};
            std::size_t cursor=0;
            while(cursor<text.size()) {
                const auto result=okage::consume_text_control(text,cursor,state);
                for(auto n:{std::uint32_t(cursor),state.flags,state.progress,state.color,state.previous_color,
                    state.x,state.y,state.line_origin,state.line_height,state.font,state.period,state.phase,state.delay,result})word(n);
                for(const auto* value:{&state.speaker,&state.window,&state.option,&state.replacement,&state.description}) {
                    word(value->size());output.insert(output.end(),value->begin(),value->end());
                }
                if(!result)cursor+=static_cast<unsigned char>(text[cursor])>=0x80 ? 2 : 1;
            }
            write(argv[3],output);return 0;
        }
        if(command=="spu-sample" && argc==4) {
            const auto sample=okage::decode_spu_sample(read(argv[2]));
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(sample.pcm.data()),sample.pcm.size()*2));
            std::cout<<"Native SPU sample: "<<sample.pcm.size()<<" samples, "<<sample.consumed
                     <<" bytes, end flags "<<unsigned(sample.end_flags)<<", loop sample "
                     <<(sample.loop_sample ? std::to_string(*sample.loop_sample) : "none")<<'\n';return 0;
        }
        if(command=="dialogue-wait" && argc==4) {
            const auto cases=read(argv[2]);
            if(cases.empty() || cases.size()%48)throw std::runtime_error("Dialogue fixture record bounds");
            std::vector<std::uint32_t> output;
            for(std::size_t at=0;at<cases.size();at+=48) {
                std::array<std::uint32_t,12> in;std::memcpy(in.data(),cases.data()+at,48);
                if(in[8]>16 || (in[10]!=0 && in[10]!=0xffffffff) || in[6]>1)
                    throw std::runtime_error("Dialogue fixture input bounds");
                std::vector<std::uint32_t> layers;
                for(unsigned i=0;i<in[8];++i)layers.push_back((in[9]&(1u<<i)) ? 0x40 : 0);
                okage::DialogueWaitState state{in[1],in[2],in[3],in[4],in[5]};
                const okage::DialogueObservation dialogue{in[6]!=0,__builtin_bit_cast(std::int32_t, (in[7])),layers,
                    in[10]==0 ? __builtin_bit_cast(std::int32_t, (in[11])) : -1};
                const auto result=okage::poll_dialogue_wait(state,__builtin_bit_cast(std::int32_t, (in[0])),dialogue);
                output.insert(output.end(),{state.countdown,state.observed,state.accumulated,__builtin_bit_cast(std::uint32_t, (result))});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="exterior-camera-step"&&argc==5) {
            const auto map=okage::read_xcl(read(argv[2]));const auto data=read(argv[3]);
            if(data.empty()||data.size()%184||data.size()>184*2000)throw std::runtime_error("Exterior camera fixture extent");
            const auto angles=okage::make_atan_table();const auto sine=okage::make_sine_table();
            std::vector<std::uint32_t> output;
            for(unsigned at=0;at<data.size();at+=184) {
                std::array<std::uint32_t,46> row;std::memcpy(row.data(),data.data()+at,184);
                auto f=[&](unsigned i){return __builtin_bit_cast(float, (row[i]));};
                auto n=[&](unsigned i){return __builtin_bit_cast(std::int32_t, (row[i]));};
                auto point=[&](unsigned i){return okage::Float4{f(i),f(i+1),f(i+2),f(i+3)};};
                auto number=[&](unsigned i,float value){row[i]=__builtin_bit_cast(std::uint32_t, (value));};
                auto store=[&](unsigned i,const okage::Float4& value){for(unsigned k=0;k<4;++k)number(i+k,value[k]);};
                okage::ExteriorCamera c;
                c.preset=n(0);c.obstruction_clock=n(1);c.fast_turn=row[2]!=0;c.distance=n(4);
                c.distance_scale=f(5);c.target_height=f(6);c.height=f(7);c.turn_rate=f(8);c.saved_scale=f(9);
                c.desired_height=f(10);c.maximum_height=f(11);c.phase=f(14);c.manual_rotate=row[16]!=0;
                c.snap_target=row[18]!=0;c.target_reset_counter=n(19);c.target_reset_clock=n(20);c.target_y=f(21);
                c.previous_player=point(22);c.angular_velocity=f(26);c.target_offset=f(27);
                const auto result=c.tick(point(28),point(32),point(36),row[44],row[45],map,angles,sine);
                row[0]=c.preset;row[1]=c.obstruction_clock;row[4]=c.distance;
                number(5,c.distance_scale);number(6,c.target_height);number(7,c.height);number(8,c.turn_rate);number(9,c.saved_scale);
                number(10,c.desired_height);number(11,c.maximum_height);number(14,c.phase);row[18]=c.snap_target;
                row[19]=c.target_reset_counter;row[20]=c.target_reset_clock;number(21,c.target_y);
                store(22,c.previous_player);number(26,c.angular_velocity);number(27,c.target_offset);
                store(36,result.position);store(40,result.target);output.insert(output.end(),row.begin(),row.end());
            }
            write(argv[4],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="enemy-step" && argc==4) {
            const auto bytes=read(argv[2]);
            if(bytes.empty()||bytes.size()%144||bytes.size()>144*2000)throw std::runtime_error("Enemy fixture extent");
            const auto angles=okage::make_atan_table();const auto sine=okage::make_sine_table();
            std::vector<std::uint32_t> output;const auto old=std::fegetround();std::fesetround(FE_TOWARDZERO);
            for(std::size_t at=0;at<bytes.size();at+=144) {
                std::array<std::uint32_t,36> row;
                for(unsigned i=0;i<36;++i)row[i]=std::uint32_t(bytes[at+4*i])|(std::uint32_t(bytes[at+4*i+1])<<8)|
                    (std::uint32_t(bytes[at+4*i+2])<<16)|(std::uint32_t(bytes[at+4*i+3])<<24);
                auto point=[&](unsigned i){okage::Float4 p;for(unsigned k=0;k<4;++k)p[k]=__builtin_bit_cast(float, (row[i+k]));return p;};
                auto store=[&](unsigned i,const okage::Float4& p){for(unsigned k=0;k<4;++k)row[i+k]=__builtin_bit_cast(std::uint32_t, (p[k]));};
                okage::EncounterEnemy enemy{__builtin_bit_cast(std::int32_t, (row[0])),__builtin_bit_cast(std::int32_t, (row[1])),__builtin_bit_cast(float, (row[2]))};
                okage::GameRandom random{row[3]};
                okage::EnemyObservation in{point(12),point(16),point(20),point(24),point(28),point(32),
                    __builtin_bit_cast(std::int32_t, (row[4])),__builtin_bit_cast(std::int32_t, (row[5])),__builtin_bit_cast(std::int32_t, (row[6])),__builtin_bit_cast(std::int32_t, (row[7]))};
                const auto action=enemy.tick(in,[&]{return random.next();},angles,sine);
                row[0]=__builtin_bit_cast(std::uint32_t, (enemy.counter));row[1]=__builtin_bit_cast(std::uint32_t, (enemy.cooldown));
                row[2]=__builtin_bit_cast(std::uint32_t, (enemy.speed));row[3]=random.state;
                if(action.visible)row[8]=*action.visible;
                if(action.translucent)row[9]|=0x100000;
                if(action.rear_attack)row[10]=1;
                if(action.encounter_column){row[11]=*action.encounter_column;row[6]=action.field_mode;}
                if(action.position)store(12,*action.position);
                if(action.rotation)store(16,*action.rotation);
                if(action.color)store(20,*action.color);
                if(action.alpha)row[23]=__builtin_bit_cast(std::uint32_t, (*action.alpha));
                if(action.displacement){auto p=point(12);for(unsigned k=0;k<4;++k)p[k]+=(*action.displacement)[k];store(12,p);}
                output.insert(output.end(),row.begin(),row.end());
            }
            std::fesetround(old);
            write(argv[3],{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});return 0;
        }
        if(command=="map-motion" && argc==8) {
            okage::MapObjects map(argv[2]);const auto id=std::stoi(argv[3]);
            auto selected=std::find_if(map.bindings().begin(),map.bindings().end(),[&](const auto& b){return b.alias==id;});
            if(selected==map.bindings().end())throw std::runtime_error("Map instance has no native animation binding");
            const auto frames=std::stoul(argv[5]);if(!frames||frames>2000)throw std::runtime_error("Map motion frame budget");
            std::ifstream input(argv[4]);if(!input)throw std::runtime_error("Cannot read map motion replay");
            std::vector<std::pair<unsigned,unsigned>> selections;unsigned frame,entry;
            while(input>>frame) {
                if(!(input>>entry))throw std::runtime_error("Incomplete map motion replay pair");
                if(frame>=frames||entry>=0x800||(!selections.empty()&&frame<=selections.back().first))
                    throw std::runtime_error("Map motion replay bounds/order");
                selections.emplace_back(frame,entry);
            }
            if(!input.eof()||selections.empty())throw std::runtime_error("Invalid map motion replay");
            auto& object=*selected->actor;const auto channels=std::stoul(argv[6]);
            if(object.channels().size()!=channels)throw std::runtime_error("Map motion channel count mismatch");
            std::vector<std::uint32_t> out;std::size_t selection=0;
            for(unsigned i=0;i<frames;++i) {
                if(selection<selections.size()&&selections[selection].first==i)object.select_motion(selections[selection++].second);
                object.tick_script();if(object.visible){object.tick_channels();object.evaluate();}
                out.insert(out.end(),{i,object.animation().pc(),object.animation().script_state(),unsigned(object.visible),unsigned(id),unsigned(channels)});
                for(const auto& v:object.channels())for(float f:v)out.push_back(__builtin_bit_cast(std::uint32_t, (f)));
            }
            write(argv[7],{reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4});return 0;
        }
        if(command=="field-setup" && argc==7) {
            const auto definition=okage::read_field_definition(read(argv[2]),std::stoi(argv[3]));
            const auto state=okage::field_setup_state(read(argv[4]),read(argv[5]),definition);
            write(argv[6],state);return 0;
        }
        if(command=="event-setup" && argc==6) {
            const auto script=read(argv[2]),variables=read(argv[3]);auto state=read(argv[4]);
            const auto pcs=okage::run_event_setup(script,variables,state);
            write(std::string(argv[5])+".state.bin",state);
            write(std::string(argv[5])+".pcs.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(pcs.data()),pcs.size()*4));
            std::cout<<"Native event setup completed "<<pcs.size()<<" instructions\n";return 0;
        }
        if(command=="snapshot-pose" && argc==5) {
            const auto model=okage::read_xmd(read(argv[2]));
            if(model.models.size()!=1)throw std::runtime_error("Snapshot requires one model");
            const auto bones=read(std::string(argv[3])+".bones.bin");
            const auto worldBytes=read(std::string(argv[3])+".world.bin"),viewBytes=read(std::string(argv[3])+".camera.bin");
            if(bones.size()!=model.models[0].bones.size()*48 || worldBytes.size()!=64 || viewBytes.size()!=48)
                throw std::runtime_error("Snapshot/model extent mismatch");
            okage::Matrix4 world;std::memcpy(world.data(),worldBytes.data(),64);
            okage::Float4 cameraPosition,cameraRotation,cameraScale;
            std::memcpy(cameraPosition.data(),viewBytes.data(),16);std::memcpy(cameraRotation.data(),viewBytes.data()+16,16);
            std::memcpy(cameraScale.data(),viewBytes.data()+32,16);
            const auto sine=okage::make_sine_table();
            const auto view=okage::camera_matrix(cameraPosition,cameraRotation,cameraScale,sine);
            std::vector<okage::Float4> channels(4+3*model.models[0].bones.size());
            std::memcpy(channels.data()+4,bones.data(),bones.size());
            const auto local=okage::animated_pose(model.models[0],channels,sine,true);
            const auto parent=okage::compose_pose(view,std::span(&world,1));
            const auto result=okage::compose_pose(parent[0],local);
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(result.data()),result.size()*64));return 0;
        }
        if ((command=="instance-matrices" || command=="camera-matrices") && argc==4) {
            const auto bytes=read(argv[2]);
            if(bytes.empty() || bytes.size()%48)throw std::runtime_error("Instance fixture requires 48-byte channel triples");
            std::vector<std::uint8_t> output;const auto sine=okage::make_sine_table();
            for(std::size_t at=0;at<bytes.size();at+=48) {
                okage::Float4 position,rotation,scale;
                std::memcpy(position.data(),bytes.data()+at,16);std::memcpy(rotation.data(),bytes.data()+at+16,16);
                std::memcpy(scale.data(),bytes.data()+at+32,16);
                const auto matrix=command=="camera-matrices" ? okage::camera_matrix(position,rotation,scale,sine) :
                    okage::instance_matrix(position,rotation,scale,sine);
                const auto* first=reinterpret_cast<const std::uint8_t*>(&matrix);output.insert(output.end(),first,first+64);
            }
            write(argv[3],output);return 0;
        }
        if(command=="map-facing-matrices" && argc==5) {
            const auto map=okage::read_xmp(read(argv[2]));const auto cameras=read(argv[3]);
            if(cameras.empty()||cameras.size()%16)throw std::runtime_error("Camera-facing matrix fixture extent");
            const auto sine=okage::make_sine_table();std::vector<std::uint8_t> output;
            for(std::size_t at=0;at<cameras.size();at+=16) {
                okage::Float4 rotation;std::memcpy(rotation.data(),cameras.data()+at,16);
                for(const auto& placement:map.placements) {
                    const auto transform=okage::map_transform(placement,map.definitions.at(placement.definition()));
                    if(placement.handler()||!(transform.flags&1))continue;
                    const auto matrix=okage::map_instance_matrix(transform,rotation,sine);
                    const auto* bytes=reinterpret_cast<const std::uint8_t*>(&matrix);output.insert(output.end(),bytes,bytes+sizeof(matrix));
                }
            }
            write(argv[4],output);return 0;
        }
        if ((command=="map" && argc==3) || (command=="map-transforms" && argc==4)) {
            const auto map=okage::read_xmp(read(argv[2]));
            if (command=="map") {
                std::cout<<"map\t"<<map.definitions.size()<<'\t'<<map.placements.size()<<'\n';
                for (std::size_t i=0;i<map.definitions.size();++i)
                    std::cout<<"definition\t"<<i<<'\t'<<map.definitions[i].name<<'\t'<<map.definitions[i].flags()<<'\n';
            }
            std::vector<std::uint8_t> result;
            auto append=[&](std::uint32_t word){for(unsigned i=0;i<4;++i) result.push_back(word>>(i*8));};
            for (std::size_t i=0;i<map.placements.size();++i) {
                const auto& p=map.placements[i];const auto t=okage::map_transform(p,map.definitions.at(p.definition()));
                if (command=="map") {
                    std::cout<<"placement\t"<<i<<'\t'<<p.definition()<<'\t'<<p.handler()<<'\t'<<t.flags;
                    for (const auto& v:{t.position,t.rotation,t.scale}) for (auto x:v) std::cout<<'\t'<<x;
                    std::cout<<'\n';
                }
                for (const auto& v:{t.position,t.rotation,t.scale}) for(auto x:v) append(__builtin_bit_cast(std::uint32_t, (x)));
                append(t.flags);
            }
            if (command=="map-transforms") write(argv[3],result);
            return 0;
        }
        if (command=="uv-model-cases" && argc==4) {
            const fs::path path=argv[2];
            const auto file=okage::read_xmd(read(path));
            std::vector<okage::Texture> images;
            for (auto name:file.textures) {
                if (fs::path(name).is_absolute() || fs::path(name).has_parent_path())
                    throw std::runtime_error("Unsafe model texture path");
                if (fs::path(name).extension().empty()) name+=".xim";
                images.push_back(okage::decode_xim(read(path.parent_path()/name)));
            }
            std::vector<std::uint8_t> cases;
            auto append=[&](std::uint32_t word) {for (unsigned i=0;i<4;++i) cases.push_back(word>>(i*8));};
            for (const auto& model:file.models) for (const auto& block:model.blocks) {
                std::vector<okage::Float4> coords(block.attribute_extents[3]);
                std::vector<bool> defined(coords.size());
                for (const auto& range:block.attributes) if (range.kind==8) {
                    for (std::size_t i=0;i<range.values.size();++i) {
                        if (defined.at(range.first+i)) throw std::runtime_error("Overlapping source UV ranges");
                        coords.at(range.first+i)=range.values[i]; defined.at(range.first+i)=true;
                    }
                }
                for (const auto& polygon:block.polygons) {
                    if (polygon.texture_state<0) continue;
                    const auto& state=block.textures.at(polygon.texture_state);
                    if (state.texture<0) continue;
                    const auto& image=images.at(state.texture);
                    for (const auto& corner:polygon.corners) {
                        if (corner[3]<0 || !defined.at(corner[3])) throw std::runtime_error("Missing polygon UV");
                        append(image.width); append(image.height);
                        append(std::uint32_t(std::int32_t(state.u_mode))); append(std::uint32_t(std::int32_t(state.v_mode)));
                        for (float value:state.region) append(__builtin_bit_cast(std::uint32_t, (value)));
                        append(__builtin_bit_cast(std::uint32_t, (coords.at(corner[3])[0])));
                        append(__builtin_bit_cast(std::uint32_t, (coords.at(corner[3])[1])));
                    }
                }
            }
            write(argv[3],cases); return 0;
        }
        if ((command=="uv-cases" || command=="texture-clamp-cases") && argc==4) {
            const auto cases=read(argv[2]);
            if (cases.empty() || cases.size()%40 || cases.size()>40*65536)
                throw std::runtime_error("Invalid UV fixture records");
            auto word=[&](std::size_t at) {
                return std::uint32_t(cases[at])|(std::uint32_t(cases[at+1])<<8)|
                    (std::uint32_t(cases[at+2])<<16)|(std::uint32_t(cases[at+3])<<24);
            };
            std::vector<std::uint8_t> result;
            for (std::size_t at=0;at<cases.size();at+=40) {
                const auto u=__builtin_bit_cast(std::int32_t, (word(at+8))),v=__builtin_bit_cast(std::int32_t, (word(at+12)));
                if (u<-32768 || u>32767 || v<-32768 || v>32767) throw std::runtime_error("UV fixture mode overflow");
                okage::TextureState state{0,0,std::int16_t(u),std::int16_t(v),{}};
                for (unsigned i=0;i<4;++i) state.region[i]=__builtin_bit_cast(float, (word(at+16+i*4)));
                if (command=="texture-clamp-cases") {
                    const auto bits=okage::texture_clamp(state,word(at),word(at+4));
                    for (unsigned i=0;i<8;++i) result.push_back(bits>>(i*8));
                    continue;
                }
                const auto uv=okage::texture_coordinates(state,word(at),word(at+4),
                    {__builtin_bit_cast(float, (word(at+32))),__builtin_bit_cast(float, (word(at+36)))});
                for (float value:uv) {
                    auto bits=__builtin_bit_cast(std::uint32_t, (value));
                    for (unsigned i=0;i<4;++i) result.push_back(bits>>(i*8));
                }
            }
            write(argv[3],result); return 0;
        }
        if (command=="texture-callback" && argc==5) {
            const auto model=okage::read_xmd(read(argv[2]));
            const auto text=read(argv[3]);
            okage::TextureBindings textures(model,okage::read_xta(std::string_view(reinterpret_cast<const char*>(text.data()),text.size())));
            std::vector<std::int32_t> dump;
            for (std::size_t group=0;group<textures.animation().groups.size();++group)
                for (std::size_t variant=0;variant<=textures.animation().variants.size();++variant) {
                    textures.restore(); textures.select(group,variant);
                    dump.push_back(group); dump.push_back(variant);
                    dump.insert(dump.end(),textures.images().begin(),textures.images().end());
                }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(dump.data()),dump.size()*4)); return 0;
        }
        if (command == "animation" && (argc >= 7 && argc <= 9)) {
            const auto count=std::stoul(argv[3]), entry=std::stoul(argv[4]), frames=std::stoul(argv[5]);
            if (count>5120 || frames>10000 || (frames+1)*(8+count*16)>32*1024*1024)
                throw std::runtime_error("Animation dump exceeds bounds");
            okage::Animation animation(read(argv[2]),count);
            okage::ModelFile model;
            const auto sine=okage::make_sine_table();
            if (argc>=8) {
                model=okage::read_xmd(read(argv[7]));
                if (model.models.size()!=1 || (frames+1)*(8+count*16+model.models[0].hierarchy.size()*64)>32*1024*1024)
                    throw std::runtime_error("Animation pose dump exceeds bounds");
            }
            std::unique_ptr<okage::TextureBindings> textures;
            if (argc==9) {
                const auto text=read(argv[8]);
                textures=std::make_unique<okage::TextureBindings>(model,okage::read_xta(
                    std::string_view(reinterpret_cast<const char*>(text.data()),text.size())));
                if ((frames+1)*(8+count*16+model.models[0].hierarchy.size()*64+textures->images().size()*4)>32*1024*1024)
                    throw std::runtime_error("Texture animation dump exceeds bounds");
                animation.set_texture_callback([&](std::size_t group,std::size_t variant) {textures->select(group,variant);});
            }
            animation.start(0); animation.values(); animation.start(entry);
            std::vector<std::uint8_t> dump;
            auto append=[&](const void* data,std::size_t size) {
                auto* p=static_cast<const std::uint8_t*>(data); dump.insert(dump.end(),p,p+size);
            };
            for (std::size_t frame=0;frame<=frames;++frame) {
                if (frame) animation.tick();
                const std::uint32_t state[]={animation.pc(),animation.script_state()};
                append(state,sizeof(state));
                const auto values=animation.values(); append(values.data(),values.size()*sizeof(okage::Float4));
                if (argc>=8) {
                    const auto pose=okage::animated_pose(model.models[0],values,sine,animation.scale_channels());
                    append(pose.data(),pose.size()*sizeof(okage::Matrix4));
                }
                if (textures) append(textures->images().data(),textures->images().size()*4);
            }
            write(argv[6],dump); return 0;
        }
        if (command == "sine-table" && argc == 3) {
            const auto table=okage::make_sine_table();
            write(argv[2],okage::Bytes(reinterpret_cast<const std::uint8_t*>(table.data()),sizeof(table)));
            return 0;
        }
        if (command == "pose" && argc == 4) {
            const auto file=okage::read_xmd(read(argv[2]));
            if (file.models.size()!=1) throw std::runtime_error("Pose dump expects one model");
            const auto pose=okage::rest_pose(file.models[0],okage::make_sine_table());
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(pose.data()),pose.size()*sizeof(okage::Matrix4)));
            return 0;
        }
        if (command == "pose-corpus" && argc == 4) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2]))
                if (entry.is_regular_file() && entry.path().extension()==".xmd") files.push_back(entry.path());
            std::sort(files.begin(),files.end());
            if (files.empty()) throw std::runtime_error("No XMD models found");
            const auto sine=okage::make_sine_table();
            for (const auto& path: files) {
                const auto model=okage::read_xmd(read(path));
                if (model.models.size()!=1) throw std::runtime_error("Pose corpus expects one model per file");
                const auto pose=okage::rest_pose(model.models[0],sine);
                auto output=fs::path(argv[3])/fs::relative(path,argv[2]); output.replace_extension(".pose");
                write(output,okage::Bytes(reinterpret_cast<const std::uint8_t*>(pose.data()),pose.size()*sizeof(okage::Matrix4)));
                std::cout<<path.string()<<'\t'<<output.string()<<'\t'<<pose.size()<<'\n';
            }
            return 0;
        }
        if (command == "model" && argc == 3) { model_report(argv[2]); return 0; }
        if (command == "model-check" && argc == 3) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
                auto extension=entry.path().extension().string();
                if (entry.is_regular_file() && (extension==".xmd" || extension==".XMD")) files.push_back(entry.path());
            }
            std::sort(files.begin(),files.end());
            if (files.empty()) throw std::runtime_error("No XMD models found");
            for (const auto& path : files) {
                try { model_report(path); }
                catch (const std::exception& e) { throw std::runtime_error(path.string()+": "+e.what()); }
            }
            return 0;
        }
        if (command == "texture-check" && argc == 3) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
                auto extension = entry.path().extension().string();
                if (entry.is_regular_file() && (extension == ".xim" || extension == ".XIM")) files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& path : files) {
                try {
                    const auto texture = okage::decode_xim(read(path));
                    std::cout << path.string() << '\t' << texture.width << '\t' << texture.height << '\t'
                              << texture.pixel_format << '\t' << texture.palette_format << '\n';
                } catch (const std::exception& e) { throw std::runtime_error(path.string() + ": " + e.what()); }
            }
            return 0;
        }
        const auto bytes = read(argv[2]);
        if (command == "texture" && argc == 4) {
            const auto texture = okage::decode_xim(bytes);
            const auto header = "P7\nWIDTH " + std::to_string(texture.width) + "\nHEIGHT " + std::to_string(texture.height) +
                                "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
            std::vector<std::uint8_t> pam(header.begin(), header.end());
            pam.insert(pam.end(), texture.rgba.begin(), texture.rgba.end());
            write(argv[3], pam);
            std::cout << texture.width << '\t' << texture.height << '\t' << texture.pixel_format << '\t' << texture.palette_format << '\n';
        } else if (command == "decode" && argc == 4) {
            auto result = okage::decompress_xpf(bytes);
            write(argv[3], result.bytes);
            std::cout << result.bytes.size() << '\t' << result.consumed << '\n';
        } else if ((command == "list" && argc == 3) || (command == "extract" && argc == 4)) {
            const auto entries = okage::read_xpf(bytes);
            for (const auto& entry : entries) {
                auto result = okage::decompress_xpf(okage::Bytes(bytes).subspan(entry.offset, entry.packed_size));
                if (command == "extract") write(fs::path(argv[3]) / entry.name, result.bytes);
                std::cout << entry.name << '\t' << entry.offset << '\t' << entry.packed_size
                          << '\t' << result.bytes.size() << '\t' << result.consumed << '\n';
            }
        } else throw std::runtime_error("Invalid command or arguments");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "okage-assets: " << e.what() << '\n';
        return 1;
    }
}
