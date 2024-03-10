#include "okage/title_stream.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F&& f, const char* message) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
void u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = std::uint8_t(value);
    bytes.at(offset + 1) = std::uint8_t(value >> 8);
}
void u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (8 * i));
}
void tag(std::vector<std::uint8_t>& bytes, std::size_t offset, const char* text) {
    std::memcpy(bytes.data() + offset, text, 4);
}

struct Fixture {
    std::vector<std::uint8_t> xwh = std::vector<std::uint8_t>(112);
    std::vector<std::uint8_t> xwb = std::vector<std::uint8_t>(16);
    std::array<std::int16_t, okage::TitlePcmStream::gain_table_size> gains{};
    Fixture() {
        tag(xwh,0,"RXWS");u32(xwh,4,96);u32(xwh,8,0x200);
        tag(xwh,16,"FORM");u32(xwh,20,32);u32(xwh,24,0x100);
        u32(xwh,32,1);xwh[36]=1;xwh[37]=0x1c;xwh[38]=3;xwh[40]=127;xwh[41]=127;
        xwh[45]=2;u16(xwh,46,48000);u32(xwh,52,0);u32(xwh,56,16);u32(xwh,60,8);
        tag(xwh,64,"FTXT");u32(xwh,68,32);u32(xwh,72,0x100);
        u32(xwh,80,1);u32(xwh,84,8);std::memcpy(xwh.data()+88,"thema_1.aif",12);
        u16(xwb,0,1000);u16(xwb,2,std::uint16_t(-1000));
        u16(xwb,4,2000);u16(xwb,6,std::uint16_t(-2000));
        u16(xwb,8,3000);u16(xwb,10,std::uint16_t(-3000));
        u16(xwb,12,4000);u16(xwb,14,std::uint16_t(-4000));
        for (int i=0;i<257;++i) gains[i]=std::int16_t((i-128)*32);
        gains[255]=4096; //authored127 at start, unity Q12
        gains[254]=2048;
    }
};
}

int main() {
    try {
        Fixture f;
        okage::TitleStreamOwner owner;
        owner.start(1,f.xwh,f.xwb,f.gains);
        auto state=owner.state();
        check(state.registered&&!state.stop_pending&&state.slot==1&&
              state.stop_countdown==0&&state.stream_volume==128&&state.cursor_frame==0,
              "title start binds slot 1 with source startup gain");

        std::array<std::int16_t,2> first{};
        owner.mix(first);
        check(first==std::array<std::int16_t,2>{1000,-1000}&&owner.state().cursor_frame==1,
              "started title stream mixes owned stereo PCM");
        owner.request_stop(1);
        state=owner.state();
        check(state.registered&&state.stop_pending&&state.stop_countdown==128&&
              state.stream_volume==128,"12dd50 requests, but does not immediately stop");

        std::array<std::int16_t,2> before_update{};
        owner.mix(before_update);
        check(before_update==std::array<std::int16_t,2>{2000,-2000}&&
              owner.state().stop_countdown==128,
              "PCM block consumption does not advance main-loop stop timer");
        owner.advance_main_loop();
        state=owner.state();
        check(state.registered&&state.stop_pending&&state.stop_countdown==127&&
              state.stream_volume==127,"first 12ce20 update decrements before gain write");
        std::array<std::int16_t,2> faded{};
        owner.mix(faded);
        check(faded==std::array<std::int16_t,2>{1500,-1500},
              "first deferred-stop update publishes reduced stream gain");

        for(unsigned i=1;i<128;++i) owner.advance_main_loop();
        state=owner.state();
        check(!state.registered&&!state.stop_pending&&state.slot==0&&
              state.stop_countdown==0&&state.stream_volume==0,
              "128 main-loop updates stop and release slot");
        std::array<std::int16_t,2> silence{};
        owner.mix(silence);
        check(silence==std::array<std::int16_t,2>{0,0},"released stream contributes nothing");

        Fixture replacement;
        u16(replacement.xwb,0,7000);u16(replacement.xwb,2,std::uint16_t(-7000));
        owner.start(1,f.xwh,f.xwb,f.gains);
        owner.request_stop(1);
        for(unsigned i=0;i<9;++i) owner.advance_main_loop();
        check(owner.state().stop_countdown==119,"pending stop before replacement");
        owner.start(1,replacement.xwh,replacement.xwb,replacement.gains);
        state=owner.state();
        check(state.registered&&!state.stop_pending&&state.stop_countdown==0&&
              state.stream_volume==128&&state.cursor_frame==0,
              "12db50 replacement synchronously clears a pending stop");
        std::array<std::int16_t,2> replaced{};
        owner.mix(replaced);
        check(replaced==std::array<std::int16_t,2>{7000,-7000},
              "replacement starts at its own sample cursor");

        rejects([&]{owner.start(1,f.xwh,f.xwb,f.gains);},
                "active stream cannot be silently overwritten without stop request");
        rejects([&]{owner.start(0,f.xwh,f.xwb,f.gains);},"unsupported stream slot");
        rejects([&]{owner.request_stop(0);},"unsupported stop slot");

        owner.request_stop(1);
        auto invalid=f.xwh;tag(invalid,16,"MARK");
        rejects([&]{owner.start(1,invalid,f.xwb,f.gains);},"bad replacement XWH");
        state=owner.state();
        check(!state.registered&&!state.stop_pending,
              "replacement load failure retains source-ordered old-slot release");

        std::cout<<"Title slot ownership, deferred stop and restart ordering passed\n";
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
