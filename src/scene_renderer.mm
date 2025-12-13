#import <Metal/Metal.h>
#include "okage/input_log.hpp"
#include "okage/scene_renderer.hpp"
#include <optional>
#include "okage/map.hpp"
#include "okage/field.hpp"
#include "okage/map_objects.hpp"
#include "okage/render.hpp"
#include "okage/ordinary_lighting.hpp"
#include "okage/map_material.hpp"
#include "okage/field_raster.hpp"
#include "shadow_metal.hpp"
#include "ui_metal.hpp"
#include "metal_preview_shader.hpp"
#include "gs_texture_sampling_metal.hpp"
#include "gs_material_shader.hpp"
#include "okage/gs_texture_sampling.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <bit>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <iomanip>
#include <sstream>
namespace okage {
namespace fs=std::filesystem;
namespace {
std::vector<std::uint8_t> read(const fs::path& path) {
    const auto n=fs::file_size(path);if(n>128*1024*1024)throw std::runtime_error("Scene input limit");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read scene input");
    okage::record_input_read(path,b);return b;
}
struct SceneBackend {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline,effectPipeline,mapEffectPipeline,dustPipeline,darkEffectPipeline,materialPipeline,materialAddPipeline,materialSubtractPipeline,environmentPipeline;
    id<MTLRenderPipelineState> gsMaterialPipeline,gsMaterialAddPipeline,gsMaterialSubtractPipeline;
    id<MTLDepthStencilState> depth,effectDepth,mapEffectDepth,materialDepth;
    id<MTLLibrary> library;
    explicit SceneBackend(id<MTLDevice> value):device(value) {
        NSError* error=nil;
        const auto source=std::string(gs_texture_sampling_metal_source)+shader+gs_material_shader;
        auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"field_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_preview_fragment"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_effect_fragment"];
        descriptor.colorAttachments[0].blendingEnabled=YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        effectPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!effectPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_map_effect_fragment"];
        descriptor.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOne;
        mapEffectPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!mapEffectPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorZero;
        dustPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!dustPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        // Effect6 ALPHA0x42: Cd-Cs*As/128, with alpha preserved on this
        // composited target. Reuse the effect shader's unbounded premultiply.
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].rgbBlendOperation=MTLBlendOperationReverseSubtract;
        darkEffectPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!darkEffectPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].rgbBlendOperation=MTLBlendOperationAdd;
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_material_fragment"];
        descriptor.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorZero;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOne;
        materialPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!materialPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_environment_fragment"];
        environmentPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!environmentPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_material_fragment"];
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOne;
        materialAddPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!materialAddPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].rgbBlendOperation=MTLBlendOperationReverseSubtract;
        materialSubtractPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!materialSubtractPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.fragmentFunction=[library newFunctionWithName:@"field_gs_material_fragment"];
        gsMaterialSubtractPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!gsMaterialSubtractPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].rgbBlendOperation=MTLBlendOperationAdd;
        gsMaterialAddPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!gsMaterialAddPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        descriptor.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        gsMaterialPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!gsMaterialPipeline)throw std::runtime_error(error.localizedDescription.UTF8String);
        auto depthState=[MTLDepthStencilDescriptor new]; depthState.depthCompareFunction=MTLCompareFunctionLessEqual; depthState.depthWriteEnabled=YES;
        depth=[device newDepthStencilStateWithDescriptor:depthState];
        depthState.depthCompareFunction=MTLCompareFunctionLessEqual;
        materialDepth=[device newDepthStencilStateWithDescriptor:depthState];
        depthState.depthCompareFunction=MTLCompareFunctionAlways;depthState.depthWriteEnabled=NO;
        effectDepth=[device newDepthStencilStateWithDescriptor:depthState];
        depthState.depthCompareFunction=MTLCompareFunctionLessEqual;
        mapEffectDepth=[device newDepthStencilStateWithDescriptor:depthState];
    }
};
struct SceneBackendCache {
    std::mutex mutex;
    std::vector<std::shared_ptr<const SceneBackend>> entries;
};
SceneBackendCache& backendCache() {
    static SceneBackendCache cache;
    return cache;
}
std::shared_ptr<const SceneBackend> acquireBackend(id<MTLDevice> device) {
    if(!device)throw std::runtime_error("No native Metal device");
    auto& cache=backendCache();
    std::lock_guard lock(cache.mutex);
    for(const auto& entry:cache.entries)if(entry->device==device)return entry;
    auto entry=std::make_shared<const SceneBackend>(device);
    cache.entries.push_back(entry);
    return entry;
}
using Vertex=FieldRasterVertex;
struct Draw {std::uint32_t first,count;std::int32_t image;std::array<std::uint32_t,8> state;bool effect=false,additive=false,dust=false;std::uint64_t materialBlend=0;bool depthAlways=false,subtractive=false;unsigned phase=0;float nearClip=3;bool deferred=false;unsigned bucket=0,ordinal=0;bool mapMaterial=false,unculledFieldActor=false;
    std::optional<GsTextureRegisters> gs_texture;int source_handle=-1;};
