#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "okage/effect.hpp"
#include "okage/enemy.hpp"
#include "okage/texture_handles.hpp"
#include "okage/resource_textures.hpp"
#include "okage/model_groups.hpp"
#include "okage/texture_memory.hpp"

namespace okage {

enum class StartupCoverage : std::uint8_t {
    NotRun,
    // Exact through the pre-scheduler boundary; everything later is unmodeled.
    PreSchedulerOnly,
};

enum class ResourceStage : std::uint8_t {
    PreScheduler,       // Startup/RNG prefix complete (before the call at 0x100618)
    Scheduled,
    LogosConstructed,
    LogosRetired,
    TitleConstructed,
    // Retire (the title-retirement input contract): the title from the first 1296f8 to entry of 124548.
    TitleActive,
    TitleRemovalRequested,
    TitleResourcesRetired,
                            // image list (1b5fd0 -> 1b2240); then the mode-8 destructor 12c8d8 ran
    TitleWrappersFreed,
    SuccessorEntry,
    // Restore (the Restore input contract): empty-card Restore, Cancel, back to the title.
    RestoreActive,          // 124548(mode 10) ran: 1243c8 -> 11fb78 bm_if atlas owned by wrapper+0x30
    RestoreManagerDead,
    RestoreResourcesRetired,// D+3: 101058 (releases nothing) then wrapper dtor 124448 -> 1cca30(wrapper+0x30)
    RestoreWrappersFreed,
    NameEntryActive,            // 124548(mode 9) ran: 1243c8 -> 11fb78 bm_if atlas owned by wrapper+0x30, type-9 manager
    NameEntryManagerDead,
    NameEntryCommandPublished,
    NameEntryResourcesRetired,  // D+3: 101058 (releases nothing) then 124628 -> 124448 -> 1cca30(wrapper+0x30)
    NameEntryWrappersFreed,
    ChapterActive,
    ChapterModeRequested,
    ChapterResourcesRetired,
    ChapterWrappersFreed,       // resource node then mode-2 node freed
    FieldConstructorEntry,
    FieldConstructed,           // 133830 returned: 1151b8 resource 221980 at +0x3c, 1bafd0(1), 178578(132688, node, scene, 0)
    FieldWorkerEntered,         // 132688 entry: +0x38/+0x30/+0x34 = 0, 1b8348(1)
    FieldAtlasLoaded,           // 131d88 1cd7a8(0), 131d90 186a40(1), 131cd0 -> 11fb10(bm_if[j], 27): manager+0x60, 24f730
    FieldTexenvLoaded,          // 100c60(resource, texenv.xim): 18f7a8 -> resource+0x48, 183ff0 TEX0 332430 / region 332450
    FieldRuwLoaded,             // 1a9190("fld.ruw", 0): RUW slot 0
    FieldUiPublished,           // 131e7c: manager+0x34 = 1 (the prefix is available; loading is NOT complete: +0x30 stays 0)
    FieldWorldLoading,
    FieldWorldComplete,
    FieldMapCommitted,
    FieldActive,
    FieldResourcesRetired,
    FieldWrappersFreed,
    Unmodeled,
                        // or game over): this lifecycle history cannot claim completeness afterward
};
const char* to_string(ResourceStage);
// What the memory bytes/masks mean, separate from the lifecycle above.
enum class MemoryContentCoverage : std::uint8_t {
    UploadsExact,
    DrawTargetsInvalidated,
                              // unknown; every other known nibble is an accounted upload
};
const char* to_string(MemoryContentCoverage);

// Inputs are owned game data supplied by the caller (never captured VRAM).
struct StartupTextureInputs {
    // 64 bytes at ELF 0x2003f8 (the 16-entry font/glyph CLUT).
    std::vector<std::uint8_t> font_palette;
    // Raw FNT files in source order: IQ24 (flags 0x1000), BMUI (0x2000), BM (0).
    std::array<std::vector<std::uint8_t>, 3> fonts;
    // ENV_TEX.XPF members by authored name (31 names, see startup_effect_names()).
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)> read_effect;
    bool record_observations{};
};

enum class StartupRngPhase : std::uint8_t { Noise, EffectGrid, EffectTemplate, Session,
                                            RestoreUiGlyph, RestoreUiIcon, RestoreUiSelect,
                                            NameUiGlyph, NameUiIcon, NameUiSelect };
