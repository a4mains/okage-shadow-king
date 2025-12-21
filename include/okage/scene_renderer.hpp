#pragma once
#include "okage/pose.hpp"
#include "okage/actor.hpp"
#include "okage/effect.hpp"
#include "okage/field_shadows.hpp"
#include "okage/scene_model_asset.hpp"
#include <filesystem>
#include <memory>
#include <functional>
#include <iosfwd>
#include <utility>
#ifdef __OBJC__
@protocol MTLCommandBuffer;
@protocol MTLTexture;
#endif
namespace okage {
class MapObjects;
struct UiGeometryList;
struct UiGeometryTexture;
struct UiMetalBaseline;
struct UiMetalTexture;
struct SceneUiPass {
    const UiGeometryList& geometry;
    const UiMetalBaseline& baseline;
    const std::function<UiMetalTexture(const UiGeometryTexture&)>& textures;
};
struct SceneGpuPass;
#ifdef __OBJC__
struct SceneGpuPass {
    std::function<std::function<void()>(id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>)> encode;
    bool before_deferred_effects=false;
};
#endif
struct FieldOrdinaryLighting;
void configure_scene_model_texture_source(const SceneModelAsset&,const TextureHandleOwner&);
struct SceneActorDescriptor {std::uint32_t flags;bool height_enabled;};
struct SceneActor {const FieldActor* actor;std::filesystem::path model_path;int alias=0;std::optional<SceneActorDescriptor> descriptor{};
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> texture_bytes{};
    std::vector<ResourceTextureSlot> texture_slots{};
    bool textures_owned=false;
    std::shared_ptr<const ModelFile> shadow_model{};
    bool shadow_owned=false;};
class SceneRenderer {
    struct Impl;
public:
    class PreparedFrame {
    public:
        PreparedFrame()=default;
    private:
        struct Data;
        std::shared_ptr<const Data> data_;
        friend struct Impl;
        friend class SubmittedFrame;
    };
    class SubmittedFrame {
    public:
        SubmittedFrame()=default;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] Texture wait() const;
        [[nodiscard]] Texture readback() const;
    private:
        struct State;
        std::shared_ptr<State> state_;
        explicit SubmittedFrame(std::shared_ptr<State> state):state_(std::move(state)){}
        friend struct Impl;
    };
    static void prepare_backend();
    // Diagnostic snapshot; no reset API and no scene/asset initialization.
    static std::size_t cached_backend_count();
    SceneRenderer();
    explicit SceneRenderer(const std::filesystem::path& map,std::span<const SceneActor> actors={},const MapObjects* objects=nullptr,
                           std::span<const std::shared_ptr<const SceneModelAsset>> map_assets={});
    ~SceneRenderer();
    void add_actor(const SceneActor&);
    void retire_instances();
    void load_effect_textures(const std::filesystem::path&);
    // Same textures from verified owned bytes (effect_texture_names order).
    using EffectTextureBytes = std::array<std::vector<std::uint8_t>, 31>;
    void load_effect_textures(const EffectTextureBytes&);
    std::uint32_t add_effect_texture(const Texture&);
    EffectTextureSizes effect_texture_sizes() const;
    void bind_map_effects(const MapEffects*);
    void bind_effect_submissions(const EffectSubmissions*);
    void set_effect_light_tint(const Float4&);
    void set_clear_rgb(std::array<std::uint8_t,3>);
    //337e0c from StageSettings.bucket_base; queue division remains source9.
    void set_sort_depth_shift(unsigned);
    void set_stage_environment(const FieldOrdinaryLighting&,const FieldShadowSettings&,const Float4& sky);
    void set_material_environment(const Texture&);
    void set_shared_texture_owner(const TextureHandleOwner&, int environment_handle = -1);
    void write_texture_sampling_observation(std::ostream&) const;
    PreparedFrame prepare(const Matrix4& view,const Float4& projection,const AttachedEffects* effects=nullptr,const Float4& camera_rotation={},float far_clip=1024,const Float4& camera_position={},const FieldProjectionFrame* field_projection=nullptr,float shadow_factor=1,bool restore_shadow_alpha=true);
    PreparedFrame prepare_empty(const FieldProjectionFrame&);
    SubmittedFrame submit_prepared(const PreparedFrame&,const SceneUiPass* ui=nullptr,const SceneGpuPass* between_scene_and_ui=nullptr,std::optional<std::array<std::uint8_t,4>> execution_clear=std::nullopt,float execution_depth_clear=1.f);
    Texture render_prepared(const PreparedFrame&,const SceneUiPass* ui=nullptr,const SceneGpuPass* between_scene_and_ui=nullptr,std::optional<std::array<std::uint8_t,4>> execution_clear=std::nullopt,float execution_depth_clear=1.f);
    Texture render(const Matrix4& view,const Float4& projection,const AttachedEffects* effects=nullptr,const Float4& camera_rotation={},float far_clip=1024,const Float4& camera_position={},const FieldProjectionFrame* field_projection=nullptr,float shadow_factor=1,const SceneUiPass* ui=nullptr);
    const std::vector<FieldShadowSubmission>& shadow_submissions() const;
    std::size_t placements() const;
    std::size_t omitted_effects() const;
    std::size_t actors() const;
private:
    std::unique_ptr<Impl> impl_;
};
}
