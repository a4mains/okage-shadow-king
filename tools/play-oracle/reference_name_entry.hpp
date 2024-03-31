#pragma once
#include "ee/Ee_SubSystem.h"
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

class NameEntryTrace {
public:
    static constexpr uint32 constructor=0x124548,first=0x11a1c8,firstReturn=0x11ff38;
    static constexpr uint32 second=0x119700,secondReturn=0x120b1c;
    static constexpr uint32 padRead=0x11a230,padNormalized=0x11a274;
    static constexpr std::array<uint32,7> breakpoints={constructor,first,firstReturn,second,secondReturn,padRead,padNormalized};
    static constexpr unsigned maxRows=20000;
    static constexpr std::size_t maxBytes=128u*1024u*1024u;
    explicit NameEntryTrace(const std::filesystem::path& path) {
        if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))fail("output exists or is a symlink");
        output_.open(path);if(!output_)fail("cannot create output");
        emit("{\"schema\":\"okage-name-entry-v1\",\"event\":\"header\",\"row_limit\":20000,\"byte_limit\":134217728,"
             "\"elf_sha256\":\"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5\","
             "\"scope\":\"selector9 constructor entry and paired type9 first/second handlers; frame is boot local VBlank counter, no anchor inference\","
             "\"timing\":\"first return11ff38 is caller continuation after11b2bc cursor store and epilogue; second return120b1c is caller continuation. No interior11b2c0 breakpoint\","
             "\"raw_policy\":\"manager bytes include raw guest pointers and untouched storage, never host-link equality; no normalization or omission inside raw blocks\","
             "\"pad_scope\":\"first entry snapshots before reads1c0160/1c01d0; pressed33958e/repeat339592 are raw latches, before123820/122f98 normalization; pad_read snapshots actual getter outputs before normalization;pad_normalized snapshots the four normalized stack words;returns and second are snapshots only\","
             "\"shared_scope\":\"name is block2(shared variables+510)+334,8bytes;24eb98 is only a4byte prefix, not an allocation extent; thread is Play BIOS_STATE4014\"}");
    }
    void observe(Ee::CSubSystem& ee,unsigned frame) {
        Memory m{ee};const auto& cpu=ee.m_EE.m_State;
        const uint32 pc=cpu.nPC;
        auto reg=[&](unsigned i){return uint32(cpu.nGPR[i].nD0);};
        const auto sp=reg(CMIPS::SP),ra=reg(CMIPS::RA),thread=m.thread();
        if(pc==constructor) {
            const auto owner=reg(CMIPS::A0);m.require(owner,0x50,"constructor owner");
            const auto parent=m.word(owner+0x14);m.require(parent,0x54,"constructor parent");
            if(m.word(parent+0x4c)!=9)return; //124548 also constructs selector10.
            if(active_)fail("constructor nested in active handler");
            snapshot(m,frame,pc,sp,ra,thread,"constructor_entry",0,owner,0,cpu.nFCSR);
            ++constructors_;return;
        }
        if(pc==padRead||pc==padNormalized) {
            if(!active_||active_->kind!=0||active_->thread!=thread||active_->sp<0xd0||
               sp!=active_->sp-0xd0||reg(CMIPS::S1)!=active_->manager)
                fail("input continuation owner/thread/SP mismatch");
            const unsigned expected=pc==padRead?0:1;
            if(active_->input_stage!=expected)fail("input continuation order");
            ++active_->input_stage;
            snapshot(m,frame,pc,sp,ra,thread,pc==padRead?"pad_read":"pad_normalized",
                     active_->manager,active_->owner,active_->id,cpu.nFCSR,pc==padRead?std::optional<uint32>(reg(CMIPS::V0)):std::nullopt);
            return;
        }
        if(pc==firstReturn||pc==secondReturn) {
            if(!active_||active_->thread!=thread)return; //Other generic manager calls.
            const auto call=*active_;
            if(pc!=call.return_pc||sp!=call.sp||ra!=call.return_pc)
                fail("handler continuation PC/SP/RA mismatch");
            if(call.kind==0&&call.input_stage!=2)fail("missing consumed-input boundaries");
            validate_manager(m,call.manager,call.owner);
            snapshot(m,frame,pc,sp,ra,thread,call.kind==0?"first_return":"second_return",
                     call.manager,call.owner,call.id,cpu.nFCSR);
            ++completed_[call.kind];active_.reset();return;
        }
        if(pc!=first&&pc!=second)fail("unexpected observer PC");
        if(active_)fail("nested name-entry handler");
        const unsigned kind=pc==first?0:1;
        const auto argument=reg(CMIPS::A0);
        if(kind==1&&argument<0x100040)fail("second layer pointer underflow");
        const uint32 manager=kind==0?argument:argument-0x40;
        m.require(manager,0xef0,"manager");
        const uint32 owner=m.word(manager+0x14);
        validate_manager(m,manager,owner);
        if(!(m.word(manager+0x90)&0x40))fail("handler layer is not enabled");
        const auto expected=kind==0?firstReturn:secondReturn;
        if(ra!=expected)fail("handler caller is not the recovered generic dispatcher");
        if(kind==0&&(m.word(manager+0xe48)!=0||m.word(manager+0xe8c)!=0||m.word(manager+0x114)!=manager))
            fail("first dispatcher layer/cursor/backlink mismatch");
        Call call{manager,owner,sp,thread,expected,++calls_,kind,0};active_=call;++entered_[kind];
        snapshot(m,frame,pc,sp,ra,thread,kind==0?"first_entry":"second_entry",
                 manager,owner,call.id,cpu.nFCSR);
    }
    void finish() {
        if(active_||entered_!=completed_||!completed_[0])fail("incomplete or absent first-handler pairs");
        std::ostringstream s;s<<"{\"event\":\"finish\",\"constructors\":"<<constructors_
            <<",\"first_pairs\":"<<completed_[0]<<",\"second_pairs\":"<<completed_[1]<<",\"rows\":"<<rows_<<'}';emit(s.str());
    }
