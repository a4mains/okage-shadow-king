#pragma once
#include "okage/shadow.hpp"
#include "okage/ui_window.hpp"

namespace okage {
struct UiDrawState {
    std::int32_t background_alpha_top=0,background_alpha_bottom=0; //200b58/5c.
};
enum class UiDrawControl { Begin, CullModeZero, BeginGif, EndGif, FlushVu,
                           End, Queue, QueueFontUploads };
struct UiDrawRegister { std::uint32_t index; std::uint64_t value; };
// Resolve against the actual native render target, never a captured VRAM base.
struct UiDrawCurrentFramebuffer {};
struct UiDrawProjection { ShadowScreenParameters value; };
struct UiDrawMatrix { Matrix4 value; };
struct UiDrawVectors {
    enum class Kind { Auxiliary, Colors } kind;
    unsigned first;
    std::vector<Float4> values;
};
enum class UiDrawPacketFamily { Glyph, SpriteHeader, SpriteElement };
struct UiDrawReplay {
    UiDrawPacketFamily family;
    unsigned window,group,side,element,history,pass;
    // Snapshot at submission. Later layout/cache replacement cannot change it.
    std::shared_ptr<const UiNativePacket> packet;
};
using UiDrawCommand=std::variant<UiDrawControl,UiDrawRegister,
    UiDrawCurrentFramebuffer,UiDrawProjection,UiDrawMatrix,UiDrawVectors,UiDrawReplay>;
class UiDrawList {
public:
    std::span<const UiDrawCommand> commands() const {return commands_;}
private:
    friend struct UiDrawBuilder;
    std::vector<UiDrawCommand> commands_;
};
UiDrawList build_ui_draw_list(std::span<const std::shared_ptr<UiWindow>,8>,
    UiDrawState&,const ShadowScreenParameters&,std::span<const float> sine);
}
