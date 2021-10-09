#include "okage/model.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace okage {
namespace {
[[noreturn]] void fail(const char* why) { throw std::runtime_error(why); }
struct Reader {
    Bytes bytes;
    void range(std::size_t p, std::size_t n) const {
        if (p > bytes.size() || n > bytes.size()-p) fail("XMD field outside bounded section");
    }
    std::uint16_t u16(std::size_t p) const {
        range(p,2); return std::uint16_t(bytes[p]) | std::uint16_t(bytes[p+1])<<8;
    }
    std::int16_t i16(std::size_t p) const { return __builtin_bit_cast(std::int16_t, (u16(p))); }
    std::uint32_t u32(std::size_t p) const {
        return std::uint32_t(u16(p)) | std::uint32_t(u16(p+2))<<16;
    }
    std::int32_t i32(std::size_t p) const { return __builtin_bit_cast(std::int32_t, (u32(p))); }
    float f32(std::size_t p) const { return __builtin_bit_cast(float, (u32(p))); }
    Float4 floats(std::size_t p) const { return {f32(p),f32(p+4),f32(p+8),f32(p+12)}; }
    std::vector<std::string> names(std::size_t p) const {
        const auto count=u32(p); p+=4;
        range(p,count);
        std::vector<std::string> result;
        for (std::uint32_t i=0; i<count; ++i) {
            std::string name;
            for (;;) {
                range(p,1); const auto c=bytes[p++];
                if (!c) break;
                name.push_back(static_cast<char>(c));
            }
            if (name.empty()) fail("Empty XMD name");
            result.push_back(std::move(name));
        }
        return result;
    }
};

RenderBlock block(const Reader& source, std::size_t& at, const Model& model, std::size_t texture_count) {
    RenderBlock result{};
    result.offset=at; result.bounds=source.floats(at); at+=16;
    std::int32_t hierarchy=-1, texture=-1, material=-1;
    bool drawing=false;
    std::array<std::vector<bool>,4> available;
    for (;;) {
        const auto word=source.u32(at);
        const std::size_t length=word&0xffffff;
        const auto opcode=static_cast<std::uint8_t>(word>>24);
        if (length<4 || length%4) fail("Invalid XMD command extent");
        source.range(at,length);
        Reader cmd{source.bytes.subspan(at,length)};
        result.commands.push_back({at,length,opcode});
        if ((opcode==1 || opcode>8) && !drawing) { result.draw_offset=at; drawing=true; }
        if (drawing && opcode>=2 && opcode<=8) fail("XMD attribute command follows draw commands");
        if (opcode==1) {
            if (length!=4) fail("Unsupported XMD end command");
            at+=length; result.end=at; break;
        }
        switch (opcode) {
        case 2:
            if ((length-4)%24 || result.texture_command) fail("Invalid XMD texture state table");
            result.texture_command=at;
            for (std::size_t p=4; p<length; p+=24) {
                TextureState state{cmd.i16(p),cmd.i16(p+2),cmd.i16(p+4),cmd.i16(p+6),cmd.floats(p+8)};
                if (state.texture>=0 && std::size_t(state.texture)>=texture_count)
                    fail("XMD texture state references missing texture");
                result.textures.push_back(state);
            }
            break;
        case 3:
            if ((length-4)%32 || result.material_command) fail("Invalid XMD material table");
            result.material_command=at;
            for (std::size_t p=4; p<length; p+=32) {
                Material entry{};
                for (std::size_t j=0; j<8; ++j) entry.words[j]=cmd.u32(p+j*4);
                result.materials.push_back(entry);
            }
            break;
        case 4:
            if (length!=8) fail("Invalid XMD hierarchy selection");
            hierarchy=cmd.i32(4);
            if (hierarchy<0 || std::size_t(hierarchy)>=model.hierarchy.size())
                fail("XMD attribute references missing hierarchy node");
            result.commands.back().hierarchy_node=hierarchy;
            break;
        case 5: case 6: case 7: case 8: {
            AttributeRange entry{opcode,cmd.u16(4),cmd.u16(8),hierarchy,{}};
            if (cmd.u16(6)) fail("Unsupported XMD attribute frame sequence");
            if (entry.flags!=(opcode<=6 ? 1 : 0)) fail("Unsupported XMD attribute flags");
            const auto count=cmd.u16(10);
            const std::size_t stride=opcode==8 ? 8 : 16;
            if (length!=12+std::size_t(count)*stride) fail("XMD attribute payload size mismatch");
            if (opcode<=6 && hierarchy<0) fail("XMD transformed attribute has no hierarchy node");
            for (std::size_t i=0; i<count; ++i) {
                const auto p=12+i*stride;
                entry.values.push_back(opcode==8 ? Float4{cmd.f32(p),cmd.f32(p+4),0,0} : cmd.floats(p));
            }
            const auto end=std::uint32_t(entry.first)+count;
            if (end>32768) fail("XMD attribute extent exceeds signed index space");
            auto& present=available[opcode-5];
            present.resize(std::max(present.size(),std::size_t(end)),false);
            std::fill(present.begin()+entry.first,present.begin()+end,true);
            result.attribute_extents[opcode-5]=std::max(result.attribute_extents[opcode-5],end);
            result.commands.back().attribute=static_cast<std::int32_t>(result.attributes.size());
            result.attributes.push_back(std::move(entry));
            break;
        }
        case 9: case 10: {
            if (length!=8) fail("Invalid XMD material/texture selection");
            const auto value=cmd.i32(4);
            const auto count=opcode==9 ? result.textures.size() : result.materials.size();
            if (value>=0 && std::size_t(value)>=count) fail("XMD selection outside state table");
            (opcode==9 ? texture : material)=value;
            break;
        }
        case 11: case 12: {
            const std::size_t corners=opcode==11 ? 3 : 4;
            if (length<8 || (length-8)%(corners*8)) fail("XMD polygon payload size mismatch");
            for (std::size_t p=8; p<length; p+=corners*8) {
                Polygon polygon{cmd.u32(4),texture,material,{}};
                for (std::size_t j=0; j<corners; ++j) {
                    std::array<std::int16_t,4> indices{};
                    for (std::size_t k=0; k<4; ++k) {
                        const auto index=cmd.i16(p+j*8+k*2); indices[k]=index;
                        bool required=k==0 || (k==1 && index>=0) || (k==2 && index>=0);
                        if (k==3 && texture>=0 && result.textures[texture].texture>=0) required=true;
                        if (required && (index<0 || std::size_t(index)>=available[k].size() || !available[k][index]))
                            fail("XMD polygon references absent attribute");
                    }
                    polygon.corners.push_back(indices);
                }
                result.polygons.push_back(std::move(polygon));
            }
            break;
        }
        default: fail("Unsupported XMD command opcode");
        }
        at+=length;
    }
    return result;
}
}