const char* to_string(StartupRngPhase);
struct StartupRandomDraw {
    StartupRngPhase phase{};
    std::uint32_t state{};  // generator state after the draw
    std::uint32_t value{};  // returned value (state & 0x7fffffff)
};
class StartupRandomStream {
public:
    static constexpr std::size_t log_limit = 1u << 20;
    StartupRandomStream(GameRandom& random, bool record) : random_(&random), record_(record) {}
    std::uint32_t draw();
    void enter(StartupRngPhase phase) noexcept { phase_ = phase; }
    StartupRngPhase phase() const noexcept { return phase_; }
    std::uint64_t draws() const noexcept { return draws_; }
    std::uint32_t state() const noexcept { return random_->state; }
    const std::vector<StartupRandomDraw>& log() const noexcept { return log_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
private:
    GameRandom* random_;
    bool record_;
    StartupRngPhase phase_{StartupRngPhase::Noise};
    std::uint64_t draws_{}, dropped_{};
    std::vector<StartupRandomDraw> log_;
};

struct StartupTimelineEvent {
    enum class Kind : std::uint8_t { Allocation, Upload, PhaseBegin, PhaseEnd } kind{};
    std::uint32_t allocation{};      // allocation id (Allocation/Upload)
    std::uint32_t base_block{};      // destination TBP (Allocation/Upload)
    const char* phase{};             // PhaseBegin/PhaseEnd label
    std::uint64_t draws{};           // RNG draws before this event
    std::uint32_t rng_state{};       // generator state at this event
};

using StartupEffectPreparation = std::function<void(StartupRandomStream&)>;

// 0x221bf8 table, authored order.
const std::array<const char*, 31>& startup_effect_names();

struct StartupLayer {
    TextureDescriptor color;
    std::optional<TextureDescriptor> depth;
};

// One entry per top-level step so comparisons can localize a divergence.
struct StartupCheckpoint {
    const char* step{};
    std::uint32_t allocations{};
    std::uint32_t live_handles{};
    std::uint32_t available{};
    TextureMemoryIdentity identity{};
};

class StartupTextureOwner {
public:
    static constexpr std::uint32_t expected_allocations = 79;
    static constexpr std::uint32_t expected_handles = 36;

    StartupTextureOwner();
    // Nonmoving and noncopyable: handles() holds a pointer to memory().
    StartupTextureOwner(const StartupTextureOwner&) = delete;
    StartupTextureOwner& operator=(const StartupTextureOwner&) = delete;
    StartupTextureOwner(StartupTextureOwner&&) = delete;
    StartupTextureOwner& operator=(StartupTextureOwner&&) = delete;
    ~StartupTextureOwner();

    void run_startup(const StartupTextureInputs& inputs, GameRandom& random,
                     const StartupEffectPreparation& prepare_effects);

    StartupCoverage coverage() const noexcept { return coverage_; }