private:
    struct Memory {
        Ee::CSubSystem& ee;
        void require(uint32 at,uint32 size,const char* key) const {
            if(at<0x100000||size>PS2::EE_RAM_SIZE||at>PS2::EE_RAM_SIZE-size) {
                std::ostringstream s;s<<key<<" outside RAM address0x"<<std::hex<<at<<" size0x"<<size;fail(s.str());
            }
        }
        uint32 word(uint32 at) const {require(at,4,"word");if(at&3)fail("unaligned word");uint32 v;std::memcpy(&v,ee.m_ram+at,4);return v;}
        uint32 thread() const {uint32 v;std::memcpy(&v,ee.m_ram+0x4014,4);return v;}
        void raw(std::ostringstream& s,const char* key,uint32 at,uint32 size) const {
            require(at,size,key);static constexpr char hex[]="0123456789abcdef";
            s<<",\""<<key<<"\":{\"address\":"<<at<<",\"size\":"<<size<<",\"hex\":\"";
            for(uint32 i=0;i<size;++i){auto v=ee.m_ram[at+i];s<<hex[v>>4]<<hex[v&15];}s<<"\"}";
        }
    };
    struct Call {uint32 manager,owner,sp,thread,return_pc,id;unsigned kind,input_stage;};
    std::optional<Call> active_;
    std::ofstream output_;std::size_t bytes_=0;unsigned rows_=0,calls_=0,constructors_=0,lastFrame_=0;
    std::array<unsigned,2> entered_{},completed_{};
    [[noreturn]] static void fail(const std::string& why){throw std::runtime_error("Name-entry observer: "+why);}
    void emit(const std::string& row) {
        if(row.size()+1>maxBytes-bytes_)fail("output byte limit");
        output_<<row<<'\n';output_.flush();if(!output_)fail("output write failure");bytes_+=row.size()+1;
    }
    static void validate_manager(const Memory& m,uint32 manager,uint32 owner) {
        m.require(manager,0xef0,"manager");m.require(owner,0x50,"mode9 owner");
        if(m.word(manager)!=0x11fec8||m.word(manager+4)!=0x120ad0||m.word(manager+8)!=0x120b48||
           m.word(manager+0xe40)!=1||m.word(manager+0xe90)!=0||m.word(manager+0xa0)!=9||m.word(manager+0x9c)!=0x119700||
           m.word(owner+0x24)!=9||m.word(owner+0x34)!=manager)
            fail("manager/type9/owner identity mismatch");
    }
    void snapshot(const Memory& m,unsigned frame,uint32 pc,uint32 sp,uint32 ra,uint32 thread,
                  const char* edge,uint32 manager,uint32 owner,unsigned call,uint32 fcsr,std::optional<uint32> held={}) {
        if(rows_>=maxRows)fail("row limit");
        if(rows_&&frame<lastFrame_)fail("frame labels moved backwards");lastFrame_=frame;
        std::ostringstream s;s<<"{\"event\":\"name_entry\",\"sequence\":"<<rows_++<<",\"frame\":"<<frame
            <<",\"edge\":\""<<edge<<"\",\"call_id\":"<<call<<",\"pc\":"<<pc<<",\"sp\":"<<sp
            <<",\"ra\":"<<ra<<",\"thread\":"<<thread<<",\"fcsr\":"<<fcsr<<",\"manager\":"<<manager<<",\"owner\":"<<owner;
        m.raw(s,"owner_raw",owner,0x50);
        if(manager) {
            m.raw(s,"manager_raw",manager,0xef0);
            s<<",\"state_signed\":"<<std::bit_cast<std::int32_t>(m.word(manager+0xe4c))
             <<",\"state_cursor\":"<<m.word(manager+0xe8c)<<",\"layer_flags\":"<<m.word(manager+0x90)
             <<",\"command_signed\":"<<std::bit_cast<std::int32_t>(m.word(manager+0xa4))
             <<",\"argument_signed\":"<<std::bit_cast<std::int32_t>(m.word(manager+0xa8));
            const auto handle=m.word(manager+0x40);if(handle>=8)fail("window registry handle");
            const auto window=m.word(0x200688+4*handle);m.require(window,0x6ec0,"window");
            s<<",\"window_handle\":"<<handle<<",\"window_pointer\":"<<window
             <<",\"opacity_bits\":"<<m.word(window+0x270)<<",\"opacity_delta_bits\":"<<m.word(window+0x27c);
        }
        if(pc==padRead||pc==padNormalized) {
            m.require(sp,0x20,"handler pad stack");
            s<<",\"consumed_pad\":{\"pressed\":"<<m.word(sp+0x10)<<",\"released\":"<<m.word(sp+0x14)
             <<",\"held\":"<<(held?*held:m.word(sp+0x18))<<",\"repeat\":"<<m.word(sp+0x1c)
             <<",\"normalized\":"<<(pc==padNormalized?"true":"false")<<'}';
        }
        const auto vars=m.word(0x24f788);m.require(vars,0xd78,"shared variables");
        m.raw(s,"name_bytes",vars+0x510+0x334,8);m.raw(s,"selected_text_prefix",0x24eb98,4);
        s<<",\"shared_1f88c0\":"<<m.word(0x1f88c0)<<",\"mode\":"<<m.word(0x24f738);
        m.raw(s,"pad_raw",0x33958c,8);m.raw(s,"input_masks",0x1f85c8,64);
        s<<",\"pressed_raw\":"<<(m.word(0x33958c)>>16)<<",\"repeat_raw\":"<<(m.word(0x339590)>>16);
        const auto rng=m.word(0x2151b4);m.require(rng,0x5c,"RNG owner");s<<",\"rng_word\":"<<m.word(rng+0x58)<<'}';
        emit(s.str());
    }
};
