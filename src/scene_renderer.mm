#import <Metal/Metal.h>
#include "okage/scene_renderer.hpp"
#include <optional>
#include "okage/map.hpp"
#include "okage/field.hpp"
#include "okage/map_objects.hpp"
#include "okage/render.hpp"
#include "metal_preview_shader.hpp"
#include <cmath>
#include <bit>
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
struct Draw {std::uint32_t first,count;std::int32_t image;std::array<std::uint32_t,8> state;bool effect=false,additive=false,dust=false;std::uint64_t materialBlend=0;};
}
struct SceneRenderer::Impl {
    struct Input {fs::path path;ModelFile model;Matrix4 world;const FieldActor* actor=nullptr;std::optional<MapTransform> facing;std::optional<std::size_t> placement;bool visible=true;};
    const MapObjects* mapObjects=nullptr;
    SineTable sine=make_sine_table();
    std::vector<Input> inputs;
    std::optional<std::size_t> sky_input;
    Float4 sky_factors{.5f,0,.5f,0};
    std::vector<Texture> textures;
    std::vector<std::size_t> textureOffsets;
    std::size_t omittedEffects=0,actorCount=0;
    std::size_t whiteTexture=0,effectTexture=0;
    bool effectTexturesLoaded=false;
    const MapEffects* mapEffects=nullptr;
    std::optional<Float4> effectTint;
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline,effectPipeline,mapEffectPipeline,dustPipeline,materialPipeline,materialAddPipeline,materialSubtractPipeline;
    id<MTLDepthStencilState> depth,effectDepth,mapEffectDepth,materialDepth;
    id<MTLTexture> target,depthTarget;
    NSMutableArray<id<MTLTexture>>* gpuTextures;
    id<MTLCommandQueue> queue;
    explicit Impl(const fs::path& path,std::span<const SceneActor> actors,const MapObjects* objects) {
        mapObjects=objects;
        const auto map=read_xmp(read(path));
        for(std::size_t index=0;index<map.placements.size();++index) {
            const auto& placement=map.placements[index];
            if(placement.handler()) {++omittedEffects;continue;}
            const auto& definition=map.definitions.at(placement.definition());
            const auto* object=objects?objects->placement(index):nullptr;
            if((definition.flags()&8)&&!object)throw std::runtime_error("Animated map model requires native object bindings");
            const auto transform=map_transform(placement,definition);
            if((transform.flags&1)&&object)throw std::runtime_error("Animated camera-facing map model requires channel integration");
            if((transform.flags&0x100000) && (transform.scale[0]!=1 || transform.scale[1]!=1 || transform.scale[2]!=1))
                throw std::runtime_error("Optimized map scale unsupported");
            // 0x1b8a80 selects the last eligible0x20000 instance. Foreground
            // optimized0x80 and terrain0x80000 classifications take precedence.
            if(!(transform.flags&0x80080)&&(transform.flags&0x20000))sky_input=inputs.size();
            const auto file=path.parent_path()/definition.name;
            inputs.push_back({file,object?object->model():read_xmd(read(file)),instance_matrix(transform.position,transform.rotation,transform.scale,sine),object,
                (transform.flags&1)?std::optional<MapTransform>(transform):std::nullopt,index,(transform.flags&0x10000)==0});
        }
        if(inputs.empty())throw std::runtime_error("Empty field map");
        if(sky_input){auto stage=path;stage.replace_extension(".stg");sky_factors=field_sky_follow(read(stage));}
        for(const auto& binding:actors) {
            if(!binding.actor || binding.model_path.empty())throw std::runtime_error("Actor and model path required");
            inputs.push_back({binding.model_path,binding.actor->model(),{},binding.actor,{},{},true});++actorCount;
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
        descriptor.fragmentFunction=[library newFunctionWithName:@"effect_fragment"];
        descriptor.colorAttachments[0].blendingEnabled=YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        effectPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!effectPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.vertexFunction=[library newFunctionWithName:@"map_effect_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"map_effect_fragment"];
        descriptor.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOne;
        mapEffectPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!mapEffectPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorZero;
        dustPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!dustPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.vertexFunction=[library newFunctionWithName:@"preview_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"material_fragment"];
        descriptor.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorZero;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOne;
        materialPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!materialPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOne;
        materialAddPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!materialAddPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].rgbBlendOperation=MTLBlendOperationReverseSubtract;
        materialSubtractPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!materialSubtractPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto depthState=[MTLDepthStencilDescriptor new]; depthState.depthCompareFunction=MTLCompareFunctionLess; depthState.depthWriteEnabled=YES;
        depth=[device newDepthStencilStateWithDescriptor:depthState];
        depthState.depthCompareFunction=MTLCompareFunctionLessEqual;
        materialDepth=[device newDepthStencilStateWithDescriptor:depthState];
        depthState.depthCompareFunction=MTLCompareFunctionAlways;depthState.depthWriteEnabled=NO;
        effectDepth=[device newDepthStencilStateWithDescriptor:depthState];
        depthState.depthCompareFunction=MTLCompareFunctionLessEqual;
        mapEffectDepth=[device newDepthStencilStateWithDescriptor:depthState];
        auto targetDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        targetDesc.usage=MTLTextureUsageRenderTarget; targetDesc.storageMode=MTLStorageModeShared;
        target=[device newTextureWithDescriptor:targetDesc];
        auto depthDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width height:height mipmapped:NO];
        depthDesc.usage=MTLTextureUsageRenderTarget; depthDesc.storageMode=MTLStorageModePrivate;
        depthTarget=[device newTextureWithDescriptor:depthDesc];
        gpuTextures=[NSMutableArray array];
        whiteTexture=textures.size();textures.push_back({1,1,0,0,{255,255,255,255}});
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
    void loadEffects(const fs::path& directory) {
        if(effectTexturesLoaded)throw std::runtime_error("Effect textures already loaded");
        effectTexture=textures.size();
        for(const auto name:effect_texture_names) {
            auto image=decode_xim(read(directory/name));
            auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image.width height:image.height mipmapped:NO];
            td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
            id<MTLTexture> texture=[device newTextureWithDescriptor:td];
            if(!texture)throw std::runtime_error("Effect texture allocation failed");
            [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:image.rgba.data() bytesPerRow:image.width*4];
            [gpuTextures addObject:texture];textures.push_back(std::move(image));
        }
        effectTexturesLoaded=true;
    }
    Texture render(const Matrix4& view,const Float4& projection,const AttachedEffects* effects,const Float4& camera,float farClip,const Float4& cameraPosition) {
        std::vector<Vertex> vertices;
        std::vector<Draw> draws;
        okage::Float4 low{INFINITY,INFINITY,INFINITY,0},high{-INFINITY,-INFINITY,-INFINITY,0};
        for(std::size_t inputIndex=0;inputIndex<inputs.size();++inputIndex) {
            const auto& input=inputs[inputIndex];const auto textureOffset=textureOffsets[inputIndex];
            if(input.placement && !(mapObjects?mapObjects->visible(*input.placement):input.visible))continue;
            if(input.actor && !input.actor->visible)continue;
            const bool materialActor=input.actor && input.actor->encounter_model;
            float instanceAlpha=1;
            if(input.actor && input.actor->render_flags) {
                if(!materialActor || input.actor->render_flags!=0x100000)
                    throw std::runtime_error("Actor instance render mode is unrecovered");
                const auto& ambient=input.actor->channels().at(3);
                if(ambient[0]!=1 || ambient[1]!=1 || ambient[2]!=1)
                    throw std::runtime_error("Actor instance ambient RGB is unrecovered");
                instanceAlpha=ambient[3];
            }
            std::vector<Draw> translucent;
            std::size_t textureBase=0;
            auto world=input.actor ? input.actor->world(sine) : input.world;
            if(input.facing)world=map_instance_matrix(*input.facing,{-camera[0],-camera[1],0,1},sine);
            if(input.placement&&mapObjects&&!input.actor)world[3]=mapObjects->position(*input.placement);
            if(sky_input&&inputIndex==*sky_input){
                const auto position=sky_follow_position(cameraPosition,sky_factors);
                world[3]={position[0],position[1],position[2],1};
            }
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
                        const Material* material=nullptr;
                        if(materialActor) {
                            if(polygon.material<0)throw std::runtime_error("Special actor has no material");
                            material=&block.materials.at(polygon.material);
                            if(!(__builtin_bit_cast(float, (material->words[5]))>0) || material->words[7])
                                throw std::runtime_error("Unrecovered special-actor material class");
                            draw.materialBlend=material_blend_register(*material);
                        }
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
                            if(material) {
                                vertex.color=material_source_color(*material,vertex.color);
                                vertex.color[3]=material_instance_alpha(vertex.color[3],instanceAlpha);
                            }
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
                        if(material) {
                            draw.state[7]=1;draws.push_back(draw);
                            draw.state[7]=2;translucent.push_back(draw);
                        } else draws.push_back(draw);
                    }
                    textureBase+=block.textures.size();
                }
            }
            draws.insert(draws.end(),translucent.begin(),translucent.end());
        }
        for(unsigned slot=0;slot<84;++slot){
        if(mapEffects&&(*mapEffects)[slot]&&((*mapEffects)[slot]->id!=17||((*mapEffects)[slot]->flags&0x20))) {
            const auto& effect=(*mapEffects)[slot];
            if(!effectTexturesLoaded)throw std::runtime_error("Map effect textures are unavailable");
            for(const auto& e:map_effect_draws(*effect,view,camera,farClip,sine,effectTint?&*effectTint:nullptr)){
                const bool untextured=e.texture==0xffffffffu;
                const auto texture=untextured?-1:std::int32_t(effectTexture+e.texture);
                if(!untextured){const auto& image=textures.at(texture);
                    if(e.uv[2]>=image.width||e.uv[3]>=image.height)throw std::runtime_error("Map effect texture bounds");}
                Draw draw{std::uint32_t(vertices.size()),std::uint32_t(e.vertices.size()),std::int32_t(texture),
                    {e.uv[0],e.uv[2],e.uv[1],e.uv[3],2,2,untextured?0u:1u,e.additive?0u:1u},true,e.additive,!e.additive};
                for(const auto& source:e.vertices){
                    Float4 p{},c{};
                    for(unsigned r=0;r<4;++r){
                        p[r]=e.matrix[0][r]*source.position[0]+e.matrix[1][r]*source.position[1]+
                             e.matrix[2][r]*source.position[2]+e.matrix[3][r];
                        c[r]=std::clamp(std::trunc(source.color[r]*e.color[r]),0.0f,255.0f);
                    }
                    vertices.push_back({p,c,source.uv});
                }
                draws.push_back(draw);
            }
        }
        if(effects&&(*effects)[slot]&&!((*effects)[slot]->animation.flags&0x300)) {
            const auto& attached=(*effects)[slot];
            if(!effectTexturesLoaded)throw std::runtime_error("Effect textures are unavailable");
            for(const auto& sprite:attached->animation.sprites) {
                auto origin=attached->position;for(unsigned i=0;i<3;++i)origin[i]+=sprite.position[i];
                const auto world=instance_matrix(origin,{1-camera[0],1-camera[1],sprite.rotation[2],0},sprite.scale,sine);
                const auto matrix=compose_pose(view,std::span(&world,1))[0];
                const std::array<Float4,4> corners={Float4{0,-1,0,1},Float4{1,-1,0,1},Float4{1,0,0,1},Float4{0,0,0,1}};
                if(sprite.texture<14||sprite.texture>16)throw std::runtime_error("Unsupported effect texture");
                const auto texture=effectTexture+sprite.texture;const auto& image=textures.at(texture);
                const unsigned left=sprite.uv[0],top=sprite.uv[1],right=sprite.uv[2],bottom=sprite.uv[3];
                Draw draw{std::uint32_t(vertices.size()),6,std::int32_t(texture),{left,std::min(right,image.width-1),top,std::min(bottom,image.height-1),2,2,1,0},true};
                const float u0=left/1024.f,v0=top/1024.f;
                const float u1=(sprite.texture==14?32.f:float(left+16))/1024.f;
                const float v1=(sprite.texture==14?32.f:16.f)/1024.f;
                const std::array<Float4,4> uv={Float4{u0,v0,0,0},Float4{u1,v0,0,0},Float4{u1,v1,0,0},Float4{u0,v1,0,0}};
                for(unsigned c:{0u,1u,2u,0u,2u,3u}) {
                    Float4 p{};for(unsigned r=0;r<4;++r)p[r]=matrix[0][r]*corners[c][0]+matrix[1][r]*corners[c][1]+matrix[2][r]*corners[c][2]+matrix[3][r];
                    vertices.push_back({p,sprite.color,uv[c]});
                }
                draws.push_back(draw);
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
            [encoder setCullMode:draw.materialBlend?MTLCullModeBack:MTLCullModeNone];
            [encoder setRenderPipelineState:draw.materialBlend?(draw.materialBlend==0x48?materialAddPipeline:draw.materialBlend==0x42?materialSubtractPipeline:materialPipeline):draw.dust?dustPipeline:draw.additive?mapEffectPipeline:draw.effect?effectPipeline:pipeline];
            [encoder setDepthStencilState:draw.materialBlend?(draw.state[7]==1?materialDepth:mapEffectDepth):(draw.additive||draw.dust)?mapEffectDepth:draw.effect?effectDepth:depth];
            [encoder setFragmentTexture:gpuTextures[draw.image<0 ? whiteTexture : draw.image] atIndex:0];
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
SceneRenderer::SceneRenderer(const fs::path& map,std::span<const SceneActor> actors,const MapObjects* objects):
    impl_(std::make_unique<Impl>(map,actors,objects)){}
SceneRenderer::~SceneRenderer()=default;
void SceneRenderer::load_effect_textures(const fs::path& path){impl_->loadEffects(path);}
EffectTextureSizes SceneRenderer::effect_texture_sizes() const {
    if(!impl_->effectTexturesLoaded)throw std::runtime_error("Effect textures are unavailable");
    EffectTextureSizes result{};
    for(unsigned i=0;i<result.size();++i){const auto& t=impl_->textures.at(impl_->effectTexture+i);result[i]={t.width,t.height};}
    return result;
}
void SceneRenderer::set_effect_light_tint(const Float4& tint){impl_->effectTint=tint;}
void SceneRenderer::bind_map_effects(const MapEffects* effects){impl_->mapEffects=effects;}
Texture SceneRenderer::render(const Matrix4& view,const Float4& projection,const AttachedEffects* effects,const Float4& camera,float farClip,const Float4& cameraPosition){return impl_->render(view,projection,effects,camera,farClip,cameraPosition);}
std::size_t SceneRenderer::placements() const{return impl_->inputs.size()-impl_->actorCount;}
std::size_t SceneRenderer::actors() const{return impl_->actorCount;}
std::size_t SceneRenderer::omitted_effects() const{return impl_->omittedEffects;}
}
