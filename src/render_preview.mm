#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include "okage/render.hpp"
#include "okage/pose.hpp"
#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include "okage/map.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

namespace fs=std::filesystem;
static std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size=fs::file_size(path);
    if (size>128*1024*1024) throw std::runtime_error("Preview input exceeds size limit");
    std::vector<std::uint8_t> result(size);
    std::ifstream in(path,std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(result.data()),result.size())) throw std::runtime_error("Cannot read preview input");
    return result;
}
static void save(const fs::path& path,NSData* data) {
    if (!data || fs::is_symlink(path)) throw std::runtime_error("Invalid preview output");
    if (fs::exists(path)) {
        const auto old=read(path);
        if (old.size()!=data.length || std::memcmp(old.data(),data.bytes,old.size()))
            throw std::runtime_error("Refusing different existing preview output");
        return;
    }
    std::ofstream out(path,std::ios::binary);
    if (!out.write(static_cast<const char*>(data.bytes),data.length)) throw std::runtime_error("Cannot write preview output");
}
struct Vertex { okage::Float4 position,color,uv; };
struct Draw { std::uint32_t first,count; std::int32_t image; std::array<std::uint32_t,8> state; };
static const char* shader=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position, color, uv; };
struct Varying { float4 position [[position]]; float4 color; float2 uv; };
struct State { uint4 region; uint4 mode; };
vertex Varying preview_vertex(uint id [[vertex_id]], device const Vertex* v [[buffer(0)]]) {
    return {v[id].position, v[id].color, v[id].uv.xy};
}
fragment float4 preview_fragment(Varying v [[stage_in]], texture2d<float> image [[texture(0)]],
                                constant State& state [[buffer(0)]]) {
    float4 color=v.color;
    if (state.mode.z) {
        int2 p=int2(floor(v.uv*1024.0f));
        p.x=state.mode.x==2 ? min(int(state.region.y),max(int(state.region.x),p.x)) :
                             ((p.x & int(state.region.x)) | int(state.region.y));
        p.y=state.mode.y==2 ? min(int(state.region.w),max(int(state.region.z),p.y)) :
                             ((p.y & int(state.region.z)) | int(state.region.w));
        if (any(p<0) || p.x>=int(image.get_width()) || p.y>=int(image.get_height()))
            return float4(1,0,1,1);
        color*=image.read(uint2(p));
    }
    if (color.a<0.5f) discard_fragment();
    return float4(color.rgb,1);
}
)metal";

