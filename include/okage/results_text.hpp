#pragma once
#include "okage/assets.hpp"
#include <array>
#include <string>

namespace okage {
class ResultsTextData {
public:
    explicit ResultsTextData(Bytes);
    std::string money(std::int32_t value) const;
    std::string money_summary(std::int32_t value) const;
private:
    std::array<std::string,5> formats_;
    std::string render(std::int32_t value,unsigned final_format) const;
};
}
