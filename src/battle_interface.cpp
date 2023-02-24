#include "okage/battle_interface.hpp"
#include <algorithm>
#include <stdexcept>

namespace okage {
namespace {
void validate_layer_increment(std::int32_t increment) {
    if(increment < -16777216 || increment > 16777216)
        throw std::runtime_error("Battle layer opacity increment conversion is unrecovered");
}
}
BattleInterface::BattleInterface(BattleUiShared& shared,const BattleUiData& data,
    std::shared_ptr<const void> texture,std::uint32_t parent_flags,
    const std::function<void(unsigned)>& constructor):shared_(shared) {
    if(!texture||!constructor)throw std::runtime_error("Battle interface requires texture and constructors");
    shared_.atlas_texture=-1;
    // Header code/owner links are typed host ownership, never guest addresses.
    words_[0x1c/4]=parent_flags&0x18000000;
    try {
        for(unsigned i=0;i<2;++i) {
            const auto& d=data.layers()[i];const auto& w=d.words;const unsigned base=(0x40+i*0xe0)/4;
            auto& stream=shared_.streams.at(d.stream()-1);
            if(!stream||stream->binding_id!=d.stream())throw std::runtime_error("Battle interface stream binding mismatch");
            words_[base+0x54/4]=d.stream(); // Typed reference ID, not a guest pointer.
            const auto free=std::find(shared_.registry.begin(),shared_.registry.end(),nullptr);
            if(free==shared_.registry.end())throw std::runtime_error("UI window registry exhausted");
            const unsigned slot=free-shared_.registry.begin();
            // Scan the existing stream before the layer rewrites it below.
            auto owner=std::make_shared<UiWindow>(d.glyph_capacity(),d.sprite_capacity(),slot,stream);
            *free=std::move(owner);handles_[i]=int(slot);words_[base]=slot;
            words_[base+0x40/4]=w[1];words_[base+0x44/4]=w[2];
            const bool dirty=(*free)->configure(w,shared_.atlas,texture);
            if(dirty)shared_.atlas_dirty=true; // 1a5500 sets 200530, not 24f734.
            if(w[21])words_[base+1]=1;
            std::copy_n(w.begin()+4,12,words_.begin()+base+4);
            words_[base+0x4c/4]=std::uint32_t(shared_.atlas_texture);
            for(unsigned j=0;j<8;++j) {
                words_[base+0x6c/4+j]=0xffffffff;words_[base+0xac/4+j]=0xffffffff;
            }
            words_[base+0x64/4]=0xffffffff;
            stream->entries={UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
                             UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}};
            shared_.current_stream=d.stream();shared_.stream_cursor=4;
            constructor(d.type());
            words_[base+0x50/4]|=0x50;words_[base+0x5c/4]=w[23];words_[base+0x60/4]=d.type();
            // Both supported authored types have one state slot (1f8f88).
            words_[0xe90/4+i]=i;
        }
        words_[0xe40/4]=2;
        words_[0x8c/4]=0xffffffff;words_[0xe44/4]=words_[0x180/4];words_[0xed8/4]=0xffffffff;
        words_[0x168/4]=0xffffffff;words_[0x16c/4]=0xffffffff;words_[0x88/4]=0xffffffff;
    }catch(...){release();throw;}
}
void BattleInterface::release() {
    for(auto& handle:handles_)if(handle>=0){shared_.registry[unsigned(handle)].reset();handle=-1;}
}
BattleInterface::~BattleInterface(){release();}
const UiWindow& BattleInterface::window(unsigned layer) const {
    if(layer>=2||handles_[layer]<0||!shared_.registry[unsigned(handles_[layer])])
        throw std::runtime_error("Battle interface window lifetime");
    return *shared_.registry[unsigned(handles_[layer])];
}
void BattleInterface::open_layer(unsigned layer) {
    (void)window(layer);
    const auto state_index=words_[0xe90/4+layer];
    constexpr unsigned state_count=(0xe8c-0xe4c)/4;
    if(state_index>=state_count)
        throw std::runtime_error("Battle interface open state index outside owner");
    const unsigned base=(0x40+layer*0xe0)/4;
    //11fe28 clears command and argument, selects the manager state, then opens.
    words_[base+0x68/4]=0;
    words_[base+0x64/4]=0;
    words_[0xe4c/4+state_index]=1;
    shared_.registry[unsigned(handles_[layer])]->set_opacity_tween(16.f,1.f,0,128);
}
void BattleInterface::fade_layer(unsigned layer,std::int32_t increment) {
    (void)window(layer);
    validate_layer_increment(increment);
    shared_.registry[unsigned(handles_[layer])]->set_opacity_tween(static_cast<float>(increment),128.f,0,128);
}
void BattleInterface::reveal_layer(unsigned layer,std::int32_t increment) {
    (void)window(layer);
    validate_layer_increment(increment);
    shared_.registry[unsigned(handles_[layer])]->set_opacity_tween(float(increment),1.f,0,128);
}
}
