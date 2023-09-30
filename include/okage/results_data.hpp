#pragma once
#include "okage/assets.hpp"
#include <array>
#include <span>

namespace okage {
struct ResultsUiDefinition {
    std::array<std::uint32_t,28> words{};
    unsigned type() const {return words[24];}
    unsigned glyph_capacity() const {return words[25];}
    unsigned sprite_capacity() const {return words[26];}
};

struct ResultsAnimationRecord {
    std::array<std::uint32_t,4> words{};
};

class ResultsUiData {
public:
    explicit ResultsUiData(Bytes);
    const ResultsUiDefinition& definition() const {return definition_;}
    const std::array<std::uint32_t,16>& widget_ids() const {return widget_ids_;}
    const std::array<ResultsAnimationRecord,49>& animation_records() const {return animation_records_;}
    std::span<const ResultsAnimationRecord> animation_table(unsigned index) const;
    const std::array<std::array<std::uint32_t,4>,2>& stream_constants() const {return stream_constants_;}
private:
    ResultsUiDefinition definition_;
    std::array<std::uint32_t,16> widget_ids_{};
    std::array<ResultsAnimationRecord,49> animation_records_{};
    std::array<std::array<std::uint32_t,4>,2> stream_constants_{};
};
}
