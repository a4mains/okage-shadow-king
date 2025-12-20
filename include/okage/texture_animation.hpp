#pragma once
#include "okage/model.hpp"
#include <string_view>
#include <cstdint>
#include <functional>
#include <optional>

namespace okage {
struct TextureAnimation {
    std::vector<std::int16_t> groups;
    std::vector<std::string> variants; // One-based XMT choices; zero restores.
};
TextureAnimation read_xta(std::string_view text);

class TextureBindings {
public:
    TextureBindings(const ModelFile& model, TextureAnimation animation);
    void select(std::size_t group, std::size_t variant);
    void restore();
    const std::vector<std::int32_t>& images() const { return current_; }
    const std::vector<std::string>& names() const { return names_; }
    const TextureAnimation& animation() const { return animation_; }
    using SourceRegisterResolver = std::function<std::uint64_t(int image, int palette)>;
    void configure_source_registers(const void* owner_identity, SourceRegisterResolver) const;
    bool source_registers_configured() const noexcept { return source_.has_value(); }
    const std::vector<std::optional<std::uint64_t>>& source_registers() const;
private:
    TextureAnimation animation_;
    std::vector<std::string> names_;
    std::vector<std::int32_t> original_, current_, original_palettes_;
    struct SourceRegisters {
        const void* owner_identity;
        SourceRegisterResolver resolve;
        std::vector<std::optional<std::uint64_t>> original, current;
    };
    mutable std::optional<SourceRegisters> source_;
    bool missed_source_callback_=false;
    std::size_t base_count_;
};
}
