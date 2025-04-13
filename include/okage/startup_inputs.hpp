#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace okage {

struct StartupInputs {
    // 64 bytes at ELF 0x2003f8 (NATIVE_STARTUP/FONTCLUT.BIN).
    std::vector<std::uint8_t> font_palette;
    // Raw FNT files: IQ24, BMUI, BM (source order).
    std::array<std::vector<std::uint8_t>, 3> fonts;
    // ENV_TEX.XPF members in authored order (startup_effect_names()).
    std::array<std::vector<std::uint8_t>, 31> effects;
    // CMNDATA/2D/TES.EFD.
    std::vector<std::uint8_t> efd;
};

// Every failure message names the file and says how to regenerate the data.
class StartupInputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

StartupInputs load_startup_inputs(const std::filesystem::path& game_data_root);

} // namespace okage
