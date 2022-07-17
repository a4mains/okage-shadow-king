#include "okage/ui_window.hpp"
#include "okage/ruw.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace okage {
namespace {
void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
std::size_t index(std::size_t offset,std::size_t bytes) {
    check(offset%4==0&&offset<bytes&&bytes-offset>=4,"UI window word bounds");
    return offset/4;
}
unsigned capacity(unsigned value) {
    check(value>=1&&value<=32,"Unsupported UI window capacity");return value;
}
std::array<std::uint32_t,4> scan(const UiWindowStream& stream) {
    check(stream.binding_id!=0,"Missing UI stream binding");
    std::array<std::uint32_t,4> counts{}; // 1958,195c,1960,1964 after 19c228.
    std::uint64_t ascii=0,wide=0,commands=0,controls=0;
    for(const auto& entry:stream.entries) {
        if(const auto* command=std::get_if<UiStreamCommand>(&entry)) {
            check(command->opcode>=1&&command->opcode<=27,"Unsupported UI stream command");
            if(const auto* ref=std::get_if<UiStreamReference>(&command->argument))
                check(*ref==UiStreamReference::MenuTransform1f9c00||
                      *ref==UiStreamReference::MenuTransform1f9c2c||
                      *ref==UiStreamReference::RuwMetrics200b78||
                      *ref==UiStreamReference::RuwClip200b88,"Unsupported UI stream reference");
            if(const auto* ref=std::get_if<UiWidgetReference>(&command->argument))
                check(command->opcode==23&&ref->resource&&
                      ref->group<ref->resource->widgets().size()&&
                      ref->index<ref->resource->widgets()[ref->group].size(),"Invalid UI widget reference");
            ++commands;continue;
        }
        const auto& text=std::get<UiStreamText>(entry).bytes;
        std::size_t at=0;
        while(true) {
            check(at<text.size(),"Unterminated UI stream text");
            const auto c=text[at];if(c==0)break;
            if(c<32)++controls;
            if(c&128) {
                check(text.size()-at>=2,"Truncated UI stream double-byte character");
                ++wide;at+=2;
            } else {++ascii;++at;}
        }
    }
    check(ascii>=controls&&ascii-controls<=4096,"UI stream exceeds heap flags");
    for(auto value:{wide,commands,controls})
        check(value<=std::uint64_t(std::numeric_limits<std::int32_t>::max()),"UI stream count overflow");
    counts={std::uint32_t(ascii-controls),std::uint32_t(wide),std::uint32_t(commands),std::uint32_t(controls)};
    return counts;
}
}
UiWindowBuffer::UiWindowBuffer(std::size_t bytes,bool cleared):bytes_(bytes),known_(bytes,cleared) {}
std::optional<std::uint8_t> UiWindowBuffer::known_byte(std::size_t at) const {
    check(at<bytes_.size(),"UI window buffer bounds");
    if(!known_[at])return std::nullopt;return bytes_[at];
}
std::uint32_t UiWindowBuffer::word(std::size_t at) const {
    index(at,bytes_.size());std::uint32_t result=0;
    for(unsigned i=0;i<4;++i) {
        const auto byte=known_byte(at+i);check(bool(byte),"Uninitialized UI window buffer word");
        result|=std::uint32_t(*byte)<<(i*8);
    }
    return result;
}
void UiWindowBuffer::put_word(std::size_t at,std::uint32_t value) {
    index(at,bytes_.size());
    for(unsigned i=0;i<4;++i){bytes_[at+i]=value>>(i*8);known_[at+i]=true;}
}
UiWindow::UiWindow(unsigned a,unsigned b,unsigned slot,std::shared_ptr<const UiWindowStream> stream)
    :stream_(std::move(stream)),flags_(0x1000,true),
     glyph_{UiWindowBuffer((capacity(a)+1)*256,false),UiWindowBuffer((capacity(a)+1)*256,false)},
     sprite_{UiWindowBuffer((capacity(b)+1)*256,true),UiWindowBuffer((capacity(b)+1)*256,true)},
     extra_{UiWindowBuffer(0x12c0,true),UiWindowBuffer(0x12c0,false),UiWindowBuffer(0x12c0,false)} {
    check(slot<8,"UI window registry slot bounds");check(bool(stream_),"Missing UI window stream");
    const auto counts=scan(*stream_);
    known_.set();
    // These are native owners, not guest addresses or portable allocator costs.
    for(auto at:{0u,0x2698u,0x3bf0u,0x18fcu,0x650u,0x654u,0x658u,0x65cu,
                 0x3d0cu,0x3d10u,0x3d14u,0x3d1cu,0x3d20u})known_.reset(at/4);
    put(4,slot);put(0x3c00,a);put(0x3c04,b);
    for(unsigned i=0;i<4;++i)put(0x1958+i*4,counts[i]);
    for(unsigned i=0;i<a;++i)glyph_cache_.push_back({UiWindowBuffer(0x12c0,false),UiWindowBuffer(0x12c0,false)});
    for(unsigned i=0;i<b;++i)sprite_cache_.push_back({UiWindowBuffer(0x12c0,false),UiWindowBuffer(0x12c0,false)});
    for(unsigned i=0;i<a*16;++i)glyph_[1].put_word(i*16+8,0xbf800000);
    for(unsigned i=0;i<b*16;++i)sprite_[1].put_word(i*16+8,0xbf800000);
    // 198ab8 -> 1a58f8/1a5940. Both capacities share the embedded flags.
    for(unsigned i=0;i<a;++i)put(0x3c08+i*4,word(0x3c08+i*4)|5);
    for(unsigned i=0;i<b;++i)put(0x3c08+i*4,word(0x3c08+i*4)|8);
    put(0x1900,1);put(0x4f0,1);put(0x22c,0x80808080);
    put(0x224,0x3f800000);put(0x228,0x3f800000);
    for(unsigned i=0;i<32;++i) {
        put(0xc+i*4,(i%16)*32);put(0x8c+i*4,(i/16)*32);
        put(0x10c+i*4,16);put(0x18c+i*4,16);
    }
    for(const auto [at,value]:std::initializer_list<std::pair<unsigned,std::uint32_t>>{
        {0x24c,20},{0x218,12},{0x21c,24},{0x258,4},{0x26c,0x3f800000},
        {0x2e4,0x80400040},{0x23c,4},{0x240,4},{0x250,4},{0x254,4},
        {0x268,0x3f800000},{0x25c,0x5555},{0x274,0x43000000},{0x27c,0x3e4ccccd},
        {0x564,0x43100000},{0x2dc,0x80808080},{0x2e0,0x80404000},{0x220,0xffffffff},
        {0x560,0x43100000},{0x2d0,0x80808080},{0x2d4,0x80808080},{0x2d8,0x80808080},
        {0x2e8,0x80004040},{0x2ec,0x80400000},{0x284,0x42c80000},{0x8a8,1},
        {0x214,0xffffffff},{0x192c,1},{0xcd4,1},{0x280,0x42c80000},
        {0x268c,1},{0x2690,1},{0x2694,1},{0x3be4,0xffffffff},{0x210,0xffffffff}})put(at,value);
    put(0x1900,(word(0x1900)&~0x10u)|0x644);
    // The remainder of 198e38 repeats flags/z resets and writes these scalars.
    put(0x22c,0x80808080);put(0x224,0x3f800000);put(0x228,0x3f800000);
    put(0x1900,word(0x1900)|0x8000);
}
void UiWindow::put(std::size_t at,std::uint32_t value) {
    const auto i=index(at,scalar_bytes);words_[i]=value;known_.set(i);
}
std::optional<std::uint32_t> UiWindow::known_word(std::size_t at) const {
    const auto i=index(at,scalar_bytes);if(!known_[i])return std::nullopt;return words_[i];
}
std::uint32_t UiWindow::word(std::size_t at) const {
    const auto result=known_word(at);check(bool(result),"Unknown UI window scalar word");return *result;
}
void UiWindow::set_opacity_tween(float increment,float current,
                                 std::uint32_t lower,std::uint32_t upper) {
    put(0x27c,__builtin_bit_cast(std::uint32_t, (increment)));
    put(0x270,__builtin_bit_cast(std::uint32_t, (current)));
    put(0x274,__builtin_bit_cast(std::uint32_t, (static_cast<float>(upper&0xff))));
    put(0x278,__builtin_bit_cast(std::uint32_t, (static_cast<float>(lower&0xff))));
}
float UiWindow::opacity() const {return __builtin_bit_cast(float, (word(0x270)));}
void UiWindow::set_transform_xyz(const std::array<std::uint32_t,3>& first,
                                 const std::array<std::uint32_t,3>& second) {
    put(0x4f4,1);
    for(unsigned i=0;i<3;++i){put(0x2f0+i*4,first[i]);put(0x520+i*4,first[i]);}
    for(unsigned i=0;i<3;++i){put(0x3f0+i*4,second[i]);put(0x550+i*4,second[i]);}
}
const UiWindowBuffer& UiWindow::glyph_buffer(unsigned i) const {check(i<2,"UI glyph buffer bounds");return glyph_[i];}
const UiWindowBuffer& UiWindow::sprite_buffer(unsigned i) const {check(i<2,"UI sprite buffer bounds");return sprite_[i];}
const UiWindowBuffer& UiWindow::glyph_cache(unsigned i,unsigned side) const {
    check(i<glyph_cache_.size()&&side<2,"UI glyph cache bounds");return glyph_cache_[i][side];
}
const UiWindowBuffer& UiWindow::sprite_cache(unsigned i,unsigned side) const {
    check(i<sprite_cache_.size()&&side<2,"UI sprite cache bounds");return sprite_cache_[i][side];
}
const UiWindowBuffer& UiWindow::extra_cache(unsigned i) const {check(i<3,"UI extra cache bounds");return extra_[i];}
bool UiWindow::configure(const std::array<std::uint32_t,28>& w,std::span<const AtlasTuple> atlas,
                         std::shared_ptr<const void> texture) {
    check(w[0]==stream_->binding_id&&w[25]==word(0x3c00)&&w[26]==word(0x3c04),"UI configuration owner mismatch");
    std::size_t entries=0;
    if(w[21]) {
        check(bool(texture)&&!atlas.empty()&&atlas.size()<=32,"Missing or unsupported UI atlas binding");
        while(entries<atlas.size()&&atlas[entries]!=AtlasTuple{})++entries;
        check(entries<atlas.size()||entries==32,"Unterminated UI atlas");
    } else check(atlas.empty()&&!texture,"Unexpected UI atlas binding");
    // Source literal copies, with no floating arithmetic or reinterpretation.
    put(0x24c,w[1]);for(auto at:{0x250,0x254,0x258})put(at,w[2]);
    put(0x20c,w[3]);for(unsigned i=0;i<3;++i)put(0x560+i*4,w[12+i]);
    put(0x4f8,16);put(0x530,0x40000000);put(0x534,0x40000000);put(0x538,0);
    known_.reset(0x53c/4);
    put(0x540,0);put(0x544,0);put(0x548,0);put(0x54c,0x41f00000);
    for(unsigned i=0;i<4;++i)put(0x2e0+i*4,w[16+i]);
    put(0x23c,0);put(0x240,0);
    put(0x1900,w[20]==1?word(0x1900)|0x1000:word(0x1900)&~0x1000u);
    set_transform_xyz({w[4],w[5],w[6]},{w[8],w[9],w[10]});
    put(0x25c,(word(0x25c)&0xffff0000)|0x5c5c);
    for(unsigned i=0;i<entries;++i)for(unsigned j=0;j<4;++j)put(0xc+j*0x80+i*4,atlas[i][j]);
    if(w[21]){texture_=std::move(texture);known_.reset(8/4);}
    return entries!=0;
}
}