struct SceneGsMemory {
    Ps2TextureSnapshot snapshot;
    id<MTLBuffer> bytes,known;
};
}
struct SceneRenderer::PreparedFrame::Data {
    std::shared_ptr<const int> renderer;
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;
    FieldShadowFrame shadows;
    ShadowRasterMapping shadow_mapping;
    std::array<std::uint8_t,3> clear;
    NSArray<id<MTLTexture>>* textures;
    std::size_t white_texture;
    bool field_projection,restore_shadow_alpha;
    std::shared_ptr<const SceneGsMemory> gs;
};
struct SceneRenderer::SubmittedFrame::State {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> command;
    id<MTLTexture> color,depth;
    id<MTLBuffer> vertices;
    std::shared_ptr<const SceneBackend> backend;
    std::shared_ptr<ShadowMetal> shadow_metal;
    std::shared_ptr<UiMetal> ui_metal;
    std::shared_ptr<const void> prepared_owner;
    std::shared_ptr<std::vector<std::shared_ptr<const void>>> ui_owners;
    std::vector<std::function<void()>> validators;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    mutable bool validation_in_progress=false;
    mutable std::optional<Texture> image;
    mutable std::exception_ptr failure;
};
struct SceneRenderer::Impl {
    struct BlockPreparation {
        std::vector<std::optional<PreparedTextureState>> textures;
        std::vector<std::optional<PolygonTextureCoordinates>> coordinates;
        std::vector<std::array<PreparedMapBaseColor,4>> mapBaseColors;
        std::vector<std::optional<std::uint64_t>> originalTex0;
    };
    struct PartPreparation {
        std::vector<BlockPreparation> blocks;
        std::vector<std::vector<ModelBlockBound>> fieldBounds;
        std::vector<std::size_t> textureBases;
    };
    struct Input {fs::path path;ModelFile model;Matrix4 world;const FieldActor* actor=nullptr;std::optional<MapTransform> facing;std::optional<std::size_t> placement;bool visible=true;bool afterShadows=false;std::uint32_t flags=0;unsigned ordinal=0;bool special=false;std::optional<SceneActorDescriptor> descriptor{};
        std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> textureBytes{};std::vector<ResourceTextureSlot> textureSlots{};bool texturesOwned=false;};
    const MapObjects* mapObjects=nullptr;
    SineTable sine=make_sine_table();
    std::vector<Input> inputs;
    std::vector<std::vector<PartPreparation>> preparation;
    std::vector<FieldShadowActor> shadowActors;
    const TextureHandleOwner* sharedTextures=nullptr;
    std::optional<std::uint64_t> environmentTex0;
    int environmentHandle=-1;
    std::shared_ptr<const SceneGsMemory> cachedGs;
    std::string textureSamplingObservation="{\"shared_gs\":false}";
    FieldShadowSettings shadowSettings;
    FieldOrdinaryLighting ordinaryLighting;
    FieldShadowFrame shadowFrame;
    std::shared_ptr<ShadowMetal> shadowMetal;
    std::shared_ptr<UiMetal> uiMetal;
    std::optional<std::size_t> sky_input;
    Float4 sky_factors{.5f,0,.5f,0};
    std::vector<Texture> textures;
    std::vector<std::size_t> textureOffsets;
    std::optional<std::size_t> materialEnvironment;
    Float4 materialEnvironmentRegion{};
    std::size_t omittedEffects=0,actorCount=0;
    unsigned nextFrontOrdinal=0;
    unsigned sortDepthShift=19;
    const std::shared_ptr<const int> identity=std::make_shared<const int>(0);
    std::size_t whiteTexture=0;
    std::vector<std::size_t> effectTextureSlots;
    bool effectTexturesLoaded=false;
    const MapEffects* mapEffects=nullptr;
    const EffectSubmissions* effectSubmissions=nullptr;
    std::optional<Float4> effectTint;
    std::array<std::uint8_t,3> clearRGB{64,64,64};
    std::shared_ptr<const SceneBackend> backend;
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline,effectPipeline,mapEffectPipeline,dustPipeline,darkEffectPipeline,materialPipeline,materialAddPipeline,materialSubtractPipeline,environmentPipeline;
    id<MTLRenderPipelineState> gsMaterialPipeline,gsMaterialAddPipeline,gsMaterialSubtractPipeline;
    id<MTLDepthStencilState> depth,effectDepth,mapEffectDepth,materialDepth;
    NSMutableArray<id<MTLTexture>>* gpuTextures;
    id<MTLCommandQueue> queue;
    Impl(){initializeMetal();}
    explicit Impl(const fs::path& path,std::span<const SceneActor> actors,const MapObjects* objects,
                  std::span<const std::shared_ptr<const SceneModelAsset>> assets) {
        mapObjects=objects;
        const auto map=read_xmp(read(path));
        if(!assets.empty() && assets.size()!=map.definitions.size())throw std::runtime_error("Owned map definition count");
        unsigned back=640;
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
            const auto asset=assets.empty()?nullptr:assets[placement.definition()];
            if(!assets.empty() && !asset)throw std::runtime_error("Owned map definition unavailable");
            inputs.push_back({file,object?object->model():asset?asset->model:read_xmd(read(file)),instance_matrix(transform.position,transform.rotation,transform.scale,sine),object,
                (transform.flags&1)?std::optional<MapTransform>(transform):std::nullopt,index,(transform.flags&0x10000)==0});
            if(asset){inputs.back().textureSlots=asset->textures;inputs.back().texturesOwned=true;}
            inputs.back().flags=transform.flags;
            inputs.back().ordinal=transform.flags&0x100000?640+--back:nextFrontOrdinal++;
        }
        if(inputs.empty())throw std::runtime_error("Empty field map");
        if(sky_input){auto stage=path;stage.replace_extension(".stg");sky_factors=field_sky_follow(read(stage));}
        {auto stage=path;stage.replace_extension(".stg");const auto bytes=read(stage);shadowSettings=field_shadow_settings(bytes);ordinaryLighting=field_ordinary_lighting(bytes);}
        for(const auto& binding:actors) {
            auto [input,shadow]=actorInput(binding);
            inputs.push_back(std::move(input));shadowActors.push_back(std::move(shadow));
            ++actorCount;++nextFrontOrdinal;
        }
        for(const auto& input:inputs) {
            textureOffsets.push_back(textures.size());
            auto images=inputTextures(input);
            for(auto& image:images)textures.push_back(std::move(image));
            preparation.push_back(inputPreparation(input));
        }
        initializeMetal();
    }
    void initializeMetal() {
        device=MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("No native Metal device");
        backend=acquireBackend(device);
        pipeline=backend->pipeline;effectPipeline=backend->effectPipeline;
        mapEffectPipeline=backend->mapEffectPipeline;dustPipeline=backend->dustPipeline;
        darkEffectPipeline=backend->darkEffectPipeline;materialPipeline=backend->materialPipeline;
        environmentPipeline=backend->environmentPipeline;
        gsMaterialPipeline=backend->gsMaterialPipeline;gsMaterialAddPipeline=backend->gsMaterialAddPipeline;
        gsMaterialSubtractPipeline=backend->gsMaterialSubtractPipeline;
        materialAddPipeline=backend->materialAddPipeline;materialSubtractPipeline=backend->materialSubtractPipeline;
        depth=backend->depth;effectDepth=backend->effectDepth;
        mapEffectDepth=backend->mapEffectDepth;materialDepth=backend->materialDepth;
        gpuTextures=[NSMutableArray array];
        whiteTexture=textures.size();textures.push_back({1,1,0,0,{255,255,255,255}});
        for(const auto& image:textures)[gpuTextures addObject:uploadTexture(image)];
        queue=[device newCommandQueue];
    }
    std::pair<Input,FieldShadowActor> actorInput(const SceneActor& binding) const {
        if(!binding.actor || binding.model_path.empty())throw std::runtime_error("Actor and model path required");
        if(std::any_of(inputs.begin(),inputs.end(),[&](const Input& input){return input.actor==binding.actor;}))
            throw std::runtime_error("Scene actor already registered");
        const auto companion_path=binding.model_path.parent_path()/"shadow.xmd";
        std::optional<ModelFile> companion;
        if(binding.shadow_owned) {
            if(binding.shadow_model)companion=*binding.shadow_model;
        } else if(fs::exists(companion_path))companion=read_xmd(read(companion_path));
        auto name=binding.model_path.parent_path().stem().string();
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(binding.descriptor) {
            const auto flags=binding.descriptor->flags;
            if(flags!=0&&flags!=0x200080&&flags!=0x2000a0&&flags!=0x2000e0)
                throw std::runtime_error("Unrecovered explicit actor flags");
            if(binding.actor->render_flags!=flags||binding.actor->encounter_model)
                throw std::runtime_error("Explicit actor descriptor disagrees with instance");
        }
        auto shadow=binding.descriptor?make_instance_shadow_actor(binding.alias,*binding.actor,
            companion?&*companion:nullptr,binding.descriptor->flags,binding.descriptor->height_enabled):
            make_field_shadow_actor(binding.alias,name,*binding.actor,companion?&*companion:nullptr);
        Input input{binding.model_path,binding.actor->model(),{},binding.actor,{},{},true,shadow.ordinary_after_shadows()};
        input.flags=shadow.flags;input.ordinal=nextFrontOrdinal;
        input.special=!binding.descriptor&&(name=="wkumo"||name=="rkumo"||name=="bkumo");
        input.descriptor=binding.descriptor;input.textureBytes=binding.texture_bytes;input.textureSlots=binding.texture_slots;input.texturesOwned=binding.textures_owned;
        return {std::move(input),std::move(shadow)};
    }
    static std::vector<Texture> inputTextures(const Input& input) {
        std::vector<Texture> images;
        const auto& names=input.actor ? input.actor->textures().names() : input.model.textures;
        if(input.texturesOwned || !input.textureSlots.empty()) {
            if(input.textureSlots.size()!=names.size())throw std::runtime_error("Owned model slot count");
            for(const auto& slot:input.textureSlots) {
                if(slot.handle<0 || !slot.image || !slot.bytes)throw std::runtime_error("Owned model binding unavailable");
                images.push_back(decode_xim(*slot.bytes));
            }
            return images;
        }
        if(!input.textureBytes.empty()) {
            //Owned bytes from the caller's source-order load: no second file read.
            if(input.textureBytes.size()!=names.size())throw std::runtime_error("Owned model texture count");
            for(const auto& bytes:input.textureBytes) {
                if(!bytes)throw std::runtime_error("Owned model texture unavailable");
                images.push_back(decode_xim(*bytes));
            }
            return images;
        }
        for(auto name:names) {
            const fs::path relative=name;
            if(relative.is_absolute() || relative.has_parent_path())throw std::runtime_error("Unsafe scene texture path");
            if(relative.extension().empty())name+=".xim";
            images.push_back(decode_xim(read(input.path.parent_path()/name)));
        }
        return images;
    }
    static std::vector<PartPreparation> inputPreparation(const Input& input) {
        std::vector<PartPreparation> parts;
        std::size_t textureBase=0;
        for(const auto& part:input.model.models) {
            auto& prepared=parts.emplace_back();
            for(const auto& block:part.blocks) {
                prepared.textureBases.push_back(textureBase);textureBase+=block.textures.size();
                if(input.actor&&!input.placement&&!input.descriptor)
                    prepared.fieldBounds.push_back(model_block_bounds(block,false));
                prepared.blocks.push_back({std::vector<std::optional<PreparedTextureState>>(block.textures.size()),
                    std::vector<std::optional<PolygonTextureCoordinates>>(block.polygons.size()),
                    (input.placement||input.descriptor||input.actor)?
                        std::vector<std::array<PreparedMapBaseColor,4>>(block.polygons.size()):std::vector<std::array<PreparedMapBaseColor,4>>{}});
            }
        }
        return parts;
    }
    std::uint64_t sourceTex0(const ResourceTextureSlot& slot,int palette) const {
        if(!sharedTextures || !sharedTextures->handle_live(slot.handle) ||
           sharedTextures->handle_serial(slot.handle)!=slot.handle_serial ||
           sharedTextures->record_of(slot.handle)!=slot.record ||
           sharedTextures->record_serial(slot.record)!=slot.record_serial)
            throw std::runtime_error("Shared GS material has a stale handle/record incarnation");
        const auto image=sharedTextures->image(slot.handle);
        if(!image || image!=slot.image)throw std::runtime_error("Shared GS material image binding changed");
        const auto allocation=sharedTextures->memory().allocation(image->id);
        if(!allocation || allocation->serial!=image->serial)throw std::runtime_error("Shared GS material image allocation is stale");
        std::uint64_t clut=0;
        if(const auto descriptor=sharedTextures->palette(slot.handle,palette<0?0:palette)) {
            const auto value=sharedTextures->memory().allocation(descriptor->id);
            if(!value || value->serial!=descriptor->serial)throw std::runtime_error("Shared GS material CLUT allocation is stale");
            clut=value->descriptor;
        }
        const auto value=allocation->descriptor;
        return 0x20000006a8000000ull | (value&0x3fff) | (((value>>14)&63)<<14) |
            (((value>>32)&63)<<20) | ((clut&0x3fff)<<37) | (((clut>>32)&63)<<51);
    }
    void prepareShared(const Input& input,std::vector<PartPreparation>& parts) {
        if(!sharedTextures)return;
        if(!input.texturesOwned)throw std::runtime_error("Shared GS renderer requires source-owned model slots");
        for(std::size_t p=0;p<input.model.models.size();++p)
            for(std::size_t b=0;b<input.model.models[p].blocks.size();++b) {
                const auto& block=input.model.models[p].blocks[b];
                auto& originals=parts[p].blocks[b].originalTex0;
                originals.resize(block.textures.size());
                for(std::size_t t=0;t<block.textures.size();++t) {
                    const auto& state=block.textures[t];
                    if(state.texture>=0)originals[t]=sourceTex0(input.textureSlots.at(state.texture),state.palette);
                }
            }
    }
    std::shared_ptr<const SceneGsMemory> captureGs() {
        if(!sharedTextures)return {};
        const auto& memory=sharedTextures->memory();
        if(cachedGs && cachedGs->snapshot.identity()==memory.identity() &&
           cachedGs->snapshot.storage_identity()==memory.storage_identity())return cachedGs;
        auto result=std::make_shared<SceneGsMemory>();result->snapshot=memory.snapshot();
        const auto bytes=result->snapshot.physical_bytes(),known=result->snapshot.known_nibbles();
        result->bytes=[device newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
        result->known=[device newBufferWithBytes:known.data() length:known.size() options:MTLResourceStorageModeShared];
        if(!result->bytes||!result->known)throw std::runtime_error("Immutable shared GS Metal buffer allocation failed");
        cachedGs=result;return result;
    }
    id<MTLTexture> uploadTexture(const Texture& image) {
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image.width height:image.height mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[device newTextureWithDescriptor:td];
        if(!texture)throw std::runtime_error("Metal texture allocation failed");
        [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:image.rgba.data() bytesPerRow:image.width*4];
        return texture;
    }
    void addActor(const SceneActor& binding) {
        auto [input,shadow]=actorInput(binding);
        auto images=inputTextures(input);
        auto prepared=inputPreparation(input);
        prepareShared(input,prepared);
        auto uploaded=[NSMutableArray<id<MTLTexture>> arrayWithCapacity:images.size()];
        for(const auto& image:images)[uploaded addObject:uploadTexture(image)];
        inputs.reserve(inputs.size()+1);shadowActors.reserve(shadowActors.size()+1);
        textureOffsets.reserve(textureOffsets.size()+1);preparation.reserve(preparation.size()+1);
        textures.reserve(textures.size()+images.size());
        [gpuTextures addObjectsFromArray:uploaded];
        textureOffsets.push_back(textures.size());
        for(auto& image:images)textures.push_back(std::move(image));
        preparation.push_back(std::move(prepared));
        inputs.push_back(std::move(input));shadowActors.push_back(std::move(shadow));
        ++actorCount;++nextFrontOrdinal;
    }
    void retireInstances() {
        inputs.clear();preparation.clear();textureOffsets.clear();
        shadowActors.clear();sky_input.reset();mapObjects=nullptr;
        actorCount=0;nextFrontOrdinal=0;omittedEffects=0;
    }
    static void validateEffectTexture(const Texture& image) {
        if(!image.width||!image.height||image.width>4096||image.height>4096||
           image.rgba.size()!=std::size_t(image.width)*image.height*4)
            throw std::runtime_error("Invalid effect texture extent or RGBA size");
    }
    std::uint32_t addEffectTexture(const Texture& source) {
        if(!effectTexturesLoaded)throw std::runtime_error("Effect textures are unavailable");
        validateEffectTexture(source);
        if(effectTextureSlots.size()>=0xffffffffu||textures.size()>0x7fffffffu)
            throw std::runtime_error("Effect texture identity limit");
        Texture image=source;
        auto uploaded=uploadTexture(image);
        auto nextGpu=[gpuTextures mutableCopy];
        [nextGpu addObject:uploaded];
        textures.reserve(textures.size()+1);
        effectTextureSlots.reserve(effectTextureSlots.size()+1);
        const auto slot=std::uint32_t(effectTextureSlots.size());
        effectTextureSlots.push_back(textures.size());
        textures.push_back(std::move(image));
        gpuTextures=nextGpu;
        return slot;
    }
    void loadEffects(const fs::path& directory) {
        loadEffectsFrom([&](unsigned i){return read(directory/effect_texture_names[i]);});
    }
    //Host upload of already verified owned bytes (no file reread, not a logical allocation).
    void loadEffectsFrom(const std::function<std::vector<std::uint8_t>(unsigned)>& bytes) {
        if(effectTexturesLoaded)throw std::runtime_error("Effect textures already loaded");
        std::vector<Texture> images;
        std::vector<std::size_t> slots;
        images.reserve(effect_texture_names.size());slots.reserve(effect_texture_names.size());
        auto nextGpu=[gpuTextures mutableCopy];
        for(unsigned index=0;index<effect_texture_names.size();++index) {
            auto image=decode_xim(bytes(index));
            validateEffectTexture(image);
            slots.push_back(textures.size()+images.size());
            [nextGpu addObject:uploadTexture(image)];
            images.push_back(std::move(image));
        }
        textures.reserve(textures.size()+images.size());
        for(auto& image:images)textures.push_back(std::move(image));
        effectTextureSlots=std::move(slots);gpuTextures=nextGpu;
        effectTexturesLoaded=true;
    }
    PreparedFrame prepare(const Matrix4& view,const Float4& projection,const AttachedEffects* effects,const Float4& camera,float farClip,const Float4& cameraPosition,const FieldProjectionFrame* fieldProjection,float shadowFactor,bool restoreShadowAlpha) {
        shadowFrame=fieldProjection?field_shadow_frame(shadowActors,view,*fieldProjection,shadowSettings,shadowFactor,sine,sortDepthShift):FieldShadowFrame{};
        std::vector<Vertex> vertices;
        std::vector<Draw> draws;
        bool hadOrdinaryGeometry=false,hadEligibleInput=false;
        okage::Float4 low{INFINITY,INFINITY,INFINITY,0},high{-INFINITY,-INFINITY,-INFINITY,0};
        for(std::size_t inputIndex=0;inputIndex<inputs.size();++inputIndex) {
            const auto& input=inputs[inputIndex];const auto textureOffset=textureOffsets[inputIndex];
            if(input.placement && !(mapObjects?mapObjects->visible(*input.placement):input.visible))continue;
            if(input.actor && !input.actor->visible)continue;
            hadEligibleInput=true;
            const bool mapMaterial=input.placement.has_value();
            const bool explicitActor=input.descriptor.has_value();
            if(explicitActor&&((input.actor->render_flags^input.descriptor->flags)&~0x100000u))
                throw std::runtime_error("Explicit actor flags changed after binding");
            const bool fieldActor=input.actor&&!mapMaterial&&!explicitActor;
            const bool fieldMode0=fieldActor&&input.actor->render_flags==0;
            const bool ordinaryInstance=input.actor&&input.actor->render_flags==0x100000;
            if(fieldActor&&!fieldMode0&&!ordinaryInstance)
                throw std::runtime_error("Actor instance render mode is unrecovered: "+input.path.string()+
                    " flags="+std::to_string(input.actor->render_flags));
            const bool ordinaryMaterial=mapMaterial||explicitActor||fieldMode0||ordinaryInstance;
            std::optional<Matrix4> directionMatrix;
            auto lightColors=ordinaryLighting.colors;
            if(ordinaryInstance||(explicitActor&&(input.actor->render_flags&0x100000))) {
                lightColors[3]=input.actor->channels().at(3);
            }
            auto world=input.actor ? input.actor->world(sine) : input.world;
            if(input.facing)world=map_instance_matrix(*input.facing,{-camera[0],-camera[1],0,1},sine);
            if(input.placement&&mapObjects&&!input.actor)
                world=map_instance_matrix(mapObjects->transform(*input.placement),{-camera[0],-camera[1],0,1},sine);
            if(sky_input&&inputIndex==*sky_input){
                const auto position=sky_follow_position(cameraPosition,sky_factors);
                world[3]={position[0],position[1],position[2],1};
            }
            const unsigned sort=fieldProjection?field_ordinary_bucket(input.flags,field_depth_bucket(view,world[3],fieldProjection->ordinary,sortDepthShift),input.special):0;
            std::size_t partIndex=0;
            for (const auto& part:input.model.models) {
                std::vector<Draw> mapTranslucent;
                auto& preparedPart=preparation.at(inputIndex).at(partIndex++);
                auto pose=input.actor&&input.actor->has_motion() ? okage::animated_pose(part,input.actor->channels(),sine,input.actor->animation().scale_channels()) : okage::rest_pose(part,sine);
                {
                    const auto viewWorld=okage::compose_pose(view,std::span(&world,1));
                    pose=okage::compose_pose(viewWorld[0],pose);
                }
                std::vector<ModelBlockSelection> selected;
                if(fieldProjection&&!mapMaterial&&!explicitActor&&(fieldMode0||ordinaryInstance))
                    selected=visible_model_blocks(preparedPart.fieldBounds,pose,fieldProjection->ordinary);
                else for(unsigned block=0;block<part.blocks.size();++block)selected.push_back({block,0});
                for (const auto& selection:selected) {
                    const auto blockIndex=std::size_t(selection.block)+1;
                    const auto& block=part.blocks.at(selection.block);
                    auto& prepared=preparedPart.blocks.at(selection.block);
                    const auto textureBase=preparedPart.textureBases.at(selection.block);
                    const auto firstVertex=vertices.size(),firstDraw=draws.size(),firstMapTranslucent=mapTranslucent.size();
                    const auto positions=okage::posed_positions(block,pose);
                    std::optional<OrdinaryNormalCache> normals;
                    std::vector<Float4> lights;std::vector<bool> lightDefined;
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
                    const bool mapNormalBlock=ordinaryMaterial&&map_material_block_uses_normals(block);
                    std::vector<Draw> extraOpaque,extraTranslucent;
                    std::size_t polygonIndex=0;
                    for (const auto& polygon:block.polygons) {
                        auto& preparedUV=prepared.coordinates.at(polygonIndex++);
                        const auto triangles=okage::triangle_corners(polygon.corners.size());
                        std::array<std::array<float,2>,4> polygonUV{};
                        Draw draw{std::uint32_t(vertices.size()),0,-1,{}};
                        draw.bucket=sort;draw.ordinal=input.ordinal;draw.phase=sort>517?1:0;draw.mapMaterial=mapMaterial||explicitActor;
                        draw.unculledFieldActor=fieldMode0&&!input.actor->encounter_model;
                        const Material* material=nullptr;
                        bool mapTransparent=false;
                        MapMaterialExtraPasses extraPasses;
                        if(ordinaryMaterial) {
                            if(polygon.material<0)throw std::runtime_error("Map polygon has no material");
                            material=&block.materials.at(polygon.material);
                            try {
                                mapTransparent=map_material_class(*material,-1).translucent;
                                extraPasses=map_material_extra_passes(*material,mapNormalBlock);
                                if(extraPasses.second)throw std::runtime_error("Unrecovered map material second extra lighting pass");
                                if(extraPasses.first&&!materialEnvironment)throw std::runtime_error("Material first extra pass has no owned environment texture");
                                draw.materialBlend=material_blend_register(*material);
                            } catch(const std::exception& e) {
                                throw std::runtime_error("Material "+input.path.string()+" model "+std::to_string(partIndex-1)+
                                    " block "+std::to_string(selection.block)+" material "+std::to_string(polygon.material)+
                                    " flags="+std::to_string(material->words[7])+": "+e.what());
                            }
                        }
                        const okage::TextureState* state=nullptr;
                        const bool textureEnabled=ordinaryMaterial?map_material_textured(polygon):polygon.texture_state>=0;
                        if(ordinaryMaterial&&textureEnabled&&block.textures.at(polygon.texture_state).texture<0)
                            throw std::runtime_error("Textured map polygon has no image");
                        if (textureEnabled && block.textures.at(polygon.texture_state).texture>=0) {
                            state=&block.textures.at(polygon.texture_state);
                            draw.image=textureOffset+(input.actor ? input.actor->textures().images().at(textureBase+polygon.texture_state) : state->texture);
                            const auto& original=textures.at(textureOffset+state->texture);
                            auto& preparedState=prepared.textures.at(polygon.texture_state);
                            if(!preparedState)preparedState.emplace(*state,original.width,original.height);
                            const auto clamp=preparedState->clamp();
                            draw.state={std::uint32_t(clamp>>4)&1023,std::uint32_t(clamp>>14)&1023,
                                std::uint32_t(clamp>>24)&1023,std::uint32_t(clamp>>34)&1023,
                                std::uint32_t(clamp)&3,std::uint32_t(clamp>>2)&3,1,0};
                            if(sharedTextures && ordinaryMaterial) {
                                const auto selected=input.actor?input.actor->textures().images().at(textureBase+polygon.texture_state):state->texture;
                                const auto& slot=input.textureSlots.at(selected);
                                // Variant0 restores exact saved TEX0. Replacement
                                // callbacks write only reg6 using default CLUT.
                                const auto original=prepared.originalTex0.at(polygon.texture_state);
                                if(!original)throw std::runtime_error("Shared GS original texture packet is unavailable");
                                draw.gs_texture=GsTextureRegisters{selected==state->texture?*original:sourceTex0(slot,0),
                                    0x60,clamp,0x8000008080ull};draw.source_handle=slot.handle;
                            }
                            // Ordinary setup TEXA has TA0=128, AEM=1. PSM24
                            // stores no alpha: RGB zero becomes transparent.
                            if((ordinaryMaterial||ordinaryInstance) && textures.at(draw.image).pixel_format==1)draw.state[6]|=2;
                            if(!preparedUV) {
                                for(std::size_t i=0;i<polygon.corners.size();++i) {
                                    const auto index=polygon.corners[i][3];
                                    if(index<0 || !uvDefined.at(index))throw std::runtime_error("Missing preview UV");
                                    polygonUV[i]={uv.at(index)[0],uv.at(index)[1]};
                                }
                                preparedUV=preparedState->coordinates(std::span(polygonUV).first(polygon.corners.size()));
                            }
                            polygonUV=*preparedUV;
                        }
                        for (auto triangle:triangles) for (auto index:triangle) {
                            const auto& corner=polygon.corners.at(index);
                            Vertex vertex{positions.at(corner[0]),{1,1,1,1},{0,0,0,0}};
                            if (corner[2]>=0 && colorDefined.at(corner[2])) vertex.color=colors.at(corner[2]);
                            else throw std::runtime_error("Missing preview source color");
                            if(material&&ordinaryMaterial&&(corner[1]<0||(material->words[7]&1))) {
                                vertex.color=prepared.mapBaseColors.at(polygonIndex-1)[index].get(
                                    *material,vertex.color,ordinaryLighting.base,!fieldProjection);
                            } else if(material) {
                                vertex.color=material_source_color(*material,vertex.color);
                                if(ordinaryMaterial) {
                                    const auto normal=corner[1];
                                    const auto classification=map_material_class(*material,normal);
                                    const Float4* light=&ordinaryLighting.base;
                                    if(!classification.base_lighting) {
                                        if(!normals) {
                                            normals=ordinary_normal_cache(block,pose);
                                            lights.resize(normals->normals.size());lightDefined.resize(lights.size());
                                        }
                                        if(!normals->defined.at(normal))throw std::runtime_error("Missing map normal");
                                        if(!directionMatrix)try {
                                            directionMatrix=ordinary_direction_matrix(ordinaryLighting.directions,view);
                                        } catch(const std::exception& e) {
                                            throw std::runtime_error("Map direction matrix "+input.path.string()+" block "+std::to_string(blockIndex-1)+": "+e.what());
                                        }
                                        if(!lightDefined.at(normal)) {
                                            try {
                                                lights[normal]=ordinary_light_normal(normals->normals[normal],*directionMatrix,lightColors,normals->path).lighting_cache;
                                            } catch(const std::exception& e) {
                                                throw std::runtime_error("Map normal "+input.path.string()+" block "+std::to_string(blockIndex-1)+" normal "+std::to_string(normal)+": "+e.what());
                                            }
                                            lightDefined[normal]=true;
                                        }
                                        light=&lights[normal];
                                    }
                                    vertex.color=fieldProjection?ordinary_material_modulation(vertex.color,*light):ordinary_material_color(vertex.color,*light);
                                }
                            }
                            if (state) {
                                const auto& value=polygonUV[index];
                                vertex.uv={value[0],value[1],0,0};
                            }
                            for (unsigned axis=0;axis<3;++axis) {
                                if (!std::isfinite(vertex.position[axis])) throw std::runtime_error("Non-finite preview position");
                                low[axis]=std::min(low[axis],vertex.position[axis]); high[axis]=std::max(high[axis],vertex.position[axis]);
                            }
                            vertices.push_back(vertex); ++draw.count;
                        }
                        if(ordinaryMaterial) {
                            if(mapTransparent)mapTranslucent.push_back(draw);
                            else {draw.state[7]=3;draws.push_back(draw);}
                        } else draws.push_back(draw);
                        if(extraPasses.first) {
                            Draw extra=draw;extra.first=std::uint32_t(vertices.size());extra.count=0;
                            extra.image=std::int32_t(*materialEnvironment);extra.materialBlend=0x44;
                            extra.state={0,1023,0,1023,2,2,5,3};
                            if(sharedTextures) {
                                if(!environmentTex0)throw std::runtime_error("Shared GS environment packet is unavailable");
                                extra.gs_texture=GsTextureRegisters{*environmentTex0,0x60,
                                    std::uint64_t(2)|(std::uint64_t(2)<<2)|(std::uint64_t(1023)<<14)|(std::uint64_t(1023)<<34),0x8000008080ull};
                                extra.source_handle=environmentHandle;
                            }
                            if(textures.at(*materialEnvironment).pixel_format==1)extra.state[6]|=2;
                            const auto source=ordinary_environment_source(std::bit_cast<float>(material->words[3]));
                            for(auto triangle:triangles)for(auto index:triangle) {
                                const auto normal=polygon.corners.at(index)[1];
                                //183720's base-slot sentinel has no generated normal UV.
                                if(normal<0||(material->words[7]&1)||!normals||!normals->defined.at(normal))
                                    throw std::runtime_error("Unrecovered base-slot environment UV: "+input.path.string());
                                const auto lit=ordinary_light_normal(normals->normals.at(normal),*directionMatrix,lightColors,normals->path);
                                const auto uv=ordinary_environment_uv(lit.normalized_normal,materialEnvironmentRegion);
                                Vertex vertex{positions.at(polygon.corners.at(index)[0]),
                                    fieldProjection?ordinary_material_modulation(source,lit.lighting_cache):ordinary_material_color(source,lit.lighting_cache),
                                    {uv[0],uv[1],0,0}};
                                vertices.push_back(vertex);++extra.count;
                            }
                            (mapTransparent?extraTranslucent:extraOpaque).push_back(extra);
                        }
                    }
                    draws.insert(draws.end(),extraOpaque.begin(),extraOpaque.end());
                    mapTranslucent.insert(mapTranslucent.end(),extraTranslucent.begin(),extraTranslucent.end());
                    hadOrdinaryGeometry=hadOrdinaryGeometry||vertices.size()!=firstVertex;
                    if(fieldProjection&&field_raster_block_rejected(std::span(vertices).subspan(firstVertex),fieldProjection->ordinary)) {
                        vertices.resize(firstVertex);draws.resize(firstDraw);mapTranslucent.resize(firstMapTranslucent);
                    }
                }
                map_translucent_passes(std::span<const Draw>(mapTranslucent),[&](Draw draw,unsigned alpha_test){
                    draw.state[7]=alpha_test;draws.push_back(draw);
                });
            }
        }
        if(fieldProjection)std::stable_sort(draws.begin(),draws.end(),[](const Draw& a,const Draw& b){
            return a.bucket!=b.bucket?a.bucket<b.bucket:a.ordinal>b.ordinal;
        });
        EffectSubmissions previewSubmissions;
        if(!effectSubmissions)for(unsigned slot=0;slot<84;++slot) {
            if(mapEffects&&(*mapEffects)[slot]&&((*mapEffects)[slot]->id!=17||((*mapEffects)[slot]->flags&0x20)))
                previewSubmissions.push_back({slot,(*mapEffects)[slot]->snapshot()});
            if(effects&&(*effects)[slot])previewSubmissions.push_back({slot,*(*effects)[slot]});
        }
        for(const auto& submission:effectSubmissions?*effectSubmissions:previewSubmissions){
        if(const auto* effect=std::get_if<MapEffect>(&submission.effect)) {
            if(!effectTexturesLoaded)throw std::runtime_error("Map effect textures are unavailable");
            for(const auto& e:map_effect_draws(*effect,view,camera,farClip,sine,effectTint?&*effectTint:nullptr,
                    effect->flags&0x400?EffectDrawPass::Deferred:EffectDrawPass::Ordinary,
                    [&](std::uint32_t logical){
                        const auto& image=textures.at(effectTextureSlots.at(logical));
                        return std::array<std::uint32_t,2>{image.width,image.height};
                    })){
                const bool untextured=e.texture==0xffffffffu;
                const auto texture=untextured?-1:std::int32_t(effectTextureSlots.at(e.texture));
                if(!untextured){const auto& image=textures.at(texture);
                    if(e.uv[2]>=image.width||e.uv[3]>=image.height)throw std::runtime_error("Map effect texture bounds");}
                if(e.clamp_mode!=2&&e.clamp_mode!=3)throw std::runtime_error("Unrecovered effect texture addressing");
                //0x1028e0/0x103234: generic CLAMP packing reverses each pair
                // relative to mode2. Mode3 uses extent-1 as mask, origin as OR.
                const auto region=e.clamp_mode==3?
                    std::array<unsigned,4>{e.uv[2],e.uv[0],e.uv[3],e.uv[1]}:
                    std::array<unsigned,4>{e.uv[0],e.uv[2],e.uv[1],e.uv[3]};
                Draw draw{std::uint32_t(vertices.size()),std::uint32_t(e.vertices.size()),std::int32_t(texture),
                    {region[0],region[1],region[2],region[3],e.clamp_mode,e.clamp_mode,untextured?0u:1u,(e.additive||e.subtractive)?0u:1u},true,e.additive,!e.additive&&!e.subtractive};
                draw.depthAlways=!e.depth_test;
                draw.subtractive=e.subtractive;
                draw.phase=2;draw.nearClip=effect->near_clip;draw.deferred=(effect->flags&0x400)!=0;
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
        if(const auto* attached=std::get_if<AttachedEffect>(&submission.effect);attached&&!(attached->animation.flags&0x4300)) {
            if(!effectTexturesLoaded)throw std::runtime_error("Effect textures are unavailable");
            for(const auto& sprite:attached->animation.sprites) {
                auto origin=attached->position;for(unsigned i=0;i<3;++i)origin[i]+=sprite.position[i];
                const auto world=instance_matrix(origin,{1-camera[0],1-camera[1],sprite.rotation[2],0},sprite.scale,sine);
                const auto matrix=compose_pose(view,std::span(&world,1))[0];
                const std::array<Float4,4> corners={Float4{0,-1,0,1},Float4{1,-1,0,1},Float4{1,0,0,1},Float4{0,0,0,1}};
                if(sprite.texture<14||sprite.texture>16)throw std::runtime_error("Unsupported effect texture");
                const auto texture=effectTextureSlots.at(sprite.texture);const auto& image=textures.at(texture);
                const unsigned left=sprite.uv[0],top=sprite.uv[1],right=sprite.uv[2],bottom=sprite.uv[3];
                Draw draw{std::uint32_t(vertices.size()),6,std::int32_t(texture),{left,std::min(right,image.width-1),top,std::min(bottom,image.height-1),2,2,1,0},true};
                draw.phase=2;draw.nearClip=attached->near_clip;
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
        if (hadEligibleInput&&vertices.empty()&&!hadOrdinaryGeometry) throw std::runtime_error("Empty preview geometry");
        if(fieldProjection){
            std::vector<Vertex> projected;
            for(auto& draw:draws){
                auto settings=draw.effect?(draw.deferred?fieldProjection->deferred:fieldProjection->effects):fieldProjection->ordinary;
                if(draw.effect)settings.clip_near=draw.nearClip;
                auto part=field_raster_triangles(std::span(vertices).subspan(draw.first,draw.count),settings,draw.materialBlend||draw.additive||draw.dust||draw.subtractive);
                draw.first=static_cast<unsigned>(projected.size());draw.count=static_cast<unsigned>(part.size());
                projected.insert(projected.end(),part.begin(),part.end());
            }
            vertices=std::move(projected);
        }else for(auto& v:vertices) {
            const auto p=v.position;
            v.position={p[0]*projection[0],-p[1]*projection[1],(p[2]*projection[2]+projection[3]+p[2])*0.5f,p[2]};
        }
        return snapshot(std::move(vertices),std::move(draws),fieldProjection,restoreShadowAlpha);
    }
    PreparedFrame snapshot(std::vector<Vertex> vertices,std::vector<Draw> draws,const FieldProjectionFrame* fieldProjection,bool restoreOpaqueAlpha=true) {
        auto data=std::make_shared<PreparedFrame::Data>();
        data->renderer=identity;data->vertices=std::move(vertices);data->draws=std::move(draws);
        data->gs=captureGs();
        std::ostringstream observation;observation<<"{\"shared_gs\":"<<(data->gs?"true":"false");
        if(data->gs)observation<<",\"content_generation\":"<<data->gs->snapshot.identity().content_generation
            <<",\"ownership_epoch\":"<<data->gs->snapshot.identity().ownership_epoch;
        observation<<",\"draws\":[";bool comma=false;
        for(const auto& draw:data->draws)if(draw.gs_texture) {
            const auto& regs=*draw.gs_texture;
            observation<<(comma?",":"")<<"{\"first\":"<<draw.first<<",\"count\":"<<draw.count<<",\"handle\":"<<draw.source_handle
                <<",\"tex0\":\"0x"<<std::hex<<regs.tex0<<"\",\"tex1\":\"0x"<<regs.tex1
                <<"\",\"clamp\":\"0x"<<regs.clamp<<"\",\"texa\":\"0x"<<regs.texa<<"\"}"<<std::dec;comma=true;
        }
        observation<<"]}";textureSamplingObservation=observation.str();
        data->shadows=shadowFrame;data->clear=clearRGB;data->textures=[gpuTextures copy];
        data->white_texture=whiteTexture;data->field_projection=fieldProjection!=nullptr;data->restore_shadow_alpha=restoreOpaqueAlpha;
        if(fieldProjection){data->shadow_mapping.offset_x=fieldProjection->shadow.center_x-320;
            data->shadow_mapping.offset_y=fieldProjection->shadow.center_y-112;}
        PreparedFrame frame;frame.data_=std::move(data);return frame;
    }
    PreparedFrame prepareEmpty(const FieldProjectionFrame& projection) {
        //1b9be0 always queues1b1690 ->1b11b0 alpha64, even empty.
        //1b1ad8 restores registers only; omit the legacy display-alpha255 pass.
        shadowFrame={};return snapshot({},{},&projection,false);
    }
    SubmittedFrame submitPrepared(const PreparedFrame& frame,const SceneUiPass* ui,const SceneGpuPass* gpuPass,std::optional<std::array<std::uint8_t,4>> executionClear,float depthClear) {
        if(!frame.data_||frame.data_->renderer!=identity)throw std::runtime_error("Prepared scene belongs to another renderer or is empty");
        if(!std::isfinite(depthClear)||depthClear<0||depthClear>1)throw std::runtime_error("Invalid scene depth clear");
        const auto& data=*frame.data_;
        if(ui&&!data.field_projection)throw std::runtime_error("Scene UI requires GS field projection depth");
        if(gpuPass&&!gpuPass->encode)throw std::runtime_error("Scene GPU pass has no encoder");
        const auto& vertices=data.vertices;const auto& draws=data.draws;
        const bool beforeDeferred=gpuPass&&gpuPass->before_deferred_effects;
        if(beforeDeferred) {
            if(!data.field_projection)throw std::runtime_error("Title GPU pass requires GS field projection");
            for(const auto& draw:draws) {
                if(!draw.effect&&draw.bucket>=0x40d)
                    throw std::runtime_error("Title GPU pass model bucket overlaps images");
                if(draw.effect&&(!draw.deferred||draw.phase!=2))
                    throw std::runtime_error("Title GPU pass requires deferred effects only");
            }
        }
        shadowFrame=data.shadows;
        const auto clear=executionClear.value_or(std::array<std::uint8_t,4>{data.clear[0],data.clear[1],data.clear[2],255});
        auto targetDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:640 height:224 mipmapped:NO];
        targetDesc.usage=MTLTextureUsageRenderTarget;targetDesc.storageMode=MTLStorageModeShared;
        id<MTLTexture> ticketColor=[device newTextureWithDescriptor:targetDesc];
        auto depthDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:640 height:224 mipmapped:NO];
        depthDesc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;depthDesc.storageMode=MTLStorageModePrivate;
        id<MTLTexture> ticketDepth=[device newTextureWithDescriptor:depthDesc];
        if(!ticketColor||!ticketDepth)throw std::runtime_error("Scene frame target allocation failed");
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=ticketColor; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(clear[0]/255.,clear[1]/255.,clear[2]/255.,clear[3]/255.);
        pass.depthAttachment.texture=ticketDepth; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.clearDepth=depthClear;
        pass.depthAttachment.storeAction=MTLStoreActionStore;
        id<MTLCommandBuffer> command=[queue commandBuffer];
        if(!command||!command.retainedReferences)throw std::runtime_error("Scene frame requires a retaining Metal command buffer");
        id<MTLBuffer> vertexBuffer=vertices.empty()?nil:[device newBufferWithBytes:vertices.data() length:vertices.size()*sizeof(Vertex) options:MTLResourceStorageModeShared];
        const bool hasEnvironment=std::any_of(draws.begin(),draws.end(),[](const auto& draw){return draw.count&&draw.materialBlend&&(draw.state[6]&4u);});
        const bool hasGs=std::any_of(draws.begin(),draws.end(),[](const auto& draw){return draw.count&&draw.gs_texture.has_value();});
        id<MTLBuffer> invalidGs=nil;
        if(hasGs) {
            if(!data.gs)throw std::runtime_error("Shared GS draw lost its immutable memory snapshot");
            const std::uint32_t untouched=UINT32_MAX;
            invalidGs=[device newBufferWithBytes:&untouched length:sizeof(untouched) options:MTLResourceStorageModeShared];
            if(!invalidGs)throw std::runtime_error("Shared GS validation buffer allocation failed");
        }
        id<MTLBuffer> invalidEnvironment=nil;
        if(hasEnvironment) {
            const std::uint32_t untouched=UINT32_MAX;
            invalidEnvironment=[device newBufferWithBytes:&untouched length:sizeof(untouched) options:MTLResourceStorageModeShared];
            if(!invalidEnvironment)throw std::runtime_error("Environment validation allocation failed");
        }
        auto encode=[&](unsigned stage){
        id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline]; [encoder setDepthStencilState:depth]; [encoder setCullMode:MTLCullModeNone];
        if(vertexBuffer)[encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        if(invalidEnvironment)[encoder setFragmentBuffer:invalidEnvironment offset:0 atIndex:1];
        for (const auto& draw:draws) {
            if(data.field_projection) {
                if(stage==0&&draw.phase!=0)continue;
                if(stage==1&&(draw.phase==0||(beforeDeferred&&draw.effect)))continue;
                if(stage==2&&(!draw.effect||!draw.deferred))continue;
            }
            if(!draw.count)continue;
            [encoder setFrontFacingWinding:draw.mapMaterial?MTLWindingCounterClockwise:MTLWindingClockwise];
            [encoder setCullMode:draw.materialBlend&&!draw.unculledFieldActor?MTLCullModeBack:MTLCullModeNone];
            [encoder setRenderPipelineState:draw.gs_texture?(draw.materialBlend==0x48?gsMaterialAddPipeline:draw.materialBlend==0x42?gsMaterialSubtractPipeline:gsMaterialPipeline):draw.materialBlend?((draw.state[6]&4u)?environmentPipeline:draw.materialBlend==0x48?materialAddPipeline:draw.materialBlend==0x42?materialSubtractPipeline:materialPipeline):draw.subtractive?darkEffectPipeline:draw.dust?dustPipeline:draw.additive?mapEffectPipeline:draw.effect?effectPipeline:pipeline];
            [encoder setDepthStencilState:draw.depthAlways?effectDepth:draw.materialBlend?(draw.state[7]!=2?materialDepth:mapEffectDepth):(draw.additive||draw.dust||draw.subtractive)?mapEffectDepth:draw.effect?effectDepth:depth];
            [encoder setFragmentTexture:data.textures[draw.image<0 ? data.white_texture : draw.image] atIndex:0];
            [encoder setFragmentBytes:draw.state.data() length:sizeof(draw.state) atIndex:0];
            if(draw.gs_texture) {
                [encoder setFragmentBuffer:invalidGs offset:0 atIndex:1];
                [encoder setFragmentBuffer:data.gs->bytes offset:0 atIndex:2];
                [encoder setFragmentBuffer:data.gs->known offset:0 atIndex:3];
                [encoder setFragmentBytes:&*draw.gs_texture length:sizeof(GsTextureRegisters) atIndex:4];
            } else if(invalidEnvironment)[encoder setFragmentBuffer:invalidEnvironment offset:0 atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:draw.first vertexCount:draw.count];
        }
        [encoder endEncoding];
        };
        encode(0);
        if(data.field_projection){
            if(!shadowMetal)shadowMetal=std::make_shared<ShadowMetal>(device);
            std::vector<ShadowRasterDraw> shadowDraws;shadowDraws.reserve(shadowFrame.draws.size());
            for(const auto& draw:shadowFrame.draws)shadowDraws.push_back({draw.prism,draw.color});
            shadowMetal->encode(command,ticketColor,ticketDepth,shadowDraws,data.shadow_mapping,data.restore_shadow_alpha);
            pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
            pass.depthAttachment.loadAction=MTLLoadActionLoad;
            encode(1);
        }
        std::function<void()> validatePass;
        if(gpuPass){
            validatePass=gpuPass->encode(command,ticketColor,ticketDepth);
            if(!validatePass)throw std::runtime_error("Scene GPU pass has no completion validator");
        }
        if(beforeDeferred)encode(2);
        std::optional<UiMetalValidation> uiValidation;
        auto uiOwners=std::make_shared<std::vector<std::shared_ptr<const void>>>();
        if(ui){
            if(!uiMetal)uiMetal=std::make_shared<UiMetal>(device);
            auto retainResolver=[owners=uiOwners,resolver=ui->textures](const UiGeometryTexture& binding) {
                auto resolved=resolver(binding);
                if(resolved.owner)owners->push_back(resolved.owner);
                return resolved;
            };
            uiValidation=uiMetal->encode(command,ticketColor,ticketDepth,ui->geometry,ui->baseline,retainResolver);
        }
        auto state=std::make_shared<SubmittedFrame::State>();
        state->device=device;state->queue=queue;state->command=command;
        state->color=ticketColor;state->depth=ticketDepth;state->vertices=vertexBuffer;
        state->backend=backend;state->shadow_metal=shadowMetal;state->ui_metal=uiMetal;
        state->prepared_owner=frame.data_;state->ui_owners=std::move(uiOwners);
        if(invalidEnvironment)state->validators.push_back([invalidEnvironment]{
            const auto address=*static_cast<const std::uint32_t*>(invalidEnvironment.contents);
            if(address!=UINT32_MAX)throw std::runtime_error("Unrecovered environment sampled texture address outside owned image: x="+
                std::to_string(address&0xffffu)+", y="+std::to_string(address>>16));
        });
        if(invalidGs)state->validators.push_back([invalidGs,memory=data.gs]{
            const auto code=*static_cast<const std::uint32_t*>(invalidGs.contents);
            if(code==UINT32_MAX)return;
            if(code==0xfffffffeu||code==0xfffffffdu)throw std::runtime_error("Unsupported shared GS texture register/coordinate state code="+std::to_string(code));
            const auto address=(code&0x7fffffu)>>1,nibble=code&1u;
            const auto value=memory->snapshot.observe_nibble(address,std::uint8_t(nibble));
            std::ostringstream message;message<<"Unknown shared GS "<<((code&0x800000u)?"CLUT":"texture")
                <<" nibble address=0x"<<std::hex<<address<<std::dec<<" nibble="<<nibble<<" owner=";
            if(value.allocation_owner)message<<*value.allocation_owner;else message<<"none";
            throw std::runtime_error(message.str());
        });
        if(validatePass)state->validators.push_back(std::move(validatePass));
        if(uiValidation)state->validators.push_back([validation=std::move(*uiValidation),command]{
            validation.validate_completed(command);
        });
        [command commit];
        return SubmittedFrame(std::move(state));
    }
};
void SceneRenderer::prepare_backend() {
    @autoreleasepool { (void)acquireBackend(MTLCreateSystemDefaultDevice()); }
}
bool SceneRenderer::SubmittedFrame::ready() const noexcept {
    if(!state_||!state_->command)return false;
    const auto status=state_->command.status;
    return status==MTLCommandBufferStatusCompleted||status==MTLCommandBufferStatusError;
}
Texture SceneRenderer::SubmittedFrame::readback() const {
    if(!state_)throw std::runtime_error("Empty submitted scene frame");
    const auto state=state_;
    {
        std::unique_lock lock(state->mutex);
        state->changed.wait(lock,[&]{return !state->validation_in_progress;});
        if(state->failure)std::rethrow_exception(state->failure);
        if(state->image)return *state->image;
        state->validation_in_progress=true;
    }
    try {
        [state->command waitUntilCompleted];
        if(state->command.status!=MTLCommandBufferStatusCompleted) {
            const char* description=state->command.error.localizedDescription.UTF8String;
            throw std::runtime_error(description?description:"Scene Metal command failed");
        }
        for(const auto& validate:state->validators)validate();
        Texture image{640,224,0,0,std::vector<std::uint8_t>(640*224*4)};
        [state->color getBytes:image.rgba.data() bytesPerRow:640*4
            fromRegion:MTLRegionMake2D(0,0,640,224) mipmapLevel:0];
        {
            std::lock_guard lock(state->mutex);
            state->image=std::move(image);
            state->validation_in_progress=false;
        }
        state->changed.notify_all();
        std::lock_guard lock(state->mutex);
        return *state->image;
    } catch(...) {
        const auto failure=std::current_exception();
        {
            std::lock_guard lock(state->mutex);
            state->failure=failure;
            state->validation_in_progress=false;
        }
        state->changed.notify_all();
        std::rethrow_exception(failure);
    }
}
Texture SceneRenderer::SubmittedFrame::wait() const{return readback();}
std::size_t SceneRenderer::cached_backend_count() {
    auto& cache=backendCache();
    std::lock_guard lock(cache.mutex);
    return cache.entries.size();
}
SceneRenderer::SceneRenderer():impl_(std::make_unique<Impl>()){}
SceneRenderer::SceneRenderer(const fs::path& map,std::span<const SceneActor> actors,const MapObjects* objects,
                             std::span<const std::shared_ptr<const SceneModelAsset>> assets):
    impl_(std::make_unique<Impl>(map,actors,objects,assets)){}
SceneRenderer::~SceneRenderer()=default;
void SceneRenderer::add_actor(const SceneActor& actor){impl_->addActor(actor);}
void SceneRenderer::retire_instances(){impl_->retireInstances();}
void SceneRenderer::load_effect_textures(const fs::path& path){impl_->loadEffects(path);}
void SceneRenderer::load_effect_textures(const EffectTextureBytes& bytes) {
    impl_->loadEffectsFrom([&](unsigned i){
        if(bytes[i].empty())throw std::runtime_error("Verified effect texture bytes missing");
        return bytes[i];
    });
}
std::uint32_t SceneRenderer::add_effect_texture(const Texture& image){return impl_->addEffectTexture(image);}
EffectTextureSizes SceneRenderer::effect_texture_sizes() const {
    if(!impl_->effectTexturesLoaded)throw std::runtime_error("Effect textures are unavailable");
    EffectTextureSizes result{};
    for(unsigned i=0;i<result.size();++i){const auto& t=impl_->textures.at(impl_->effectTextureSlots.at(i));result[i]={t.width,t.height};}
    return result;
}
void SceneRenderer::set_effect_light_tint(const Float4& tint){impl_->effectTint=tint;}
void SceneRenderer::set_clear_rgb(std::array<std::uint8_t,3> rgb){impl_->clearRGB=rgb;}
void SceneRenderer::set_sort_depth_shift(unsigned shift){
    if(shift>31)throw std::runtime_error("Unsupported sort depth shift");
    impl_->sortDepthShift=shift;
}
void SceneRenderer::set_stage_environment(const FieldOrdinaryLighting& light,const FieldShadowSettings& shadow,const Float4& sky) {
    if(light.base!=impl_->ordinaryLighting.base)
        for(auto& input:impl_->preparation)for(auto& part:input)for(auto& block:part.blocks)
            for(auto& polygon:block.mapBaseColors)polygon={};
    impl_->ordinaryLighting=light;impl_->shadowSettings=shadow;impl_->sky_factors=sky;
}
void SceneRenderer::set_shared_texture_owner(const TextureHandleOwner& owner,int environment) {
    if(impl_->sharedTextures && impl_->sharedTextures!=&owner)
        throw std::runtime_error("Shared GS renderer texture owner changed");
    impl_->sharedTextures=&owner;
    for(std::size_t i=0;i<impl_->inputs.size();++i)impl_->prepareShared(impl_->inputs[i],impl_->preparation[i]);
    if(environment>=0) {
        ResourceTextureSlot slot;slot.handle=environment;slot.handle_serial=owner.handle_serial(environment);
        slot.record=owner.record_of(environment);slot.record_serial=owner.record_serial(slot.record);
        slot.image=owner.image(environment);
        impl_->environmentTex0=impl_->sourceTex0(slot,0);
        impl_->environmentHandle=environment;
    }
}
void SceneRenderer::write_texture_sampling_observation(std::ostream& out) const {
    out<<impl_->textureSamplingObservation<<'\n';
    if(!out)throw std::runtime_error("Shared GS texture observation write failed");
}
void SceneRenderer::set_material_environment(const Texture& image) {
    if(!image.width||!image.height||image.width>1024||image.height>1024||
       image.rgba.size()!=std::size_t(image.width)*image.height*4)
        throw std::runtime_error("Invalid owned material environment image");
    impl_->materialEnvironmentRegion={.5f/1024,.5f/1024,(float(image.width)+.5f)/1024,(float(image.height)+.5f)/1024};
    auto gpu=impl_->uploadTexture(image);
    if(impl_->materialEnvironment) {
        const auto index=*impl_->materialEnvironment;
        impl_->textures.at(index)=image;impl_->gpuTextures[index]=gpu;
    } else {
        impl_->materialEnvironment=impl_->textures.size();impl_->textures.push_back(image);
        [impl_->gpuTextures addObject:gpu];
    }
}
void SceneRenderer::bind_map_effects(const MapEffects* effects){impl_->mapEffects=effects;}
void SceneRenderer::bind_effect_submissions(const EffectSubmissions* effects){impl_->effectSubmissions=effects;}
SceneRenderer::PreparedFrame SceneRenderer::prepare(const Matrix4& view,const Float4& projection,const AttachedEffects* effects,const Float4& camera,float farClip,const Float4& cameraPosition,const FieldProjectionFrame* fieldProjection,float shadowFactor,bool restoreShadowAlpha){return impl_->prepare(view,projection,effects,camera,farClip,cameraPosition,fieldProjection,shadowFactor,restoreShadowAlpha);}
SceneRenderer::PreparedFrame SceneRenderer::prepare_empty(const FieldProjectionFrame& projection){return impl_->prepareEmpty(projection);}
SceneRenderer::SubmittedFrame SceneRenderer::submit_prepared(const PreparedFrame& frame,const SceneUiPass* ui,const SceneGpuPass* pass,std::optional<std::array<std::uint8_t,4>> clear,float depthClear){return impl_->submitPrepared(frame,ui,pass,clear,depthClear);}
Texture SceneRenderer::render_prepared(const PreparedFrame& frame,const SceneUiPass* ui,const SceneGpuPass* pass,std::optional<std::array<std::uint8_t,4>> clear,float depthClear){return submit_prepared(frame,ui,pass,clear,depthClear).wait();}
Texture SceneRenderer::render(const Matrix4& view,const Float4& projection,const AttachedEffects* effects,const Float4& camera,float farClip,const Float4& cameraPosition,const FieldProjectionFrame* fieldProjection,float shadowFactor,const SceneUiPass* ui){
    if(ui&&!fieldProjection)throw std::runtime_error("Scene UI requires GS field projection depth");
    return render_prepared(prepare(view,projection,effects,camera,farClip,cameraPosition,fieldProjection,shadowFactor),ui);
}
const std::vector<FieldShadowSubmission>& SceneRenderer::shadow_submissions() const{return impl_->shadowFrame.submissions;}
std::size_t SceneRenderer::placements() const{return impl_->inputs.size()-impl_->actorCount;}
std::size_t SceneRenderer::actors() const{return impl_->actorCount;}
std::size_t SceneRenderer::omitted_effects() const{return impl_->omittedEffects;}
}
