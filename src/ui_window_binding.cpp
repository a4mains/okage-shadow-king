#include "okage/ui_window.hpp"
#include "okage/ruw.hpp"
#include <algorithm>
#include <stdexcept>

namespace okage {
void UiWindow::bind_ruw(std::shared_ptr<RuwResource> resource,
                        std::span<const std::size_t> groups) {
    if(!resource)throw std::runtime_error("Missing UI RUW owner");
    std::size_t count=0;
    for(auto group:groups) {
        if(group>=resource->widgets().size())throw std::runtime_error("UI RUW group bounds");
        const auto size=resource->widgets()[group].size();
        if(size>253-count)throw std::runtime_error("UI RUW embedded stream overflow");
        count+=size;
    }
    auto bound=std::make_shared<UiWindowStream>();
    bound->binding_id=stream_->binding_id;
    bound->embedded_ruw=true;
    bound->entries.reserve(count+2);
    bound->entries.emplace_back(UiStreamCommand{14,UiStreamReference::RuwMetrics200b78});
    bound->entries.emplace_back(UiStreamCommand{6,UiStreamReference::RuwClip200b88});
    for(auto group:groups)for(std::size_t i=0;i<resource->widgets()[group].size();++i)
        bound->entries.emplace_back(UiStreamCommand{23,UiWidgetReference{resource,group,i}});

    unsigned cursor=0;
    for(const auto& entry:bound->entries) {
        const auto& command=std::get<UiStreamCommand>(entry);
        put(0x26dc+cursor*4,command.opcode);++cursor;
        known_.reset((0x26dc+cursor*4)/4);++cursor;
    }
    put(0x26dc+cursor*4,0);put(0x2edc,cursor);
    for(auto group:groups)resource->prepare_group_binding(group);
    put(0x3bec,2);
    stream_=std::move(bound);known_.reset(0x2698/4);

    const unsigned glyph_count=word(0x3c00),sprite_count=word(0x3c04);
    for(unsigned i=0;i<glyph_count;++i)put(0x3c08+i*4,word(0x3c08+i*4)|5);
    for(unsigned i=0;i<sprite_count;++i)put(0x3c08+i*4,word(0x3c08+i*4)|8);
    put(0xcc0,0);put(0xcc4,0);
    put(0x268,word(0x26c));put(0x258,word(0x250));put(0x254,word(0x250));
    put(0x1900,(word(0x1900)&0xfffff6c5u)|1);
    put(0x3be4,0xffffffff);
    for(auto at:{0xce8,0x264,0x260,0xcd0})put(at,0);
    for(auto& buffer:glyph_)for(std::size_t at=0;at<buffer.size();at+=4)buffer.put_word(at,0);
    for(unsigned i=0;i<glyph_count*16;++i)glyph_[1].put_word(i*16+8,0xbf800000);
}
}
