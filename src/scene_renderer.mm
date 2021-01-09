#import <Metal/Metal.h>
#include "okage/scene_renderer.hpp"
#include "okage/map.hpp"
#include "okage/render.hpp"
#include "metal_preview_shader.hpp"
#include <cmath>
#include <fstream>
namespace okage {
namespace fs=std::filesystem;
namespace {
std::vector<std::uint8_t> read(const fs::path& path) {
    const auto n=fs::file_size(path);if(n>128*1024*1024)throw std::runtime_error("Scene input limit");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read scene input");return b;
}
struct Vertex {Float4 position,color,uv;};
struct Draw {std::uint32_t first,count;std::int32_t image;std::array<std::uint32_t,8> state;};
}
struct SceneRenderer::Impl {
    struct Input {fs::path path;ModelFile model;Matrix4 world;const FieldActor* actor=nullptr;};
    SineTable sine=make_sine_table();
    std::vector<Input> inputs;
    std::vector<Texture> textures;
    std::vector<std::size_t> textureOffsets;
    std::size_t omittedEffects=0,actorCount=0;
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth;
    id<MTLTexture> target,depthTarget;
    NSMutableArray<id<MTLTexture>>* gpuTextures;
    id<MTLCommandQueue> queue;
    explicit Impl(const fs::path& path,std::span<const SceneActor> actors) {
        const auto map=read_xmp(read(path));
        for(const auto& placement:map.placements) {
            if(placement.handler()) {++omittedEffects;continue;}
            const auto& definition=map.definitions.at(placement.definition());
            if(definition.flags()&8)throw std::runtime_error("Animated map model unsupported");
            const auto transform=map_transform(placement,definition);
            if(transform.flags&1)throw std::runtime_error("Camera-facing map model unsupported");
            if((transform.flags&0x100000) && (transform.scale[0]!=1 || transform.scale[1]!=1 || transform.scale[2]!=1))
                throw std::runtime_error("Optimized map scale unsupported");
            const auto file=path.parent_path()/definition.name;
            inputs.push_back({file,read_xmd(read(file)),instance_matrix(transform.position,transform.rotation,transform.scale,sine)});
        }
        if(inputs.empty())throw std::runtime_error("Empty field map");
        for(const auto& binding:actors) {
            if(!binding.actor || binding.model_path.empty())throw std::runtime_error("Actor and model path required");
            inputs.push_back({binding.model_path,binding.actor->model(),{},binding.actor});++actorCount;
        }
        for(const auto& input:inputs) {
            textureOffsets.push_back(textures.size());
            for(auto name:input.actor ? input.actor->textures().names() : input.model.textures) {
                const fs::path relative=name;
                if(relative.is_absolute() || relative.has_parent_path())throw std::runtime_error("Unsafe scene texture path");
                if(relative.extension().empty())name+=".xim";
                textures.push_back(decode_xim(read(input.path.parent_path()/name)));
            }
        }
        constexpr unsigned width=640,height=224;
        device=MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("No native Metal device");
        NSError* error=nil;
        id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:shader] options:nil error:&error];
        if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"preview_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"preview_fragment"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto depthState=[MTLDepthStencilDescriptor new]; depthState.depthCompareFunction=MTLCompareFunctionLess; depthState.depthWriteEnabled=YES;
        depth=[device newDepthStencilStateWithDescriptor:depthState];
        auto targetDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        targetDesc.usage=MTLTextureUsageRenderTarget; targetDesc.storageMode=MTLStorageModeShared;
        target=[device newTextureWithDescriptor:targetDesc];
        auto depthDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width height:height mipmapped:NO];
        depthDesc.usage=MTLTextureUsageRenderTarget; depthDesc.storageMode=MTLStorageModePrivate;
        depthTarget=[device newTextureWithDescriptor:depthDesc];
        gpuTextures=[NSMutableArray array];
        textures.push_back({1,1,0,0,{255,255,255,255}});
        for (const auto& image:textures) {
            auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image.width height:image.height mipmapped:NO];
            td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead;
            id<MTLTexture> texture=[device newTextureWithDescriptor:td];
            if (!texture) throw std::runtime_error("Metal texture allocation failed");
            [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:image.rgba.data() bytesPerRow:image.width*4];
            [gpuTextures addObject:texture];
        }
        queue=[device newCommandQueue];
    }
    Texture render(const Matrix4& view,const Float4& projection) {
        std::vector<Vertex> vertices;
        std::vector<Draw> draws;
        okage::Float4 low{INFINITY,INFINITY,INFINITY,0},high{-INFINITY,-INFINITY,-INFINITY,0};
        for(std::size_t inputIndex=0;inputIndex<inputs.size();++inputIndex) {
            const auto& input=inputs[inputIndex];const auto textureOffset=textureOffsets[inputIndex];
            if(input.actor && !input.actor->visible)continue;
            std::size_t textureBase=0;
            const auto world=input.actor ? input.actor->world(sine) : input.world;
            for (const auto& part:input.model.models) {
                auto pose=input.actor ? okage::animated_pose(part,input.actor->channels(),sine,input.actor->animation().scale_channels()) : okage::rest_pose(part,sine);
                {
                    const auto viewWorld=okage::compose_pose(view,std::span(&world,1));
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
                            draw.image=textureOffset+(input.actor ? input.actor->textures().images().at(textureBase+polygon.texture_state) : state->texture);
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
                        draws.push_back(draw);
                    }
                    textureBase+=block.textures.size();
                }
            }
        }
        if (vertices.empty()) throw std::runtime_error("Empty preview geometry");
        for(auto& v:vertices) {
            const auto p=v.position;
            v.position={p[0]*projection[0],-p[1]*projection[1],(p[2]*projection[2]+projection[3]+p[2])*0.5f,p[2]};
        }
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=target; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(.055,.07,.095,1);
        pass.depthAttachment.texture=depthTarget; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.clearDepth=1;
        id<MTLCommandBuffer> command=[queue commandBuffer];
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
        Texture image{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
        [target getBytes:image.rgba.data() bytesPerRow:640*4 fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];
        return image;
    }
};
SceneRenderer::SceneRenderer(const fs::path& map,std::span<const SceneActor> actors):impl_(std::make_unique<Impl>(map,actors)){}
SceneRenderer::~SceneRenderer()=default;
Texture SceneRenderer::render(const Matrix4& view,const Float4& projection){return impl_->render(view,projection);}
std::size_t SceneRenderer::placements() const{return impl_->inputs.size()-impl_->actorCount;}
std::size_t SceneRenderer::actors() const{return impl_->actorCount;}
std::size_t SceneRenderer::omitted_effects() const{return impl_->omittedEffects;}
}