    // ---- Title lifecycle after the prefix (same memory and handle owner) ----
    ResourceStage stage() const noexcept { return stage_; }
    MemoryContentCoverage memory_coverage() const noexcept { return memory_coverage_; }
    void begin_scheduled_frames();
    // 12c350(0): logo construction may begin (stage check only).
    void begin_logo_construction();
    // 0x1b2348: a file-read failure (nullopt) appends nothing and returns -1; an
    // 18f7a8 load failure appends -1. Returns the handle (or -1).
    int append_image(const std::optional<std::vector<std::uint8_t>>& file);
    // 0x1b2240: retire every image-list handle in insertion order, then reset.
    void retire_image_list();
    void begin_title_construction();
    XmdLoadResult load_and_bind_model(const std::string& model, std::span<const std::string> names,
                                      const TextureHandleOwner::FileReader& reader);
    void finish_title_construction();
    void begin_title_updates();
    // A named observation point inside a title stage (e.g. "stable-menu"); no state change.
    void note_checkpoint(const char* name);
    void note_effect_create(int slot, unsigned shape);
    void note_effect_bind(int slot, unsigned element, int handle);
    void note_effect_removal_request(int slot);
    // 0x102218 armed 0x200 on the requested job (same traversal): reclaim pending. State only.
    void note_effect_reclaim_armed(int slot);
    // After reclamation the job memory is gone: shape unknown (null) and no element handles.
    struct EffectObservation { int slot{-1}; const char* state{}; std::vector<int> element_handles; bool shape_known{true}; };
    const std::optional<EffectObservation>& effect106() const noexcept { return effect106_; }
    void note_mode_request(unsigned mode, unsigned selector, unsigned arg);
    void note_mode_resource_retire(unsigned selector, unsigned mode = 8);  // also logs 101240 -> 103640(-2)
    void note_mode_destroy(unsigned selector, unsigned mode = 8);
    // One 0x1bdaf0 node free: the resource node (the native session object's reset) or the mode node
    // (GameModeParent releasing its dead allocation).
    void note_mode_wrapper_free(unsigned selector, unsigned mode, bool mode_node);
    // Session generator draws after the prefix (GameRandom::observe), in order, with the native frame.
    void note_session_draw(std::uint32_t state, std::uint32_t frame, unsigned site = 0);  // site: RngSite value
    const std::vector<std::uint32_t>& session_draw_frames() const noexcept { return session_draw_frames_; }
    int construct_restore(const std::vector<std::uint8_t>& atlas);
    int restore_atlas() const noexcept { return wrapper_atlas_; }
    // Restore resource child's first tick: 0x102218 frees the reclaim-pending water job (host only).
    void note_effect_reclaim(int slot);
    // D: 11e690 state -2, 1248d8(4,0,0), manager dead; window destroyed (fonts are not freed).
    void restore_manager_death();
    // D+3: resource child 101058 (resource-retire, 103640(-2)), then entry of the wrapper dtor
    // 124628/124448 (destroy row); release_restore_atlas() is its 1cca30(+0x30) tail.
    void retire_restore_resources();
    void release_restore_atlas();  // logs the 124628 destroy row (entry), then 1cca30(+0x30)
    void note_ui(const std::string& op, int value);
    void note_manager_free();
    // free_restore_wrappers() in two steps around the wrapper-free checkpoint (entry of the
    // 0x1bdaf0 that frees the wrapper node): the resource node row, then the wrapper node row.
    void free_restore_resource_node();
    // D+4: resource node then wrapper node freed.
    void free_restore_wrappers();
    bool second_title() const noexcept { return second_title_; }
    int construct_name_entry(const std::vector<std::uint8_t>& atlas);
    int name_atlas() const noexcept { return wrapper_atlas_; }
    // D: the native type-9 manager reached phase -3 (death flag); window destroyed in the same traversal.
    void name_manager_death();
    // 1248d8(a, b, c): the published game command (observed from the native Session's pending command).
    void note_command(std::uint32_t a, std::uint32_t b, std::uint32_t c);
    // D+2: 124630 phase 1 published {14,1,0} and requested {2,72,0} (rows from the native request).
    void name_entry_requested(unsigned mode, unsigned selector, unsigned arg);
    // D+3: 101058 of the mode-9 resource child, then entry of 124628/124448; release_name_atlas() is its
    // 1cca30(wrapper+0x30) tail.
    void retire_name_entry_resources();
    void release_name_atlas();
    void free_name_entry_resource_node();
    void free_name_entry_wrappers();
    int construct_chapter(unsigned chapter, const std::vector<std::uint8_t>& image);
    // 133a98 phase 19 after the fade completed: 1248d8(0,0,0) then 124930(root, 2, scene, entrance).
    void chapter_requested(unsigned mode, unsigned scene, unsigned entrance);
    void retire_chapter_resources();
    void destroy_chapter_mode();
    void free_chapter_resource_node();
    void free_chapter_wrappers();
    // Entry of the second 133830 with command 0 (the Newgame stop).
    void enter_field_constructor(unsigned scene, unsigned entrance, unsigned command = 0);
    // ---- Field: the ordinary-field common prefix  ----
    using OwnedBytes = std::shared_ptr<const std::vector<std::uint8_t>>;
    // Source-visible state of the prefix, the field manager (the mode-2 node) and its resource child.
    struct FieldPrefixState {
        bool resource_created{};         // 1151b8(node, 221980, 0x50) -> node+0x3c
        std::uint32_t switch_337e70{};
        bool job_queued{};               // 178578(132688, node, scene, 0)
        unsigned scene{}, entrance{};
        unsigned constructor_command{};
        int w30{}, w34{};                // 132688 clears both; 131e7c sets +0x34; +0x30 only after 131d28 returns
        int manager_60{};
        int resource_48{-1};
        int atlas_global_24f730{-1};
        std::uint32_t clut_class_2138c4{};
        std::uint64_t tex0_332430{};
        std::array<std::uint32_t, 4> region_332450{};
        bool ruw_slot0_live{};           // 1a9190("fld.ruw", 0)
        std::uint64_t ruw_bytes{};
        std::string ruw_sha256;
    };
    const FieldPrefixState& field_prefix() const noexcept { return field_; }
    void construct_field_mode(unsigned scene, unsigned entrance);
    // 132688 entry (the job ran): the publication words are cleared, 1b8348(1).
    void enter_field_worker();
    int load_field_atlas(const OwnedBytes& atlas);
    // 131d14 executes after the observed 11fb10 return checkpoint.
    void bind_field_atlas();
    // 100c60(resource, "texenv.xim"): 18f7a8 -> resource+0x48, then 183ff0(handle, 0, 0, 0, height, width).
    int load_field_texenv(const OwnedBytes& texenv);
    // 1a9190("fld.ruw", 0).
    void load_field_ruw(const OwnedBytes& ruw);
    // 131e7c: manager+0x34 = 1, before world resources and worker completion.
    void publish_field_ui();
    // 1bbac8 -> 186a50(1), before authored map-definition model loads.
    void begin_field_world();
    using WorldModelTextures = ModelResourceGroup;
    const WorldModelTextures& load_field_model(const std::string& name, std::span<const std::string> names,
        const TextureHandleOwner::FileReader& reader, const std::function<void()>& buffer_return,
        unsigned model_count = 1);
    void load_field_sidecar(const OwnedBytes& bytes, const TextureHandleOwner::FileReader& reader,
                           const std::function<void()>& image_return);
    void complete_field_world(); // 132688 +0x30 store, only after all worker work succeeds
    void commit_field_map();
    void activate_field();       // 132720 returned
    const auto& world_models() const noexcept { return models_.groups(); }
    ModelGroupOwner& models() noexcept { return models_; }
    const ModelGroupOwner& models() const noexcept { return models_; }
    void retire_field_resources();
    void retire_field_manager();   // 133a38: manager atlas, RUW slots
    void free_field_resource_node();
    void free_field_wrappers();
    const ResourceTextureOwner& resources() const noexcept { return resources_; }
    unsigned chapter_scene() const noexcept { return chapter_scene_; }
    // 0x12a2d4 -> 124930(root, mode): the mode request in the frame the fade reaches zero.
    void request_title_successor(unsigned mode);
    void retire_title_resources();
    // N+1 after 101058: the mode-8 destructor (12c8d8 -> 12dd50(1), a deferred theme fade).
    void destroy_title_mode();
    // N+2: resource node then mode-8 node unlinked and freed.
    void free_title_resource_node();  // N+2, the native session object's reset
    void free_title_wrappers();
    // N+3: root phase 1 allocated and linked the successor node; stop at 124548 entry.
    void enter_successor(unsigned mode);
    unsigned successor_mode() const noexcept { return successor_mode_; }
    void mark_unmodeled(const char* reason);
    const std::string& unmodeled_reason() const noexcept { return unmodeled_reason_; }
    const std::vector<int>& image_list() const noexcept { return image_list_; }
    // Binding table of the last bound model (handles[i] for name index i; -7/-1 past the tail).
    const std::vector<int>& model_binding() const noexcept { return model_binding_; }
    const std::vector<OwnedBytes>& model_slot_bytes() const noexcept { return model_slot_bytes_; }
    bool started() const noexcept { return started_; }
    Ps2TextureMemory& memory() noexcept { return memory_; }
    const Ps2TextureMemory& memory() const noexcept { return memory_; }
    TextureHandleOwner& handles() noexcept { return handles_; }
    const TextureHandleOwner& handles() const noexcept { return handles_; }