int main(int argc,const char** argv) {
    @autoreleasepool { try {
        const bool mapMode=argc==5 && std::string(argv[1])=="--map";
        if (!mapMode && argc!=3 && argc!=6)
            throw std::runtime_error("Usage: okage-render-preview XMD OUTPUT.png [XMT ENTRY TICKS] | --map XMP OUTPUT.png REFERENCE_DRAW_PREFIX");
        const fs::path path=argv[mapMode ? 2 : 1];
        const fs::path output=argv[mapMode ? 3 : 2];
        const auto sine=okage::make_sine_table();
        const okage::Matrix4 identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
        struct Input {fs::path path;okage::ModelFile model;okage::Matrix4 world;};
        std::vector<Input> inputs;
        okage::Matrix4 view=identity;std::array<float,32> projection{};
        std::size_t omittedEffects=0;
        if(mapMode) {
            const auto map=okage::read_xmp(read(path));
            const auto v=read(std::string(argv[4])+".camera.bin"),p=read(std::string(argv[4])+".projection.bin");
            if(v.size()!=48 || p.size()!=128)throw std::runtime_error("Invalid captured camera buffers");
            okage::Float4 cameraPosition,cameraRotation,cameraScale;
            std::memcpy(cameraPosition.data(),v.data(),16);std::memcpy(cameraRotation.data(),v.data()+16,16);
            std::memcpy(cameraScale.data(),v.data()+32,16);std::memcpy(projection.data(),p.data(),128);
            view=okage::camera_matrix(cameraPosition,cameraRotation,cameraScale,sine);
            for(float x:projection)if(!std::isfinite(x))throw std::runtime_error("Non-finite captured projection");
            if(projection[3]!=1 || projection[4]!=0 || projection[5]!=0 || projection[20]!=320 || projection[21]!=112)
                throw std::runtime_error("Unsupported captured projection/viewport");
            for(const auto& placement:map.placements) {
                if(placement.handler()) {++omittedEffects;continue;}
                const auto& definition=map.definitions.at(placement.definition());
                if(definition.flags()&8)throw std::runtime_error("Map model animation needs recovered playback");
                const auto file=path.parent_path()/definition.name;
                const auto t=okage::map_transform(placement,definition);
                if(t.flags&1)throw std::runtime_error("Camera-facing map placement unsupported");
                if((t.flags&0x100000) && (t.scale[0]!=1 || t.scale[1]!=1 || t.scale[2]!=1))
                    throw std::runtime_error("Optimized map instance scale path requires further recovery");
                inputs.push_back({file,okage::read_xmd(read(file)),okage::instance_matrix(t.position,t.rotation,t.scale,sine)});
            }
            std::cout<<"Native map geometry: "<<inputs.size()<<" ordinary placements; "<<omittedEffects
                     <<" special-effect placements NOT rendered; actors/events NOT executed\n";
        } else inputs.push_back({path,okage::read_xmd(read(path)),identity});
        if(inputs.empty())throw std::runtime_error("No ordinary geometry in preview");
        auto& model=inputs.front().model;
        auto names=model.textures;
        std::unique_ptr<okage::TextureBindings> bindings;
        std::unique_ptr<okage::Animation> animation;
        if (argc==6) {
            if (model.models.size()!=1) throw std::runtime_error("Animated preview requires one model");
            fs::path xta=argv[3]; xta.replace_extension(".xta");
            if (fs::exists(xta)) {
                const auto bytes=read(xta);
                bindings=std::make_unique<okage::TextureBindings>(model,okage::read_xta(
                    std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size())));
                names=bindings->names();
            }
            const auto bytes=read(argv[3]);
            if (bytes.size()<32) throw std::runtime_error("Truncated animation header");
            const bool scale=bytes[20] || bytes[21] || bytes[22] || bytes[23];
            animation=std::make_unique<okage::Animation>(bytes,4+model.models[0].bones.size()*(scale ? 3 : 2));
            if (bindings) animation->set_texture_callback([&](auto group,auto variant){bindings->select(group,variant);});
            animation->start(0); animation->values(); animation->start(std::stoul(argv[4]));
            const auto ticks=std::stoul(argv[5]);
            if (ticks>10000) throw std::runtime_error("Preview tick limit exceeded");
            for (unsigned i=0;i<ticks;++i) animation->tick();
        }
        std::vector<okage::Texture> textures;
        std::vector<std::size_t> textureOffsets;
        for(const auto& input:inputs) {
            textureOffsets.push_back(textures.size());
            for (auto name:mapMode ? input.model.textures : names) {
                const fs::path relative=name;
                if (relative.is_absolute() || relative.has_parent_path()) throw std::runtime_error("Unsafe model texture path");
                if (relative.extension().empty()) name+=".xim";
                textures.push_back(okage::decode_xim(read(input.path.parent_path()/name)));
            }
        }
        std::vector<Vertex> vertices;
        std::vector<Draw> draws;
        std::size_t textureBase=0, polygons=0;
        okage::Float4 low{INFINITY,INFINITY,INFINITY,0},high{-INFINITY,-INFINITY,-INFINITY,0};
        for(std::size_t inputIndex=0;inputIndex<inputs.size();++inputIndex) {
            const auto& input=inputs[inputIndex];const auto textureOffset=textureOffsets[inputIndex];
            textureBase=0;
            for (const auto& part:input.model.models) {
                auto pose=animation ? okage::animated_pose(part,animation->values(),sine,animation->scale_channels()) : okage::rest_pose(part,sine);
                if(mapMode) {
                    const auto viewWorld=okage::compose_pose(view,std::span(&input.world,1));
                    pose=okage::compose_pose(viewWorld[0],pose);
                }
                for (const auto& block:part.blocks) {
                    const auto positions=okage::posed_positions(block,pose);
                    std::vector<okage::Float4> uv(block.attribute_extents[3]),colors(block.attribute_extents[2]);
                    std::vector<bool> uvDefined(uv.size()),colorDefined(colors.size());
                    for (const auto& attr:block.attributes) if (attr.kind==7 || attr.kind==8) {
                        auto& values=attr.kind==7 ? colors : uv;
                        auto& defined=attr.kind==7 ? colorDefined : uvDefined;
                        for (std::size_t i=0;i<attr.values.size();++i) {
                            if (defined.at(attr.first+i)) throw std::runtime_error("Overlapping preview color/UV ranges");
                            values.at(attr.first+i)=attr.values[i]; defined.at(attr.first+i)=true;
                        }
                    }
                    for (const auto& polygon:block.polygons) {
                        Draw draw{std::uint32_t(vertices.size()),0,-1,{}};
                        const okage::TextureState* state=nullptr;
                        if (polygon.texture_state>=0 && block.textures.at(polygon.texture_state).texture>=0) {
                            state=&block.textures.at(polygon.texture_state);
                            draw.image=textureOffset+(bindings ? bindings->images().at(textureBase+polygon.texture_state) : state->texture);
                            const auto& original=textures.at(textureOffset+state->texture);
                            const auto clamp=okage::texture_clamp(*state,original.width,original.height);
                            draw.state={std::uint32_t(clamp>>4)&1023,std::uint32_t(clamp>>14)&1023,
                                std::uint32_t(clamp>>24)&1023,std::uint32_t(clamp>>34)&1023,
                                std::uint32_t(clamp)&3,std::uint32_t(clamp>>2)&3,1,0};
                        }
                        for (auto triangle:okage::triangle_corners(polygon.corners.size())) for (auto index:triangle) {
                            const auto& corner=polygon.corners.at(index);
                            Vertex vertex{positions.at(corner[0]),{1,1,1,1},{0,0,0,0}};
                            if (corner[2]>=0 && colorDefined.at(corner[2])) vertex.color=colors.at(corner[2]);
                            else throw std::runtime_error("Missing preview source color");
                            if (state) {
                                if (corner[3]<0 || !uvDefined.at(corner[3])) throw std::runtime_error("Missing preview UV");
                                const auto& original=textures.at(textureOffset+state->texture);
                                const auto value=okage::texture_coordinates(*state,original.width,original.height,
                                    {uv.at(corner[3])[0],uv.at(corner[3])[1]});
                                vertex.uv={value[0],value[1],0,0};
                            }
                            for (unsigned axis=0;axis<3;++axis) {
                                if (!std::isfinite(vertex.position[axis])) throw std::runtime_error("Non-finite preview position");
                                low[axis]=std::min(low[axis],vertex.position[axis]); high[axis]=std::max(high[axis],vertex.position[axis]);
                            }
                            vertices.push_back(vertex); ++draw.count;
                        }
                        draws.push_back(draw); ++polygons;
                    }
                    textureBase+=block.textures.size();
                }
            }
        }
        if (vertices.empty()) throw std::runtime_error("Empty preview geometry");
        if(mapMode) {
            for(auto& v:vertices) {
                const auto p=v.position;
                v.position={p[0]*projection[0],-p[1]*projection[1],
                    (p[2]*projection[2]+projection[6]+p[2])*0.5f,p[2]};
            }
        } else {
            double radius=0; okage::Float4 center{};
            for (unsigned i=0;i<3;++i) { center[i]=(high[i]+low[i])*0.5f; radius+=std::pow((high[i]-low[i])*0.5,2); }
            radius=std::max(std::sqrt(radius),.001);
            for (auto& v:vertices) {
                const auto p=v.position;
                const double x=(p[0]-center[0])/radius,y=-(p[1]-center[1])/radius,z=(p[2]-center[2])/radius;
                const double rx=x*std::cos(.45)+z*std::sin(.45),rz=-x*std::sin(.45)+z*std::cos(.45);
                const double ry=y*std::cos(.12)-rz*std::sin(.12),depth=y*std::sin(.12)+rz*std::cos(.12);
                v.position={float(rx*.86),float(ry*.86),float(.5+depth*.25),1};
            }
        }
        const unsigned width=mapMode ? 640 : 1024,height=mapMode ? 224 : 1024;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("No native Metal device");
        NSError* error=nil;
        id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:shader] options:nil error:&error];
        if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"preview_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"preview_fragment"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto depthState=[MTLDepthStencilDescriptor new]; depthState.depthCompareFunction=MTLCompareFunctionLess; depthState.depthWriteEnabled=YES;
        id<MTLDepthStencilState> depth=[device newDepthStencilStateWithDescriptor:depthState];
        auto targetDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        targetDesc.usage=MTLTextureUsageRenderTarget; targetDesc.storageMode=MTLStorageModeShared;
        id<MTLTexture> target=[device newTextureWithDescriptor:targetDesc];
        auto depthDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width height:height mipmapped:NO];
        depthDesc.usage=MTLTextureUsageRenderTarget; depthDesc.storageMode=MTLStorageModePrivate;
        id<MTLTexture> depthTarget=[device newTextureWithDescriptor:depthDesc];
        NSMutableArray<id<MTLTexture>>* gpuTextures=[NSMutableArray array];
        textures.push_back({1,1,0,0,{255,255,255,255}});
        for (const auto& image:textures) {
            auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image.width height:image.height mipmapped:NO];
            td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead;
            id<MTLTexture> texture=[device newTextureWithDescriptor:td];
            if (!texture) throw std::runtime_error("Metal texture allocation failed");
            [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:image.rgba.data() bytesPerRow:image.width*4];
            [gpuTextures addObject:texture];
        }
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=target; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(.055,.07,.095,1);
        pass.depthAttachment.texture=depthTarget; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.clearDepth=1;
        id<MTLCommandQueue> queue=[device newCommandQueue]; id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline]; [encoder setDepthStencilState:depth]; [encoder setCullMode:MTLCullModeNone];
        id<MTLBuffer> vertexBuffer=[device newBufferWithBytes:vertices.data() length:vertices.size()*sizeof(Vertex) options:MTLResourceStorageModeShared];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        for (const auto& draw:draws) {
            [encoder setFragmentTexture:gpuTextures[draw.image<0 ? gpuTextures.count-1 : draw.image] atIndex:0];
            [encoder setFragmentBytes:draw.state.data() length:sizeof(draw.state) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:draw.first vertexCount:draw.count];
        }
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.status!=MTLCommandBufferStatusCompleted) throw std::runtime_error(command.error.localizedDescription.UTF8String);
        auto bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4
            hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width*4 bitsPerPixel:32];
        [target getBytes:bitmap.bitmapData bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        save(output,[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}]);
        std::cout<<"Native Metal unlit preview: "<<polygons<<" polygons, "<<vertices.size()/3<<" triangles, "<<textures.size()-1<<" images on "<<device.name.UTF8String<<"\n";
        std::cout<<(mapMode ? "Captured reference camera; incomplete static room geometry, no native events/effects/audio\n" : "Research camera/shading/filtering/alpha; no retail frame-fidelity claim\n");
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;} }
}
