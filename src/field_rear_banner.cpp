#include "okage/field_rear_banner.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace okage {
namespace {
constexpr std::int32_t keep=RuwResource::preserve_component;
std::uint32_t word(Bytes data,std::size_t at) {
    if(at>data.size()||data.size()-at<4)throw std::runtime_error("Rear banner data bounds");
    return unsigned(data[at])|(unsigned(data[at+1])<<8)|(unsigned(data[at+2])<<16)|(unsigned(data[at+3])<<24);
}
bool digest(Bytes data,std::size_t at,std::string_view hex) {
    if(at>data.size()||data.size()-at<hex.size()/2)return false;
    constexpr std::string_view digits="0123456789abcdef";
    for(std::size_t i=0;i<hex.size();++i)
        if(hex[i]!=digits[(data[at+i/2]>>(i%2?0:4))&15])return false;
    return true;
}
const RuwWidget& text_owner(const RuwResource& r) {
    (void)r.widget_index(FieldRearBanner::group,FieldRearBanner::text_widget);
    const auto& widget=r.widget(FieldRearBanner::group,FieldRearBanner::text_widget);
    if(widget.words[5]!=1||!widget.runtime||!widget.runtime->text)
        throw std::runtime_error("Rear encounter banner requires field RUW group16 text widget1");
    return widget;
}
}
FieldRearBannerData::FieldRearBannerData(Bytes data) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','R','B','N','1',0,0};
    constexpr std::size_t header=88,payload=112+16+16;
    if(data.size()!=header+payload||!std::equal(magic.begin(),magic.end(),data.begin())||
       word(data,8)!=1||word(data,12)!=28||word(data,16)!=1||word(data,20)!=16)
        throw std::runtime_error("Unsupported rear banner data format");
    if(!digest(data,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")||
       !digest(data,56,"9e42014fbf83588ce4728b58be4e08aaf42c81011ae7fd97536d5aeb47df71f9"))
        throw std::runtime_error("Unsupported rear banner source revision");
    for(std::size_t i=0;i<row_.size();++i)row_[i]=word(data,header+i*4);
    if(row_[0]!=1||row_[21]!=1||row_[22]!=1||row_[23]!=1||row_[24]!=13||
       row_[25]!=8||row_[26]!=2||row_[20]!=1||row_[27]!=0)
        throw std::runtime_error("Unsupported rear banner host binding or row layout");
    UiWindow::AtlasTuple terminator{};
    for(std::size_t j=0;j<4;++j)terminator[j]=word(data,header+112+j*4);
    if(terminator!=UiWindow::AtlasTuple{})throw std::runtime_error("Unsupported rear banner atlas entries");
    atlas_.push_back(terminator);
    const auto text=data.subspan(header+128,16);
    if(!std::equal(text.begin(),text.end(),"surprise attack"))
        throw std::runtime_error("Unsupported rear banner phase-11 argument");
}
FieldRearBanner::FieldRearBanner(BattleUiShared& ui,std::shared_ptr<UiWindowStream> stream,
                                 const FieldRearBannerData& data,std::shared_ptr<RuwResource> ruw,
                                 std::shared_ptr<const void> texture,std::string_view format,std::uint32_t& flag)
    :ui_(ui),ruw_(std::move(ruw)) {
    if(!ruw_||!stream||!texture)throw std::runtime_error("Rear encounter banner requires RUW, layer stream and bm_if owners");
    (void)text_owner(*ruw_);
    //115b50 preconditions, checked before any registry/global mutation.
    const auto conversion=format.find('%');
    if(conversion==format.npos||format.substr(conversion,2)!="%s"||
       format.find('%',conversion+2)!=format.npos)
        throw std::runtime_error("Rear encounter sysmes0x72 conversion is unrecovered");
    std::string text(format.substr(0,conversion));
    text+=argument;text+=format.substr(conversion+2);
    if(text.size()>=128)throw std::runtime_error("Rear encounter banner text exceeds115b50 stack buffer");
    const auto& row=data.row();
    if(row[0]!=stream->binding_id)throw std::runtime_error("Rear encounter banner layer stream binding");
    const auto free=std::find(ui_.registry.begin(),ui_.registry.end(),nullptr);
    if(free==ui_.registry.end())throw std::runtime_error("Rear encounter banner UI registry exhausted");
    slot_=unsigned(free-ui_.registry.begin());
    ui_.atlas_texture=-1; //11f5f8: *0x24f734=-1 before 1152a8.
    //11f7a0: 198e38(word25,word26,stream) scans 0x1f86c0 as it is now (zero
    //before the first banner, [14,6] afterwards), then the row setters.
    auto window=std::make_shared<UiWindow>(row[25],row[26],slot_,stream);
    *free=window;window_=window.get();
    try {
        if(window->configure(row,data.atlas(),std::move(texture)))ui_.resource.layout_shared().sprite_dirty=true;
        //row+58 0x115b20 -> 115ad8: 0x1f9ca0=0x1f86c0, cursor 0, then 1215e0
        //appends 14/0x1f9c00 and 6/0x1f9c2c; 115b20 then clears 0x1f8608.
        stream->entries={UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
                         UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}};
        ui_.current_stream=stream->binding_id;ui_.stream_cursor=4;
        flag=0;
        //115b50: flag first, then 123530(0x72) and 1f15a8 into a 128-byte stack
        //buffer. Only the reached single %s conversion is supported.
        flag=1;
        ruw_->set_text(group,text_widget,text);
        //1f1b70 strlen of the argument; SLL/ADDU/SUBU with 32-bit wrap.
        const auto x=std::bit_cast<std::int32_t>(0x140u-std::uint32_t(argument.size())*5u);
        ruw_->set_position(group,text_widget,x,keep);
    } catch(...) {
        *free=nullptr;window_=nullptr;throw;
    }
}
FieldRearBanner::~FieldRearBanner() {
    if(window_&&ui_.registry[slot_].get()==window_)ui_.registry[slot_].reset();
}
UiWindow& FieldRearBanner::live_window() const {
    auto& entry=ui_.registry[slot_];
    if(!window_||entry.get()!=window_)throw std::runtime_error("Rear encounter banner window lifetime");
    return *entry;
}
const UiWindow& FieldRearBanner::window() const {return live_window();}
bool FieldRearBanner::layout_enabled() const {return (live_window().word(0x1900)&0x1000u)!=0;}
void FieldRearBanner::traverse(UiPartySelection& party,std::span<std::uint8_t> variables,
                               unsigned game_mode,std::uint32_t flag) {
    auto& window=live_window();
    //11fec8: layer flags 0x40 were set by 11f7a0; type13 dispatches 115c48.
    //Its pad reads normalize stack copies only. Controller word +e4c[e8c]:
    if(state_==0) {
        //11faa8(8.0): 198ab8 reset, then 1a4d70(8.0,1.0,win,0,0x80).
        window.reset_for_open();window.set_opacity_tween(8.f,1.f,0,0x80);
        ui_.resource_accumulators[0]=0;
        const std::array<std::size_t,1> groups{group};window.bind_ruw(ruw_,groups);
        ruw_->reset_class(1,-1);                          //1ab8b0(0,1,-1,0)
        ruw_->set_class_rgba(group,-1,{keep,keep,keep,0}); //1ab410(0,16,-1,k,k,k,0)
        party.refresh(variables,game_mode);               //121688
        state_=1;                                         //+e4c[0]++, +a8=+a4=0
    } else if(state_<0)
        throw std::runtime_error("Rear encounter banner closing states -1/-2 have no reached writer");
    //120ad0 -> row+5c 0x115bd8 with the layer payload.
    window.set_layout_enabled(false);                     //1a5488(win,0)
    if(flag==1) {
        window.set_layout_enabled(true);                  //1a5488(win,1)
        ruw_->set_class_rgba(group,-1,{keep,keep,keep,128}); //1ab410(0,16,-1,k,k,k,128)
    }
    ++traversals_;
}
void FieldRearBanner::mark() {
    if(marked_)throw std::runtime_error("Rear encounter banner marked twice");
    marked_=true;
}
std::vector<TextGlyph> FieldRearBanner::glyphs(const Font& font) const {
    std::vector<TextGlyph> result;
    if(!bound())return result;
    const auto& widget=text_owner(*ruw_);
    const auto& runtime=*widget.runtime;
    const auto alpha=std::min<std::uint32_t>(runtime.words[4]>>24,128);
    if(!alpha||!(widget.words[4]&1))return result;
    std::string_view text(runtime.text->data(),strnlen(runtime.text->data(),runtime.text->size()));
    while(text.size()>=3&&text[0]=='\\'&&(text[1]=='f'||text[1]=='s')&&text[2]=='[') {
        const auto end=text.find(']');
        if(end==text.npos)throw std::runtime_error("Unterminated rear banner control");
        text.remove_prefix(end+1);
    }
    int x=std::bit_cast<std::int32_t>(runtime.words[6])-28;
    const int y=std::bit_cast<std::int32_t>(runtime.words[7])-27;
    for(unsigned char c:text) {
        const auto* found=font.find(c);
        if(c<32||c>=127||c=='\\'||!found)throw std::runtime_error("Unrecovered rear banner character");
        const auto& m=found->metrics;
        if(c!=' ') {
            const int px=x-std::bit_cast<std::int16_t>(m[2])/16+24;
            const int py=y-(std::bit_cast<std::int16_t>(m[1])>>2)/2+36;
            result.push_back({c,2,0x00ffffffu|(alpha<<24),px,py/2,int(m[8]/16),int((m[9]>>2)/4)});
        }
        x+=c==' '?10:m[8]/16;
    }
    return result;
}
}