    // Colour/depth/capture attachments: raw VRAM descriptors, not handles.
    const std::array<StartupLayer, 3>& layers() const noexcept { return layers_; }  // 2 colour buffers, capture
    // Null-source filter CLUTs (allocated, never uploaded, never retained).
    const std::array<TextureDescriptor, 2>& filter_cluts() const noexcept { return filter_cluts_; }
    const std::vector<StartupCheckpoint>& checkpoints() const noexcept { return checkpoints_; }
    // Handles in source order: glyph cache, noise, 31 effects, 3 fonts.
    int glyph_cache_handle() const noexcept { return glyph_cache_handle_; }
    int noise_handle() const noexcept { return noise_handle_; }
    const std::vector<int>& effect_handles() const noexcept { return effect_handles_; }
    const std::array<int, 3>& font_handles() const noexcept { return font_handles_; }
    std::uint32_t rand_state_after_noise() const noexcept { return rand_state_after_noise_; }
    const char* failed_phase() const noexcept { return failed_phase_; }
    const std::vector<StartupRandomDraw>& rng_log() const noexcept { return rng_log_; }
    const std::vector<StartupTimelineEvent>& timeline() const noexcept { return timeline_; }
    std::uint64_t rng_draws() const noexcept { return rng_draws_; }
    static constexpr std::size_t lifecycle_limit = 1u << 20;
    const std::vector<std::string>& lifecycle() const noexcept { return lifecycle_; }
    std::uint64_t lifecycle_dropped() const noexcept { return lifecycle_dropped_; }

private:
    void checkpoint(const char* step);
    void note_allocation(TextureAllocationId id);
    void note_upload(TextureAllocationId id);
    void note_phase(StartupTimelineEvent::Kind kind, const char* phase);
    void sync();
    void lifecycle_push(std::string json);
    void lifecycle_alloc(const GsTextureAllocation& a);
    void lifecycle_ref(TextureAllocationId id, int delta);
    TextureDescriptor allocate_layer_buffer(std::uint8_t cls, GsPixelStorage psm);

