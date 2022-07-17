#include "okage/ui_window.hpp"
#include "okage/ruw.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class F> void rejects(F f) {
    try{f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("Expected explicit UI window rejection");
}
std::shared_ptr<okage::UiWindowStream> stream() {
    return std::make_shared<okage::UiWindowStream>(okage::UiWindowStream{1,{}});
}
std::array<std::uint32_t,28> authored() {
    // Scalar inputs from authored interface 248100; pointer fields use host IDs.
    return {1,40,6,2,0xc39f8000,0xc29e0000,0x43828cd9,0x3f800000,
            0,0xbfc00000,0,0,0,0x43100000,0,0,
            0x53f0167d,0x53f0167d,0x53780348,0x53780348,1,1,1,1,2,29,17,0};
}
void defaults_and_allocations() {
    auto source=stream();okage::UiWindow w(29,17,3,source);
    require(w.word(4)==3&&w.word(0x3c00)==29&&w.word(0x3c04)==17,"Slot/capacity");
    require(w.word(0x1900)==0x8645,"Constructor reset/initialization order");
    require(w.word(0x25c)==0x5555&&w.word(0x24c)==20&&w.word(0x250)==4,"Display defaults");
    require(w.word(0x214)==0xffffffff&&w.word(0x210)==0xffffffff&&w.word(0x3be4)==0xffffffff,"Signed sentinels");
    require(w.word(0x26c)==0x3f800000&&w.word(0x268)==0x3f800000&&w.word(0x224)==0x3f800000,"Default scales");
    require(w.word(0x22c)==0x80808080&&w.word(0x2e0)==0x80404000&&w.word(0x2e4)==0x80400040,"Default colors");
    require(w.word(0x2e8)==0x80004040&&w.word(0x2ec)==0x80400000,"Default colors continued");
    require(w.word(0x53c)==0&&w.word(0x6ebc)==0,"Payload clear initially known");
    for(auto at:{0u,0x2698u,0x3bf0u,0x18fcu,0x650u,0x654u,0x658u,0x65cu,
                 0x3d0cu,0x3d10u,0x3d14u,0x3d1cu,0x3d20u}) {
        require(!w.known_word(at),"Host ownership is not a guest pointer");rejects([&]{w.word(at);});
    }
    require(w.word(8)==0&&!w.texture_owner(),"Unbound texture");
    for(unsigned i=0;i<32;++i) {
        require(w.word(0xc+i*4)==(i%16)*32&&w.word(0x8c+i*4)==(i/16)*32,"Default atlas position");
        require(w.word(0x10c+i*4)==16&&w.word(0x18c+i*4)==16,"Default atlas extent");
        require(w.word(0x3c08+i*4)==(i<17?13u:i<29?5u:0u),"Shared capacity flags");
    }
    require(w.flags().size()==4096&&w.flags().word(4092)==0,"Cleared flags owner");
    require(w.glyph_buffer(0).size()==30*256&&w.sprite_buffer(0).size()==18*256,"Sentinel allocation capacity");
    require(!w.glyph_buffer(0).known_byte(0)&&!w.glyph_buffer(1).known_byte(0),"Glyph allocations not cleared");
    require(w.sprite_buffer(0).word(0)==0&&w.sprite_buffer(1).word(0)==0,"Sprite allocations cleared");
    for(unsigned i=0;i<29*16;++i)require(w.glyph_buffer(1).word(i*16+8)==0xbf800000,"Glyph depth reset");
    require(!w.glyph_buffer(1).known_byte(29*256+8),"Glyph sentinel tail uninitialized");
    for(unsigned i=0;i<17*16;++i)require(w.sprite_buffer(1).word(i*16+8)==0xbf800000,"Sprite depth reset");
    require(w.sprite_buffer(1).word(17*256+8)==0,"Sprite sentinel tail cleared");
    for(unsigned side=0;side<2;++side) {
        require(w.glyph_cache(28,side).size()==0x12c0&&!w.glyph_cache(28,side).known_byte(0),"Glyph cache ownership");
        require(w.sprite_cache(16,side).size()==0x12c0&&!w.sprite_cache(16,side).known_byte(0),"Sprite cache ownership");
    }
    require(w.extra_cache(0).word(0x12bc)==0&&!w.extra_cache(1).known_byte(0)&&!w.extra_cache(2).known_byte(0),"Only first extra cache cleared");
    rejects([&]{w.word(2);});rejects([&]{w.word(0x6ec0);});rejects([&]{w.glyph_buffer(2);});
    rejects([&]{w.glyph_cache(29,0);});rejects([&]{w.sprite_cache(0,2);});rejects([&]{w.extra_cache(3);});
    rejects([&]{w.flags().word(4096);});rejects([&]{w.glyph_buffer(0).word(0);});
    const auto* owner=&w.glyph_cache(0,0);auto moved=std::move(w);
    require(&moved.glyph_cache(0,0)==owner&&&moved.stream()==source.get(),"Moving preserves owner/reference identities");
}
void stream_scan_and_lifetime() {
    auto s=stream();s->entries={
        okage::UiStreamCommand{14,okage::UiStreamReference::MenuTransform1f9c00},
        okage::UiStreamText{{'A',1,0x81,0x40,'B',0,'x'}},
        okage::UiStreamCommand{6,okage::UiStreamReference::MenuTransform1f9c2c},
        okage::UiStreamText{{0}},okage::UiStreamCommand{27,std::uint32_t(42)}};
    okage::UiWindow w(2,3,0,s);
    require(w.word(0x1958)==2&&w.word(0x195c)==1&&w.word(0x1960)==3&&w.word(0x1964)==1,"19c228 text/control/command counts");
    require(w.word(0x3c08)==13&&w.word(0x3c0c)==13&&w.word(0x3c10)==8,"Sprite flags can extend past glyph capacity");
    s->entries.clear();require(w.stream().entries.empty()&&w.word(0x1960)==3,"Shared stream changes do not retroactively rescan");
    s->entries.push_back(okage::UiStreamCommand{14,okage::UiStreamReference::MenuTransform1f9c00});
    auto weak=std::weak_ptr(s);s.reset();require(!weak.expired()&&w.stream().entries.size()==1,"Window retains stream ownership");
    auto malformed=stream();
    for(auto opcode:{0u,28u,0xffffffffu}) {
        malformed->entries={okage::UiStreamCommand{opcode,std::uint32_t(0)}};
        rejects([&]{okage::UiWindow bad(1,1,0,malformed);});
    }
    malformed->entries={okage::UiStreamCommand{1,static_cast<okage::UiStreamReference>(99)}};
    rejects([&]{okage::UiWindow bad(1,1,0,malformed);});
    for(auto bytes:{std::vector<std::uint8_t>{},std::vector<std::uint8_t>{'A'},
                   std::vector<std::uint8_t>{0x81},std::vector<std::uint8_t>{0x81,0}}) {
        malformed->entries={okage::UiStreamText{bytes}};rejects([&]{okage::UiWindow bad(1,1,0,malformed);});
    }
    auto bytes=std::vector<std::uint8_t>(4098,'A');bytes.back()=0;
    malformed->entries={okage::UiStreamText{bytes}};rejects([&]{okage::UiWindow bad(1,1,0,malformed);});
    malformed->entries.clear();malformed->binding_id=0;rejects([&]{okage::UiWindow bad(1,1,0,malformed);});
    for(unsigned c:{0u,33u,0xffffffffu}) {
        rejects([&]{okage::UiWindow bad(c,1,0,stream());});rejects([&]{okage::UiWindow bad(1,c,0,stream());});
    }
    rejects([&]{okage::UiWindow bad(1,1,8,stream());});rejects([&]{okage::UiWindow bad(1,1,0,nullptr);});
    okage::UiWindow edge(32,32,7,stream());require(edge.word(0x3c84)==13,"Bounded maximum capacity");
}
void configuration() {
    auto s=stream();okage::UiWindow w(29,17,0,s);auto a=authored();
    auto texture=std::make_shared<int>(123);const auto* identity=texture.get();
    std::vector<okage::UiWindow::AtlasTuple> atlas={{{1,2,3,4}},{{5,6,7,8}},{{0,0,0,0}},{{99,99,99,99}}};
    require(w.configure(a,atlas,texture),"Actual atlas writes mark shared dirty");
    require(w.texture_owner().get()==identity&&!w.known_word(8),"Typed texture binding");
    texture.reset();require(*static_cast<const int*>(w.texture_owner().get())==123,"Texture retained by owner");
    require(w.word(0x24c)==40&&w.word(0x250)==6&&w.word(0x254)==6&&w.word(0x258)==6,"198650 arguments");
    require(w.word(0x20c)==2&&w.word(0x4f8)==16&&w.word(0x4f4)==1,"Window scalar configuration");
    require(w.word(0x530)==0x40000000&&w.word(0x534)==0x40000000&&w.word(0x538)==0,"Configured transform xyz");
    require(!w.known_word(0x53c)&&w.word(0x54c)==0x41f00000,"Unknown stack W excluded");rejects([&]{w.word(0x53c);});
    for(unsigned i=0;i<3;++i) {
        require(w.word(0x560+i*4)==a[12+i],"Authored extent");
        require(w.word(0x2f0+i*4)==a[4+i]&&w.word(0x520+i*4)==a[4+i],"Position duplicate storage");
        require(w.word(0x3f0+i*4)==a[8+i]&&w.word(0x550+i*4)==a[8+i],"Second position duplicate storage");
    }
    for(unsigned i=0;i<4;++i)require(w.word(0x2e0+i*4)==a[16+i],"Raw authored colors");
    require(w.word(0x1900)==0x9645&&w.word(0x25c)==0x5c5c&&w.word(0x23c)==0&&w.word(0x240)==0,"Layout/group configuration");
    require(w.word(0xc)==1&&w.word(0x8c)==2&&w.word(0x10c)==3&&w.word(0x18c)==4,"Atlas tuple scatter");
    require(w.word(0x10)==5&&w.word(0x14)==64&&w.word(0x18)==96,"Terminator preserves remaining defaults");
    auto missing=a;missing[0]=2;rejects([&]{w.configure(missing,atlas,w.texture_owner());});
    missing=a;missing[25]=28;rejects([&]{w.configure(missing,atlas,w.texture_owner());});
    rejects([&]{w.configure(a,{},w.texture_owner());});rejects([&]{w.configure(a,atlas,nullptr);});
    const auto old=w.word(0x24c);missing=a;missing[1]=999;
    std::vector<okage::UiWindow::AtlasTuple> unterminated={{{1,1,1,1}}};
    rejects([&]{w.configure(missing,unterminated,w.texture_owner());});require(w.word(0x24c)==old,"Binding validation before mutation");
    a[20]=0;std::array<okage::UiWindow::AtlasTuple,1> empty{};
    require(!w.configure(a,empty,w.texture_owner())&&w.word(0x1900)==0x8645,"Explicit empty atlas and dirty off");
    std::array<okage::UiWindow::AtlasTuple,32> full;for(auto& e:full)e={1,2,3,4};
    require(w.configure(a,full,w.texture_owner())&&w.word(0x88)==1,"Full 32 atlas needs no terminator");
    okage::UiWindow absent(29,17,0,s);a[21]=0;
    require(!absent.configure(a,{},nullptr)&&absent.word(8)==0,"Absent atlas leaves texture null");
    rejects([&]{absent.configure(a,empty,nullptr);});
    // Configuration neither invokes callbacks nor replaces the shared stream.
    require(w.stream().entries.empty()&&w.word(0x1958)==0&&w.word(0x1960)==0,"No fabricated UI execution");
}
using ScalarSnapshot=std::array<std::optional<std::uint32_t>,okage::UiWindow::scalar_bytes/4>;
ScalarSnapshot snapshot(const okage::UiWindow& w) {
    ScalarSnapshot result;
    for(std::size_t i=0;i<result.size();++i)result[i]=w.known_word(i*4);
    return result;
}
void scalar_mutators() {
    auto s=stream();okage::UiWindow w(29,17,0,s);
    auto texture=std::make_shared<int>(123);
    const std::array<okage::UiWindow::AtlasTuple,1> empty{};
    w.configure(authored(),empty,texture);
    const auto* glyph=&w.glyph_buffer(0);const auto* cache=&w.extra_cache(0);
    struct RestoreRounding {
        int mode=std::fegetround();
        ~RestoreRounding(){std::fesetround(mode);}
    } restore;
    for(int mode:{FE_TONEAREST,FE_TOWARDZERO,FE_DOWNWARD,FE_UPWARD}) {
        require(std::fesetround(mode)==0,"Set test rounding mode");
        auto expected=snapshot(w);
        // Reversed bounds and out-of-range current are stored, not normalized.
        w.set_opacity_tween(-0.0f,256.25f,0x123401ff,0xffffff80);
        expected[0x27c/4]=0x80000000;expected[0x270/4]=0x43802000;
        expected[0x274/4]=0x43000000;expected[0x278/4]=0x437f0000;
        require(snapshot(w)==expected,"Opacity changes exactly four scalar words/masks");
        require(__builtin_bit_cast(std::uint32_t, (w.opacity()))==0x43802000,"Fractional opacity getter is a float copy");
        w.set_opacity_tween(-128.0f,-0.0f,0x100,0x200);
        expected[0x27c/4]=0xc3000000;expected[0x270/4]=0x80000000;
        expected[0x274/4]=0;expected[0x278/4]=0;
        require(snapshot(w)==expected&&__builtin_bit_cast(std::uint32_t, (w.opacity()))==0x80000000,
                "Repeated opacity writes preserve negative zero and masked zero bounds");
        w.set_opacity_tween(__builtin_bit_cast(float, (0x7fc12345u)),12.75f,0,128);
        expected[0x27c/4]=0x7fc12345;expected[0x270/4]=0x414c0000;
        expected[0x274/4]=0x43000000;
        require(snapshot(w)==expected&&w.opacity()==12.75f,"Opaque increment bits and fractional getter");
        require(std::fegetround()==mode,"Scalar opacity calls preserve rounding mode");
    }
    auto expected=snapshot(w);
    const std::array<std::uint32_t,3> first={0x80000000,0x3f000001,0x7fcabcde};
    const std::array<std::uint32_t,3> second={0xff800000,0x00000001,0xbf800000};
    w.set_transform_xyz(first,second);
    expected[0x4f4/4]=1;
    for(unsigned i=0;i<3;++i) {
        expected[0x2f0/4+i]=expected[0x520/4+i]=first[i];
        expected[0x3f0/4+i]=expected[0x550/4+i]=second[i];
    }
    require(snapshot(w)==expected,"Transform changes only XYZ pairs/flag, preserving all W/history/unknown lanes");
    require(!w.known_word(0x53c),"Transform does not fabricate the unknown configured W lane");
    require(w.texture_owner().get()==texture.get()&&&w.stream()==s.get()&&
            &w.glyph_buffer(0)==glyph&&&w.extra_cache(0)==cache,"Scalar mutators retain owners");
    require(w.extra_cache(0).word(0x12bc)==0&&!w.glyph_buffer(0).known_byte(0),"Scalar mutators leave buffer contents/validity untouched");
}
void ruw_binding() {
    // One template group, three widget groups (2,0,1); deliberately unordered IDs.
    std::vector<std::uint8_t> bytes(36+4*68);
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)bytes.at(at+i)=v>>(8*i);};
    put(12,1);put(16,3);put(20,1);put(24,2);put(28,0);put(32,1);
    for(unsigned i=0;i<3;++i) {
        const unsigned at=104+i*68;
        put(at+8,0x80445566);put(at+12,0x12345678);put(at+16,0x40);
        put(at+20,1);put(at+32,2);put(at+36,1);put(at+40,i&1);put(at+60,20-i);
    }
    auto resource=std::make_shared<okage::RuwResource>(bytes);
    auto source=stream();okage::UiWindow w(2,3,0,source);
    const auto original_counts=w.word(0x1960);
    const std::array<std::size_t,4> groups{2,1,0,2}; // Empty and duplicate allowed.
    w.bind_ruw(resource,groups);
    require(w.stream().embedded_ruw&&w.stream().entries.size()==6,"Owned embedded stream");
    require(source->entries.empty(),"External constructor stream remains intact");
    require(w.word(0x2edc)==12&&w.word(0x26dc+48)==0,"Cursor excludes terminating word");
    require(w.word(0x3bec)==2&&!w.known_word(0x2698),"Typed stream replaces guest pointer");
    for(unsigned i=0;i<6;++i) {
        require(w.word(0x26dc+i*8)==(i==0?14u:i==1?6u:23u),"Ordered raw commands");
        require(!w.known_word(0x26e0+i*8),"Reference operands are never scalar addresses");
    }
    const std::array<std::pair<std::size_t,std::size_t>,4> expected{{{2,0},{0,0},{0,1},{2,0}}};
    for(unsigned i=0;i<4;++i) {
        const auto& ref=std::get<okage::UiWidgetReference>(std::get<okage::UiStreamCommand>(w.stream().entries[i+2]).argument);
        require(ref.resource.get()==resource.get()&&ref.group==expected[i].first&&ref.index==expected[i].second,"Physical order and retained resource reference");
    }
    require(w.word(0x1900)==0x8645&&w.word(0x1960)==original_counts,"Reset flags without construction rescan");
    for(unsigned side=0;side<2;++side)for(unsigned at=0;at<w.glyph_buffer(side).size();at+=4)
        require(w.glyph_buffer(side).word(at)==(side==1&&at<2*256&&at%16==8?0xbf800000u:0u),"Complete glyph reset including sentinel");
    require(!w.glyph_cache(0,0).known_byte(0)&&!w.extra_cache(1).known_byte(0),"Uninitialized layout caches preserved");
    require(w.sprite_buffer(1).word(8)==0xbf800000&&w.sprite_buffer(1).word(3*256+8)==0,"Sprite output unchanged");
    require(w.word(0x3c08)==13&&w.word(0x3c10)==8,"Separate glyph and sprite flag extents");
    require(w.word(0xcc0)==0&&w.word(0xcc4)==0&&w.word(0xce8)==0&&w.word(0x264)==0&&w.word(0x260)==0&&w.word(0xcd0)==0,"Reveal and output counters reset");
    require(w.word(0x268)==w.word(0x26c)&&w.word(0x254)==w.word(0x250)&&w.word(0x258)==w.word(0x250),"Reveal defaults restored");
    const auto* previous=&w.stream();const auto old_cursor=w.word(0x2edc);
    rejects([&]{w.bind_ruw(nullptr,groups);});
    const std::array<std::size_t,2> invalid{0,3};rejects([&]{w.bind_ruw(resource,invalid);});
    std::vector<std::size_t> too_many(127,0);rejects([&]{w.bind_ruw(resource,too_many);});
    require(&w.stream()==previous&&w.word(0x2edc)==old_cursor,"Validation precedes binding mutations");
    w.bind_ruw(resource,{});
    require(w.stream().entries.size()==2&&w.word(0x2edc)==4&&w.word(0x26ec)==0,"Empty binding retains setup commands");
    require(!w.known_word(0x26f0)&&w.word(0x26f4)==23,"Shorter bind preserves unreachable stream tail");
    w.bind_ruw(resource,groups);std::weak_ptr weak=resource;resource.reset();
    require(!weak.expired(),"Bound resource lifetime");
}
}
int main(){try {
    defaults_and_allocations();stream_scan_and_lifetime();configuration();scalar_mutators();ruw_binding();
    std::cout<<"UI window ownership/default/configuration/scalar tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
