#pragma once
#include "ee/Ee_SubSystem.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class BattleUiTrace {
public:
    static constexpr uint32 entryPc=0x11f5f8;
    static constexpr uint32 continuationPc=0x14a2c8;
    static constexpr std::array<uint32,2> breakpoints={entryPc,continuationPc};

    explicit BattleUiTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))
            throw std::runtime_error("Refusing existing battle-UI trace");
        output.open(path);
        if(!output)throw std::runtime_error("Cannot create battle-UI trace");
        output<<"{\"schema\":\"okage-battle-ui-v1\",\"scope\":\"one natural 11f5f8 construction inside 14a248 phase zero, observed before activation\","
            <<"\"boundary\":\"11f5f8 entry; 14a2c8 caller continuation after the call delay slot, before caller instruction\","
            <<"\"breakpoint_delivery\":\"11f5f8 is constructor function entry; 14a2c8 is the instruction following JAL at 14a2c0 and its delay slot at 14a2c4\","
            <<"\"capture_scope\":\"RUW owner and all runtime/text pools; controller registry and 0x6ec0 registered owners; manager 0xef0 and two 0xe0 layer bodies; root 0x81d0 and camera 0x150; bounded source-written stream prefix and atlas records\","
            <<"\"stream_extent_note\":\"Only the source-established first five words (20 bytes) are copied; this is not an allocation-boundary claim\","
            <<"\"atlas_extent_note\":\"The first all-zero 16-byte row terminates the source scan, at most 32 rows; no empty-table assumption is made\","
            <<"\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\"}\n";
        flush();
    }

    void finish() {
        if(entryCount!=1||completed!=1||active)
            fail("requires exactly one paired UI constructor entry and caller continuation");
        output<<"{\"event\":\"finish\",\"constructors_entered\":"<<entryCount
              <<",\"constructors_completed\":"<<completed<<",\"snapshots\":"<<rows<<"}\n";
        flush();
    }

    void observe(Ee::CSubSystem& ee,unsigned frame) {
        const auto& cpu=ee.m_EE.m_State;
        const auto pc=uint32(cpu.nPC),sp=uint32(cpu.nGPR[CMIPS::SP].nD0);
        auto reg=[&](unsigned index){return uint32(cpu.nGPR[index].nD0);};
        Memory memory{ee};
        if(pc==entryPc) {
            if(active||entryCount||completed)fail("duplicate or nested 11f5f8 entry");
            const auto root=memory.word(0x1fce60);
            memory.require(root,0x81d0);
            if(reg(CMIPS::RA)!=continuationPc||reg(CMIPS::A0)!=root||reg(CMIPS::A1)!=0x248100||
               memory.word(root+0x24)!=0)
                fail("11f5f8 is not the reviewed phase-zero caller");
            validateInterfaceTable(memory);
            memory.require(0x1fae18,0x120);
            active=Call{root,sp,{}};
            std::memcpy(active->partySelection.data(),ee.m_ram+0x1fae18,active->partySelection.size());
            ++entryCount;
            snapshot("ui_constructor_entry",frame,cpu,memory,false,memory.word(root+0x8060));
            return;
        }
        if(pc==continuationPc) {
            if(!active||completed||sp!=active->sp||reg(CMIPS::S1)!=active->root||memory.word(0x1fce60)!=active->root)
                fail("unpaired 14a2c8 caller continuation");
            const auto manager=reg(CMIPS::V0);
            if(!manager)
                fail("UI manager return does not match the battle root");
            memory.require(manager,0xef0);
            validateManager(memory,manager);
            snapshot("ui_constructor_complete_before_activation",frame,cpu,memory,true,manager);
            ++completed;active.reset();return;
        }
        fail("unexpected UI observer breakpoint");
    }