    Ps2TextureMemory memory_;
    TextureHandleOwner handles_;
    ResourceTextureOwner resources_;
    ModelGroupOwner models_;
    std::optional<ModelRangeLease> last_world_model_, title_model_;
    StartupCoverage coverage_{StartupCoverage::NotRun};
    bool started_{};
    std::array<StartupLayer, 3> layers_{};
    std::array<TextureDescriptor, 2> filter_cluts_{};
    std::vector<StartupCheckpoint> checkpoints_;
    int glyph_cache_handle_{-1}, noise_handle_{-1};
    std::vector<int> effect_handles_;
    std::array<int, 3> font_handles_{-1, -1, -1};
    std::uint32_t rand_state_after_noise_{};
    const char* phase_{"not started"};
    const char* failed_phase_{};
    StartupRandomStream* stream_{};
    bool record_{};
    std::vector<StartupRandomDraw> rng_log_;
    std::vector<StartupTimelineEvent> timeline_;
    std::uint64_t rng_draws_{};
    std::vector<std::string> lifecycle_;
    std::uint64_t lifecycle_dropped_{};
    ResourceStage stage_{ResourceStage::PreScheduler};
    MemoryContentCoverage memory_coverage_{MemoryContentCoverage::UploadsExact};
    std::vector<int> image_list_;
    std::vector<int> model_binding_;
    std::vector<OwnedBytes> model_slot_bytes_;
    std::string unmodeled_reason_;
    unsigned successor_mode_{};
    int wrapper_atlas_{-1};  // wrapper+0x30 of the live mode-9/10 wrapper (Restore or name entry)
    FieldPrefixState field_;
    OwnedBytes field_atlas_bytes_, field_texenv_bytes_, field_ruw_bytes_;
    unsigned chapter_scene_{};
    bool second_title_{};
    std::vector<std::uint32_t> session_draw_frames_;
    std::optional<EffectObservation> effect106_;
    void require_modeled(const char* operation) const;
    void title_event(ResourceStage from, ResourceStage to, std::string json);
    void require_stage(ResourceStage expected, const char* operation) const;
    void advance(ResourceStage next);
};

void write_startup_texture_report(const StartupTextureOwner& owner, const std::filesystem::path& dir);
void write_session_resource_report(const StartupTextureOwner& owner, const std::filesystem::path& dir);
enum class CheckpointSchema : std::uint8_t { Retire, Restore, Newgame, Field, FieldWorld };
void write_title_checkpoint_report(const StartupTextureOwner& owner, const std::filesystem::path& dir,
                                   const std::string& checkpoint, std::uint32_t session_rng_state,
                                   CheckpointSchema schema = CheckpointSchema::Retire);

void write_effect_preparation_report(const EffectTemplatePreparation& prepared, const EffectPreparationTrace& trace,
                                     const std::filesystem::path& dir);

std::vector<std::uint8_t> read_elf_va(std::span<const std::uint8_t> elf, std::uint32_t va, std::size_t size);

} // namespace okage
