#include "reference_pixel.hpp"
#include <stdexcept>
int main() {
    // ReadImage32 transforms GS 0xAABBGGRR into 0xAARRGGBB in little-endian memory.
    for(const auto gs:{0x800000ffu,0x80ff0000u,0x80213b7du}) {
        const std::array<std::uint8_t,4> packed={std::uint8_t(gs>>16),std::uint8_t(gs>>8),std::uint8_t(gs),std::uint8_t(gs>>24)};
        const auto rgba=reference_rgba(packed.data());
        if(rgba!=std::array<std::uint8_t,4>{std::uint8_t(gs),std::uint8_t(gs>>8),std::uint8_t(gs>>16),255})
            throw std::runtime_error("Vulkan reference pixel conversion failed");
    }
}
