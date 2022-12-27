#include "okage/ui_draw_diagnostic.hpp"

#include <bit>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace okage {
namespace {
constexpr std::size_t max_commands=200000, max_packet_operations=200000,
                      max_float_vectors=2000000;
std::string quote(std::string_view value) {
    std::string out="\"";
    for(unsigned char c:value) {
        switch(c) {
        case '"': out+="\\\""; break;
        case '\\': out+="\\\\"; break;
        case '\b': out+="\\b"; break;
        case '\f': out+="\\f"; break;
        case '\n': out+="\\n"; break;
        case '\r': out+="\\r"; break;
        case '\t': out+="\\t"; break;
        default:
            if(c<0x20||c>=0x7f) {
                constexpr char digits[]="0123456789abcdef";
                out+="\\u00";out+=digits[c>>4];out+=digits[c&15];
            } else out+=static_cast<char>(c);
        }
    }
    out+='"';return out;
}
std::string hex32(std::uint32_t value) {
    std::ostringstream out;out<<std::hex<<std::setw(8)<<std::setfill('0')<<value;return out.str();
}
std::string hex64(std::uint64_t value) {
    std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return out.str();
}
const char* control(UiDrawControl value) {
    switch(value) {
    case UiDrawControl::Begin:return "begin";
    case UiDrawControl::CullModeZero:return "cull_mode_zero";
    case UiDrawControl::BeginGif:return "begin_gif";
    case UiDrawControl::EndGif:return "end_gif";
    case UiDrawControl::FlushVu:return "flush_vu";
    case UiDrawControl::End:return "end";
    case UiDrawControl::Queue:return "queue";
    case UiDrawControl::QueueTextState:return "queue_text_state";
    }
    throw std::runtime_error("unknown UI draw control");
}
const char* family(UiDrawPacketFamily value) {
    switch(value) {
    case UiDrawPacketFamily::Glyph:return "glyph";
    case UiDrawPacketFamily::SpriteHeader:return "sprite_header";
    case UiDrawPacketFamily::SpriteElement:return "sprite_element";
    }
    throw std::runtime_error("unknown UI draw packet family");
}
const char* operation(UiPacketOperationKind value) {
    switch(value) {
    case UiPacketOperationKind::Positions:return "positions";
    case UiPacketOperationKind::Colors:return "colors";
    case UiPacketOperationKind::UV:return "uv";
    case UiPacketOperationKind::Register:return "register";
    case UiPacketOperationKind::Texture:return "texture";
    case UiPacketOperationKind::Primitive:return "primitive";
    case UiPacketOperationKind::IndexedVertex:return "indexed_vertex";
    case UiPacketOperationKind::CompilerBoundary:return "compiler_boundary";
    case UiPacketOperationKind::AuxiliaryVectors:return "auxiliary_vectors";
    }
    throw std::runtime_error("unknown UI packet operation");
}
void bits(std::ostream& out,const Float4& value) {
    out<<'[';
    for(unsigned lane=0;lane<4;++lane) {
        if(lane)out<<',';
        out<<quote(hex32(__builtin_bit_cast(std::uint32_t, (value[lane]))));
    }
    out<<']';
}
void packet_operations(std::ostream& out,const std::vector<UiPacketOperation>& ops,
                       std::size_t& total,std::size_t& vectors) {
    if(ops.size()>max_packet_operations-total)throw std::runtime_error("UI diagnostic packet operation limit");
    total+=ops.size();out<<'[';
    for(std::size_t i=0;i<ops.size();++i) {
        if(i)out<<',';const auto& op=ops[i];
        if(op.values.size()>max_float_vectors-vectors)throw std::runtime_error("UI diagnostic float vector limit");
        vectors+=op.values.size();
        out<<"{\"kind\":"<<quote(operation(op.kind))<<",\"source_helper\":"<<quote(hex32(op.source_helper))
           <<",\"arguments_u64\":[";
        for(unsigned n=0;n<op.arguments.size();++n){if(n)out<<',';out<<quote(hex64(op.arguments[n]));}
        out<<"],\"values_f32_bits\":[";
        for(std::size_t n=0;n<op.values.size();++n){if(n)out<<',';bits(out,op.values[n]);}
        out<<"],\"first_positions_upload\":";
        if(op.first_positions_upload)out<<(*op.first_positions_upload?"true":"false");else out<<"null";
        out<<'}';
    }
    out<<']';
}
void packet(std::ostream& out,const UiNativePacket& p,std::size_t& total,std::size_t& vectors) {
    out<<"{\"operations\":";packet_operations(out,p.operations,total,vectors);out<<",\"elements\":[";
    for(std::size_t i=0;i<p.elements.size();++i) {
        if(i)out<<',';
        if(p.elements[i])packet_operations(out,*p.elements[i],total,vectors);else out<<"null";
    }
    out<<"]}";
}
}

