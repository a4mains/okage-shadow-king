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
        original_palettes_.push_back(state.palette);
    }
    current_=original_;
}
void TextureBindings::configure_source_registers(const void* owner,SourceRegisterResolver resolver) const {
    if(!owner || !resolver)throw std::runtime_error("Source texture packet resolver is unavailable");
    if(source_) {
        if(source_->owner_identity!=owner)throw std::runtime_error("Source texture packet owner changed");
        return;
    }
    if(missed_source_callback_)throw std::runtime_error("Source texture packet configured after selection/restore callback");
    SourceRegisters next{owner,std::move(resolver),{}, {}};
    next.original.reserve(original_.size());
    for(std::size_t i=0;i<original_.size();++i)
        next.original.push_back(original_[i]<0?std::nullopt:
            std::optional<std::uint64_t>(next.resolve(original_[i],original_palettes_[i])));
    next.current=next.original;
    source_=std::move(next);
}
const std::vector<std::optional<std::uint64_t>>& TextureBindings::source_registers() const {
    if(!source_)throw std::runtime_error("Source texture packet is not configured");
    return source_->current;
}
void TextureBindings::select(std::size_t group,std::size_t variant) {
    if (group>=animation_.groups.size() || variant>animation_.variants.size())
        throw std::runtime_error("XMT texture selection outside XTA table");
    const auto selected=variant?static_cast<std::int32_t>(base_count_+variant-1):0;
    const auto replacement=source_&&variant?
        std::optional<std::uint64_t>(source_->resolve(selected,0)):std::nullopt;
    if(!source_)missed_source_callback_=true;
    for (std::size_t i=0;i<current_.size();++i)
        if (original_[i]==animation_.groups[group]) {
            current_[i]=variant?selected:original_[i];
            if(source_)source_->current[i]=variant?replacement:source_->original[i];
        }
}
void TextureBindings::restore() {
    current_=original_;
    if(source_)source_->current=source_->original;
    else missed_source_callback_=true;
}
}
