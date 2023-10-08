#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace okage {
class RuwResource;
struct UiLayoutSprite;
struct Font;
enum class UiPacketOperationKind { Positions, Colors, UV, Register, Texture,
                                   Primitive, IndexedVertex, CompilerBoundary,
                                   AuxiliaryVectors };
struct UiPacketOperation {
    UiPacketOperationKind kind;
    std::uint32_t source_helper;
    std::array<std::uint64_t,4> arguments{};
    std::vector<std::array<float,4>> values; // UV uses first2 lanes only.
    std::optional<bool> first_positions_upload=std::nullopt;
};
struct UiPacketBuildContext {
    std::shared_ptr<const std::vector<Font>> fonts;
    std::int32_t background_alpha_top=0,background_alpha_bottom=0; //200b58/5c.
};
struct UiNativePacket {
    std::vector<UiPacketOperation> operations;
    //1a6908 per-slot subpackets; nullopt means source5ec0 entry==-1.
    std::array<std::optional<std::vector<UiPacketOperation>>,16> elements;
    std::shared_ptr<const std::vector<Font>> fonts;
    std::shared_ptr<const void> window_texture;
};
struct UiLayoutContext;
struct UiPlacementContext {
    std::int32_t layer;
    std::span<const float> sine;
    std::function<std::uint32_t()> random; // Real1f14b0 contract: result<=7fffffff.
};
enum class UiStreamReference { MenuTransform1f9c00, MenuTransform1f9c2c,
                               RuwMetrics200b78, RuwClip200b88 };
struct UiWidgetReference {
    std::shared_ptr<RuwResource> resource;
    std::size_t group,index;
};
struct UiStreamCommand {
    std::uint32_t opcode;
    std::variant<std::uint32_t,UiStreamReference,UiWidgetReference> argument;
};
struct UiStreamText { std::vector<std::uint8_t> bytes; }; // Explicit NUL required.
struct UiWindowStream {
    std::uint32_t binding_id;
    std::vector<std::variant<UiStreamCommand,UiStreamText>> entries;
    bool embedded_ruw=false;
};

class UiWindowBuffer {
public:
    std::size_t size() const { return bytes_.size(); }
    std::optional<std::uint8_t> known_byte(std::size_t offset) const;
    std::uint32_t word(std::size_t offset) const;
private:
    friend class UiWindow;
    friend struct UiWindowSpriteTestAccess;
    friend struct UiWindowGlyphTestAccess;
    friend struct UiWindowPacketTestAccess;
    friend struct UiWindowDrawTestAccess;
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
    void reset_for_open();
    // 1a5488: change bit0x1000 only. No layout pass runs in this setter.
    void set_layout_enabled(bool enabled);
    //1a56d0: raw scalar write to4f8; registry lookup belongs to the caller.
    void set_transform_parameter(std::uint32_t value);
    void set_transform_xyz(const std::array<std::uint32_t,3>& first_bits,
                           const std::array<std::uint32_t,3>& second_bits);
    void advance_clock();
    void bind_ruw(std::shared_ptr<RuwResource>,std::span<const std::size_t> groups);
    void layout_mode2(UiLayoutContext);
    bool prepare_mode2(UiLayoutContext);

    bool place_sprite(unsigned group,unsigned slot,const UiLayoutSprite&);

    void place_glyph(unsigned group,unsigned slot,std::int32_t x,std::int32_t y,
                     std::int32_t width,std::int32_t height,std::span<const Font>,
                     const UiPlacementContext&);
    bool place_icon(unsigned group,unsigned slot,std::int32_t x,std::int32_t y,
                    std::int32_t depth,std::int32_t selector,const UiPlacementContext&);

    void reset_glyph_packets();
    void reset_sprite_packets();
    int build_glyph_packet(const UiPacketBuildContext&);
    void build_sprite_packet(const UiPacketBuildContext&);
    const UiNativePacket* glyph_packet(unsigned group,unsigned side) const;
    const UiNativePacket* sprite_packet(unsigned group,unsigned side) const;
    void build_background_packet();
    const UiNativePacket* background_packet() const {return background_packet_?&*background_packet_:nullptr;}

    bool configure(const std::array<std::uint32_t,28>& authored,
                   std::span<const AtlasTuple> atlas,
                   std::shared_ptr<const void> texture);
private:
    friend struct UiWindowClockTestAccess;
    friend struct UiWindowSpriteTestAccess;
    friend struct UiWindowGlyphTestAccess;
    friend struct UiWindowPacketTestAccess;
    friend struct UiWindowBackgroundTestAccess;
    friend struct UiWindowDrawTestAccess;
    friend struct UiDrawBuilder;
    void put(std::size_t offset,std::uint32_t value);
    std::array<std::uint32_t,scalar_bytes/4> words_{};
    std::bitset<scalar_bytes/4> known_;
    std::shared_ptr<const UiWindowStream> stream_;
    std::shared_ptr<const void> texture_;
    UiWindowBuffer flags_;
    std::array<UiWindowBuffer,2> glyph_,sprite_;
    std::vector<std::array<UiWindowBuffer,2>> glyph_cache_,sprite_cache_;
    std::array<UiWindowBuffer,3> extra_;
    std::map<std::pair<unsigned,unsigned>,UiNativePacket> glyph_packets_,sprite_packets_;
    std::optional<UiNativePacket> background_packet_;
};
}