ModelFile read_xmd(Bytes bytes) {
    Reader file{bytes}; file.range(0,12);
    ModelFile result;
    const std::size_t root=file.u32(0), texture_root=file.u32(4);
    if (file.u32(8)) fail("Unsupported XMD header flags");
    if (root<12 || root%4 || root>bytes.size()) fail("Invalid XMD model section offset");
    if (texture_root) {
        if (texture_root<12 || texture_root>=root) fail("Invalid XMD texture table offset");
        result.textures=Reader{bytes.first(root)}.names(texture_root);
    }
    const auto count=file.u32(root);
    file.range(root+4,std::size_t(count)*8);
    std::size_t record=root+4;
    for (std::uint32_t i=0; i<count; ++i) {
        const std::size_t size=file.u32(record+4);
        if (size<8 || size%4) fail("Invalid XMD model extent");
        file.range(record,size);
        Reader input{bytes.subspan(record,size)};
        const auto flags=input.u32(0);
        if (flags&~0xffU) fail("Unsupported XMD shape-key/block-bone sections");
        std::array<std::size_t,4> offsets{};
        std::size_t header=8;
        for (std::size_t s=0; s<4; ++s) {
            const auto mode=(flags>>(s*2))&3;
            if (mode==0) continue;
            if (mode!=1) fail("Unsupported XMD shared-section mode");
            header=12+s*4; offsets[s]=input.u32(8+s*4);
        }
        for (auto offset: offsets)
            if (offset && (offset<header || offset%4 || offset>=size)) fail("Invalid XMD section offset");
        auto section=[&](std::size_t index) {
            auto start=offsets[index], end=size;
            if (!start) fail("Required XMD section absent");
            for (std::size_t s=0; s<4; ++s) {
                if (s!=index && offsets[s]==start) fail("Overlapping XMD sections");
                if (offsets[s]>start) end=std::min(end,offsets[s]);
            }
            return Reader{input.bytes.subspan(start,end-start)};
        };
        Model model{}; model.offset=record; model.end=record+size;
        const auto bones=section(0); const auto bone_count=bones.u32(0);
        if (std::size_t(bone_count)*48+4!=bones.bytes.size()) fail("XMD bone section size mismatch");
        for (std::size_t j=0; j<bone_count; ++j)
            model.bones.push_back({bones.floats(4+j*48),bones.floats(20+j*48),bones.floats(36+j*48)});
        const auto hierarchy=section(1); const auto node_count=hierarchy.u32(0);
        if (std::size_t(node_count)*4+4!=hierarchy.bytes.size()) fail("XMD hierarchy section size mismatch");
        for (std::size_t j=0; j<node_count; ++j) {
            HierarchyNode node{hierarchy.u16(4+j*4),hierarchy.i16(6+j*4)};
            if (node.bone>=bone_count || node.parent<-1 || node.parent>=std::int64_t(j))
                fail("Invalid XMD bone/hierarchy reference");
            model.hierarchy.push_back(node);
        }
        if (offsets[3]) model.names=section(3).names(0);
        const auto geometry=section(2); const auto block_count=geometry.u32(0);
        geometry.range(4,std::size_t(block_count)*20);
        std::size_t at=4;
        for (std::size_t j=0; j<block_count; ++j) {
            auto decoded=block(geometry,at,model,result.textures.size());
            const auto base=record+offsets[2];
            decoded.offset+=base; decoded.end+=base; decoded.draw_offset+=base;
            if (decoded.texture_command) decoded.texture_command+=base;
            if (decoded.material_command) decoded.material_command+=base;
            for (auto& cmd: decoded.commands) cmd.offset+=base;
            model.blocks.push_back(std::move(decoded));
        }
        if (at!=geometry.bytes.size()) fail("Unaccounted XMD render block bytes");
        result.models.push_back(std::move(model)); record+=size;
    }
    if (record!=bytes.size()) fail("Unaccounted XMD trailing bytes");
    return result;
}
}
