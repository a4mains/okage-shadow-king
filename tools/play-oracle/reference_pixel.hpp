#pragma once
#include <array>
#include <cstdint>
inline std::array<std::uint8_t,4> reference_rgba(const std::uint8_t* bgra) {
    return {bgra[2],bgra[1],bgra[0],255};
}