private:
    static constexpr uint32 ruwSize=19596;
    static constexpr uint32 runtimeBytes=240*44;
    static constexpr uint32 textBytes=73*128;
    static constexpr uint32 controllerBytes=0x6ec0;
    static constexpr std::array<uint32,6> expectedWidgetCounts={21,4,56,72,46,57};
    struct Call {uint32 root,sp;std::array<uint8,0x120> partySelection{};};
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 size) const {
            if(at<0x100000||size>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-size)
                fail("snapshot range outside EE RAM");
        }
        uint32 word(uint32 at) const {
            require(at,4);uint32 value;std::memcpy(&value,ee.m_ram+at,4);return value;
        }
        void bytes(std::ostringstream& row,const char* name,uint32 at,uint32 size) const {
            require(at,size);static constexpr char digits[]="0123456789abcdef";
            row<<",\""<<name<<"\":{\"address\":"<<at<<",\"bytes\":"<<size<<",\"raw_hex\":\"";
            for(uint32 i=0;i<size;++i){const auto b=ee.m_ram[at+i];row<<digits[b>>4]<<digits[b&15];}
            row<<"\"}";
        }
        bool zero16(uint32 at) const {
            require(at,16);for(unsigned i=0;i<16;++i)if(ee.m_ram[at+i])return false;return true;
        }
    };
    std::ofstream output;
    std::optional<Call> active;
    unsigned entryCount=0,completed=0,rows=0;

    [[noreturn]] static void fail(const char* message) {
        throw std::runtime_error(std::string("Battle-UI observer: ")+message);
    }
    void flush() {output.flush();if(!output)fail("trace write failure");}
    static void validateInterfaceTable(const Memory& m) {
        static constexpr std::array<std::array<uint32,8>,2> expected={{
            {{0x1fd820,0x1fdba8,0x163fb8,0x164090,2,29,17,0}},
            {{0x1fdb88,0x1fdba8,0x168228,0x1684d0,3,13,8,0}}
        }};
        for(unsigned i=0;i<2;++i) {
            const auto at=0x248100+i*112;
            m.require(at,112);
            if(m.word(at)!=expected[i][0]||m.word(at+4)!=40||m.word(at+8)!=6||m.word(at+12)!=2||
               m.word(at+0x54)!=expected[i][1]||m.word(at+0x58)!=expected[i][2]||
               m.word(at+0x5c)!=expected[i][3]||m.word(at+0x60)!=expected[i][4]||
               m.word(at+0x64)!=expected[i][5]||m.word(at+0x68)!=expected[i][6]||m.word(at+0x6c)!=0)
                fail("authored interface row differs from pinned two-record scope");
        }
        if(m.word(0x2481e0)!=0)fail("pinned interface terminator is absent");
    }
    static void validateRuw(const Memory& m,uint32 raw,uint32 runtime,uint32 text) {
        m.require(raw,ruwSize);m.require(runtime,runtimeBytes);m.require(text,textBytes);
        static constexpr unsigned char magic[8]={'r','x','u','w','1','0','0','0'};
        if(std::memcmp(m.ee.m_ram+raw,magic,8))fail("slot-zero RUW signature mismatch");
        const auto meta=0x200750;
        if(m.word(meta)!=87||m.word(meta+4)!=1||m.word(meta+8)!=6||
           m.word(meta+0x1c)!=raw+48||m.word(meta+0x20)!=raw+1440||m.word(meta+0x24)!=raw+2188)
            fail("slot-zero RUW metadata/source ranges differ");
        const auto templateCounts=m.word(meta+0x0c),templateOffsets=m.word(meta+0x10);
        const auto widgetCounts=m.word(meta+0x14),widgetOffsets=m.word(meta+0x18);
        m.require(templateCounts,4);m.require(templateOffsets,4);
        m.require(widgetCounts,24);m.require(widgetOffsets,24);
        if(m.word(templateCounts)!=11||m.word(templateOffsets)!=0)fail("template group count/offset mismatch");
        uint32 sum=0,runtimeCount=0,textCount=0;
        for(unsigned group=0;group<6;++group) {
            const auto count=m.word(widgetCounts+group*4),offset=m.word(widgetOffsets+group*4);
            if(count!=expectedWidgetCounts[group]||offset!=sum)fail("RUW live group counts or offsets mismatch");
            sum+=count;
            for(unsigned item=0;item<count;++item) {
                const auto record=m.word(meta+0x24)+(offset+item)*68;
                m.require(record,68);
                const auto type=m.word(record+0x14),flags=m.word(record+0x10),owner=m.word(record+0x40);
                const bool ownsRuntime=type==1||(type<=3&&(flags&0x40));
                if(ownsRuntime) {
                    const auto expected=runtime+runtimeCount*44;
                    if(owner!=expected)fail("RUW runtime record ownership/order mismatch");
                    if(type==1) {
                        if(m.word(owner+0x28)!=text+textCount*128)fail("RUW text owner ordering mismatch");
                        ++textCount;
                    }
                    ++runtimeCount;
                } else if(type<=3&&owner!=0)fail("unallocated recognized RUW record has runtime pointer");
            }
        }
        if(sum!=256||runtimeCount!=240||textCount!=73)fail("RUW runtime/text pool extents differ from pinned resource");
    }
    static std::pair<uint32,bool> atlasExtent(const Memory& m) {
        uint32 rows=0;
        for(;rows<32;++rows) {
            const auto at=0x1fdba8+rows*16;
            m.require(at,16);
            if(m.zero16(at))return {(rows+1)*16,true};
        }
        return {32*16,false};
    }
    static void captureAtlas(std::ostringstream& row,const Memory& m,bool post) {
        const auto [extent,terminated]=post?atlasExtent(m):std::pair<uint32,bool>{16,m.zero16(0x1fdba8)};
        row<<",\"atlas_rows_scanned\":"<<extent/16<<",\"atlas_terminator_found\":"<<(terminated?"true":"false");
        m.bytes(row,"atlas_region",0x1fdba8,extent);
    }
    static void captureControllerRegistry(std::ostringstream& row,const Memory& m,bool includeWindows) {
        const auto count=m.word(0x2006b0);
        if(count>7)fail("UI controller count exceeds source count cap");
        m.bytes(row,"controller_registry",0x200688,32);
        row<<",\"controller_count\":"<<count<<",\"controller_windows\":[";
        for(unsigned slot=0;slot<8;++slot) {
            if(slot)row<<',';
            const auto window=m.word(0x200688+slot*4);
            row<<"{\"slot\":"<<slot<<",\"pointer\":"<<window;
            if(window&&includeWindows)m.bytes(row,"raw",window,controllerBytes);
            row<<'}';
        }
        row<<']';
    }
    static void captureRuw(std::ostringstream& row,const Memory& m) {
        const auto raw=m.word(0x200708),runtime=m.word(0x200ac8),text=m.word(0x200b08);
        validateRuw(m,raw,runtime,text);
        row<<",\"ruw_slot0\":{\"raw_pointer\":"<<raw<<",\"runtime_pointer\":"<<runtime
           <<",\"text_pointer\":"<<text;
        m.bytes(row,"metadata",0x200750,0x30);
        const auto meta=0x200750;
        m.bytes(row,"template_group_counts",m.word(meta+0x0c),4);
        m.bytes(row,"template_group_offsets",m.word(meta+0x10),4);
        m.bytes(row,"widget_group_counts",m.word(meta+0x14),24);
        m.bytes(row,"widget_group_offsets",m.word(meta+0x18),24);
        m.bytes(row,"raw_owner",raw,ruwSize);
        m.bytes(row,"runtime_owner",runtime,runtimeBytes);
        m.bytes(row,"text_owner",text,textBytes);
        row<<'}';
        m.bytes(row,"ruw_raw_slots",0x200708,64);
        m.bytes(row,"ruw_runtime_slots",0x200ac8,64);
        m.bytes(row,"ruw_text_slots",0x200b08,64);
        m.bytes(row,"ruw_ready_slot0",0x332b98,4);
    }
    static void captureGlobals(std::ostringstream& row,const Memory& m) {
        row<<",\"shared_globals\":{";
        static constexpr std::array<std::pair<const char*,uint32>,14> words={{
            {"type2_flag_a",0x1fd800},{"type2_flag_b",0x1fd814},{"type2_enabled",0x1fd81c},
            {"type3_ready",0x1fdb78},{"type3_reset",0x1fdb7c},{"type3_animation",0x2e18a4},
            {"type3_selection",0x2e18a8},{"battle_ui_texture",0x24f730},
            {"atlas_texture_owner",0x24f734},{"battle_root_pointer",0x1fce60},
            {"ui_menu_state",0x1fd888},{"ui_mode_counter",0x1fd7fc},
            {"ui_bank_pointer",0x1fdb74},{"ui_selection_pointer",0x1fd770}
        }};
        for(unsigned i=0;i<words.size();++i) {
            if(i)row<<',';
            row<<'"'<<words[i].first<<"\":"<<m.word(words[i].second);
        }
        row<<'}';
        m.bytes(row,"type3_selection_words",0x2e1868,0x44);
        m.bytes(row,"battle_ui_party_refresh_state",0x1fd770,0x20);
        m.bytes(row,"party_selection_counts_and_table",0x1fae18,0x120);
        m.bytes(row,"controller_stream_state",0x1f9ca0,8);
    }
    static void validateManager(const Memory& m,uint32 manager) {
        if(m.word(manager+0xe40)!=2||m.word(manager+0xe90)!=0||m.word(manager+0xe94)!=1)
            fail("manager does not contain the two source-ordered interface layers");
        for(unsigned index=0;index<2;++index) {
            const auto layer=manager+0x40+index*0xe0,record=0x248100+index*112;
            m.require(layer,0xe0);
            const auto handle=m.word(layer),type=m.word(layer+0x60),callback=m.word(layer+0x5c);
            const auto expectedType=index?3u:2u,expectedCallback=index?0x1684d0u:0x164090u;
            if(handle>=8||m.word(layer+0x54)!=m.word(record)||type!=expectedType||callback!=expectedCallback)
                fail("manager layer does not match authored interface row");
            const auto window=m.word(0x200688+handle*4);
            m.require(window,controllerBytes);
            if(m.word(window+4)!=handle||m.word(window+0x2698)!=m.word(record)||
               m.word(window+0x3c00)!=m.word(record+0x64)||m.word(window+0x3c04)!=m.word(record+0x68))
                fail("controller window does not match authored stream and capacities");
        }
    }
    template<class State>
    void snapshot(const char* event,unsigned frame,const State& cpu,const Memory& m,bool post,uint32 manager) {
        if(++rows>2)fail("snapshot row limit exceeded");
        const auto& call=*active;const auto root=call.root;
        std::ostringstream row;
        row<<"{\"event\":\""<<event<<"\",\"sequence\":"<<rows-1<<",\"frame\":"<<frame
           <<",\"pc\":"<<uint32(cpu.nPC)<<",\"ra\":"<<uint32(cpu.nGPR[CMIPS::RA].nD0)
           <<",\"sp\":"<<uint32(cpu.nGPR[CMIPS::SP].nD0)<<",\"fcsr\":"<<cpu.nFCSR<<",\"root\":"<<root
           <<",\"constructor_sp\":"<<call.sp<<",\"post_constructor\":"<<(post?"true":"false");
        const auto camera=m.word(root+0x8068);m.require(camera,0x150);
        if(post)m.require(manager,0xef0);
        row<<",\"manager\":"<<manager<<",\"camera\":"<<camera;
        m.bytes(row,"battle_root",root,0x81d0);
        m.bytes(row,"battle_root_81ac",root+0x81ac,4);
        m.bytes(row,"battle_camera",camera,0x150);
        if(post) {
            m.bytes(row,"manager_raw",manager,0xef0);
            for(unsigned i=0;i<2;++i)m.bytes(row,i?"layer1":"layer0",manager+0x40+i*0xe0,0xe0);
        }
        m.bytes(row,"authored_interface_records_and_terminator",0x248100,0xe4);
        captureRuw(row,m);
        captureControllerRegistry(row,m,true);
        captureAtlas(row,m,post);
        m.bytes(row,"stream_type2_written_extent",0x1fd820,20);
        m.bytes(row,"stream_type3_written_extent",0x1fdb88,20);
        captureGlobals(row,m);
        row<<",\"party_selection_cache_unchanged_since_entry\":"
           <<(std::memcmp(call.partySelection.data(),m.ee.m_ram+0x1fae18,call.partySelection.size())==0?"true":"false");
        const auto rng=m.word(0x2151b4);m.require(rng,0x5c);
        row<<",\"rng_pointer\":"<<rng;
        m.bytes(row,"rng_state",rng,0x5c);
        row<<"}\n";
        output<<row.str();flush();
    }
};
