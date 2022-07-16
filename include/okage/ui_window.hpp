#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace okage {
enum class UiStreamReference { MenuTransform1f9c00, MenuTransform1f9c2c };
struct UiStreamCommand {
    std::uint32_t opcode;
    std::variant<std::uint32_t,UiStreamReference> argument;
};
struct UiStreamText { std::vector<std::uint8_t> bytes; }; // Explicit NUL required.
struct UiWindowStream {
    std::uint32_t binding_id;
    std::vector<std::variant<UiStreamCommand,UiStreamText>> entries;
};

class UiWindowBuffer {
public:
    std::size_t size() const { return bytes_.size(); }
    std::optional<std::uint8_t> known_byte(std::size_t offset) const;
    std::uint32_t word(std::size_t offset) const;
private:
    friend class UiWindow;
    UiWindowBuffer(std::size_t bytes,bool cleared);
    void put_word(std::size_t offset,std::uint32_t value);
    std::vector<std::uint8_t> bytes_;
    std::vector<bool> known_;
};

class UiWindow {
public:
    static constexpr std::size_t scalar_bytes=0x6ec0;
    using AtlasTuple=std::array<std::uint32_t,4>;
    UiWindow(unsigned capacity_a,unsigned capacity_b,unsigned slot,
             std::shared_ptr<const UiWindowStream> stream);
    UiWindow(const UiWindow&)=delete;
    UiWindow& operator=(const UiWindow&)=delete;
    UiWindow(UiWindow&&)=default;
    UiWindow& operator=(UiWindow&&)=default;

    // Byte offsets, aligned/in range. Unknown includes guest pointer lanes,
    // allocator accounting, and the uninitialized stack W copied to 0x53c.
    std::optional<std::uint32_t> known_word(std::size_t offset) const;
    std::uint32_t word(std::size_t offset) const;
    const UiWindowStream& stream() const { return *stream_; }
    const std::shared_ptr<const void>& texture_owner() const { return texture_; }
    const UiWindowBuffer& flags() const { return flags_; }
    const UiWindowBuffer& glyph_buffer(unsigned index) const;
    const UiWindowBuffer& sprite_buffer(unsigned index) const;
    const UiWindowBuffer& glyph_cache(unsigned index,unsigned side) const;
    const UiWindowBuffer& sprite_cache(unsigned index,unsigned side) const;
    const UiWindowBuffer& extra_cache(unsigned index) const;

    void set_opacity_tween(float increment,float current,
                           std::uint32_t lower,std::uint32_t upper);
    float opacity() const;
    void set_transform_xyz(const std::array<std::uint32_t,3>& first_bits,
                           const std::array<std::uint32_t,3>& second_bits);

    bool configure(const std::array<std::uint32_t,28>& authored,
                   std::span<const AtlasTuple> atlas,
                   std::shared_ptr<const void> texture);
private:
    void put(std::size_t offset,std::uint32_t value);
    std::array<std::uint32_t,scalar_bytes/4> words_{};
    std::bitset<scalar_bytes/4> known_;
    std::shared_ptr<const UiWindowStream> stream_;
    std::shared_ptr<const void> texture_;
    UiWindowBuffer flags_;
    std::array<UiWindowBuffer,2> glyph_,sprite_;
    std::vector<std::array<UiWindowBuffer,2>> glyph_cache_,sprite_cache_;
    std::array<UiWindowBuffer,3> extra_;
};
}
