#include "okage/texture_animation.hpp"
#include <charconv>
#include <stdexcept>

namespace okage {
TextureAnimation read_xta(std::string_view text) {
    if (text.size()>1024*1024) throw std::runtime_error("XTA exceeds size limit");
    TextureAnimation result;
    std::size_t at=0;
    while (at<text.size()) {
        while (at<text.size() && static_cast<unsigned char>(text[at])<32) ++at;
        auto start=at;
        while (at<text.size() && static_cast<unsigned char>(text[at])>=32) ++at;
        const auto line=text.substr(start,at-start);
        if (line.empty()) continue;
        if (line.front()=='=') {
            auto name=std::string(line.substr(1));
            if (name.empty() || name=="." || name==".." || name.size()>255)
                throw std::runtime_error("Invalid XTA image name");
            for (auto& c:name) {
                if (static_cast<unsigned char>(c)>=127 || c=='/' || c=='\\' || c==':')
                    throw std::runtime_error("Unsafe XTA image name");
                if (c>='A' && c<='Z') c+='a'-'A';
            }
            result.variants.push_back(std::move(name));
        } else {
            std::int32_t number=0;
            auto first=line.data(),last=first+line.size();
            while (first!=last && *first==' ') ++first;
            const auto parsed=std::from_chars(first,last,number);
            if (parsed.ec!=std::errc{} || parsed.ptr!=last || number<0 || number>32767)
                throw std::runtime_error("Unvalidated XTA group row");
            result.groups.push_back(static_cast<std::int16_t>(number));
        }
        if (result.groups.size()>8 || result.variants.size()>4095)
            throw std::runtime_error("XTA table exceeds observed engine bounds");
    }
    return result;
}
TextureBindings::TextureBindings(const ModelFile& model,TextureAnimation animation)
    :animation_(std::move(animation)),names_(model.textures),base_count_(model.textures.size()) {
    for (const auto group:animation_.groups)
        if (group<0 || std::size_t(group)>=base_count_) throw std::runtime_error("XTA group texture outside model");
    names_.insert(names_.end(),animation_.variants.begin(),animation_.variants.end());
    for (const auto& part:model.models) for (const auto& block:part.blocks) for (const auto& state:block.textures) {
        if (state.texture>=0 && std::size_t(state.texture)>=base_count_)
            throw std::runtime_error("Texture binding outside model");
        original_.push_back(state.texture);
    }
    current_=original_;
}
void TextureBindings::select(std::size_t group,std::size_t variant) {
    if (group>=animation_.groups.size() || variant>animation_.variants.size())
        throw std::runtime_error("XMT texture selection outside XTA table");
    for (std::size_t i=0;i<current_.size();++i)
        if (original_[i]==animation_.groups[group])
            current_[i]=variant ? static_cast<std::int32_t>(base_count_+variant-1) : original_[i];
}
void TextureBindings::restore() { current_=original_; }
}
