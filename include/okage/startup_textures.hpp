#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "okage/texture_handles.hpp"
#include "okage/texture_memory.hpp"

namespace okage {

enum class StartupCoverage : std::uint8_t {
    NotRun,
    // Exact through the pre-scheduler boundary; everything later is unmodeled.
    PreSchedulerOnly,
};

// Inputs are owned game data supplied by the caller (never captured VRAM).
struct StartupTextureInputs {
    // 64 bytes at ELF 0x2003f8 (the 16-entry font/glyph CLUT).
    std::vector<std::uint8_t> font_palette;
    // Raw FNT files in source order: IQ24 (flags 0x1000), BMUI (0x2000), BM (0).
    std::array<std::vector<std::uint8_t>, 3> fonts;
    // ENV_TEX.XPF members by authored name (31 names, see startup_effect_names()).
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)> read_effect;
    // libc rand() state word at 0x214ec8+0x58 (ELF initial value 1).
    std::uint32_t rand_state{1};
};

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

    void run_startup(const StartupTextureInputs& inputs);

    StartupCoverage coverage() const noexcept { return coverage_; }
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

private:
    void checkpoint(const char* step);
    TextureDescriptor allocate_layer_buffer(std::uint8_t cls, GsPixelStorage psm);

    Ps2TextureMemory memory_;
    TextureHandleOwner handles_;
    StartupCoverage coverage_{StartupCoverage::NotRun};
    bool started_{};
    std::array<StartupLayer, 3> layers_{};
    std::array<TextureDescriptor, 2> filter_cluts_{};
    std::vector<StartupCheckpoint> checkpoints_;
    int glyph_cache_handle_{-1}, noise_handle_{-1};
    std::vector<int> effect_handles_;
    std::array<int, 3> font_handles_{-1, -1, -1};
    std::uint32_t rand_state_after_noise_{};
};

void write_startup_texture_report(const StartupTextureOwner& owner, const std::filesystem::path& dir);

std::vector<std::uint8_t> read_elf_va(std::span<const std::uint8_t> elf, std::uint32_t va, std::size_t size);

} // namespace okage