void write_ui_draw_diagnostic(std::ostream& out,const UiDrawList& list,
                              std::uint32_t frame,std::uint32_t input_anchor,
                              std::uint32_t reference_vblank,std::string_view execution) {
    if(execution.empty())throw std::runtime_error("UI diagnostic execution label is required");
    if(execution.size()>128)throw std::runtime_error("UI diagnostic execution label exceeds bound");
    const auto commands=list.commands();
    if(commands.empty()||commands.size()>max_commands)throw std::runtime_error("UI diagnostic command count out of bounds");
    std::ostringstream json;
    json<<"{\"schema\":\"okage-native-ui-draw-list-v1\","
       <<"\"scope\":\"ordered native UiDrawList snapshot; no GPU submission, GS execution, vertices, or raster equivalence\","
       <<"\"frame\":"<<frame<<",\"input_anchor\":"<<input_anchor
       <<",\"reference_vblank\":"<<reference_vblank<<",\"execution\":"<<quote(execution)
       <<",\"command_count\":"<<commands.size()<<",\"commands\":[";
    std::size_t packetOperationsCount=0,floatVectors=0;
    for(std::size_t i=0;i<commands.size();++i) {
        if(i)json<<',';const auto& command=commands[i];json<<"{\"index\":"<<i;
        if(const auto* value=std::get_if<UiDrawControl>(&command)) {
            json<<",\"kind\":\"control\",\"name\":"<<quote(control(*value));
        } else if(const auto* value=std::get_if<UiDrawRegister>(&command)) {
            json<<",\"kind\":\"gs_register\",\"register\":"<<value->index
               <<",\"value_u64\":"<<quote(hex64(value->value));
        } else if(std::holds_alternative<UiDrawCurrentFramebuffer>(command)) {
            json<<",\"kind\":\"current_framebuffer\",\"target\":\"runtime_current\"";
        } else if(const auto* value=std::get_if<UiDrawProjection>(&command)) {
            json<<",\"kind\":\"projection\",\"float_bits\":[";
            bool comma=false;for(const auto& vector:value->value)for(float lane:vector){if(comma)json<<',';comma=true;json<<quote(hex32(__builtin_bit_cast(std::uint32_t, (lane))));}
            json<<']';
        } else if(const auto* value=std::get_if<UiDrawMatrix>(&command)) {
            json<<",\"kind\":\"matrix\",\"float_bits\":[";
            bool comma=false;for(const auto& vector:value->value)for(float lane:vector){if(comma)json<<',';comma=true;json<<quote(hex32(__builtin_bit_cast(std::uint32_t, (lane))));}
            json<<']';
        } else if(const auto* value=std::get_if<UiDrawVectors>(&command)) {
            if(value->values.size()>max_float_vectors-floatVectors)throw std::runtime_error("UI diagnostic float vector limit");
            floatVectors+=value->values.size();
            json<<",\"kind\":\"vectors\",\"vector_kind\":"
               <<quote(value->kind==UiDrawVectors::Kind::Auxiliary?"auxiliary":"colors")
               <<",\"first\":"<<value->first<<",\"values_f32_bits\":[";
            for(std::size_t n=0;n<value->values.size();++n){if(n)json<<',';bits(json,value->values[n]);}
            json<<']';
        } else if(const auto* value=std::get_if<UiDrawReplay>(&command)) {
            if(!value->packet)throw std::runtime_error("UI diagnostic replay has no packet owner");
            json<<",\"kind\":\"replay\",\"owner\":{\"family\":"<<quote(family(value->family))
               <<",\"window\":"<<value->window<<",\"group\":"<<value->group
               <<",\"side\":"<<value->side<<",\"element\":"<<value->element
               <<",\"history\":"<<value->history<<",\"pass\":"<<value->pass
               <<"},\"packet\":";
            packet(json,*value->packet,packetOperationsCount,floatVectors);
        } else throw std::runtime_error("unknown UI draw command alternative");
        json<<'}';
    }
    json<<"],\"packet_operation_count\":"<<packetOperationsCount
       <<",\"unavailable\":[\"deferred GS packet execution\",\"DMA packet bytes\",\"VU vertices\",\"GPU/raster output\"]}\n";
    const auto encoded=json.str();
    if(encoded.size()>64*1024*1024)throw std::runtime_error("UI diagnostic output byte limit");
    out<<encoded;
    if(!out)throw std::runtime_error("UI diagnostic output failure");
}
}
