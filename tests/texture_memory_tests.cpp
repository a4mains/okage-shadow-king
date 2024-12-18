#include "okage/texture_memory.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    try {
        const auto& ct32=okage::gs_texture_format(okage::GsPixelStorage::Psmct32);
        const auto& t8=okage::gs_texture_format(okage::GsPixelStorage::Psmt8);
        require(ct32.block_width==8&&ct32.block_height==8&&ct32.occupancy_mask==0xf,
                "PSMCT32 format record");
        require(t8.block_width==16&&t8.block_height==16&&t8.occupancy_mask==0xf,
                "PSMT8 format record");
        require(t8.block_order[0]==0&&t8.block_order[2]==4&&t8.block_order[4]==16&&
                t8.block_order[8]==2&&t8.block_order[31]==31,"Pinned 32-slot allocator permutation");

        okage::Ps2TextureMemory memory;
        bool unknown=false;
        try { (void)memory.allocate(1,okage::GsPixelStorage::Psmt8,64,64); }
        catch(const std::logic_error&) { unknown=true; }
        require(unknown,"Unknown initial allocator state is not treated as empty");
        auto unknown_byte=memory.observe_byte(0);
        require(!unknown_byte.value&&!unknown_byte.allocation_state_known,
                "Initial GS byte and allocator ownership remain explicitly unknown");
        memory.initialize_empty_allocator();
        require(memory.available_count()==4096&&memory.free_queue_ids().front()==0&&
                memory.free_queue_ids().back()==4095,
                "Retail initializer provides a FIFO of 4096 allocation records");
        auto image=memory.allocate(1,okage::GsPixelStorage::Psmt8,64,64);
        require(image&&image->base_block==0&&image->buffer_width_64==2,
                "64x64 PSMT8 allocation uses TBW=2");
        const std::uint64_t expected_descriptor=(2ull<<14)|(0x13ull<<32)|(64ull<<38)|(64ull<<50);
        require(image->id==0&&image->descriptor==expected_descriptor&&memory.available_count()==4095,
                "Descriptor packs FIFO id, pixel TBW, PSM, width, and height");
        auto occupancy=memory.occupancy_bytes();
        require(occupancy[0]==0xff&&occupancy[1]==0xff&&occupancy[2]==0xff&&occupancy[3]==0xff&&
                occupancy.size()==8192,
                "Occupancy snapshot is the source packed nibble bitmap");
        auto outside=memory.block_address(image->id,36,64);
        require(outside.inside_vram&&outside.block_index==36&&outside.byte_address==36*256,
                "x36,y64 resolves to allocator block36");
        require(!memory.owner_of_byte(outside.byte_address),"Out-of-image block is not claimed");
        auto inside=memory.block_address(image->id,36,48);
        require(inside.block_index<24&&memory.owner_of_byte(inside.byte_address)==image->id,
                "Owned logical block resolves to allocation owner");

        const std::array<std::uint8_t,4> bytes{0x10,0x80,0xfe,0x00};
        memory.write_bytes(inside.byte_address+9,bytes);
        auto known=memory.observe_byte(inside.byte_address+10);
        require(known.value==0x80&&known.allocation_owner==image->id,"Explicit write gives known byte and owner");
        require(!memory.observe_byte(inside.byte_address+8).value,"Allocation does not initialize GS bytes");
        require(memory.adjust_references(image->id,2),"Reference adjustment is independent of free");
        auto held=memory.allocation(image->id);
        require(held&&held->references==2,"Reference increment");
        require(!memory.free_allocation(image->id),"Free does not implicitly decrement nonzero references");
        require(memory.adjust_references(image->id,-2),"Explicit reference decrement");
        require(memory.free_allocation(image->id),"Zero-reference source free releases occupancy");
        require(!memory.owner_of_byte(inside.byte_address),"Release clears block ownership");
        require(memory.observe_byte(inside.byte_address+10).value==0x80,
                "Release preserves previously written GS byte");

        auto first=memory.allocate(0,okage::GsPixelStorage::Psmct32,8,8);
        require(first&&first->id==1,"Allocation IDs are handed out from FIFO");
        require(first->descriptor==((1ull<<14)|(1ull<<20)|(8ull<<38)|(8ull<<50)),
                "PSMCT32 descriptor has pixel TBW=1 and native field positions");
        require(memory.adjust_references(first->id,0x7fffffff),"Large positive ref adjustment is accepted");
        require(memory.allocation(first->id)->references==0x7fff,
                "Reference count saturates at signed-halfword maximum");
        require(memory.adjust_references(first->id,-0x7fffffff),"Large negative ref adjustment is accepted");
        require(memory.allocation(first->id)->references==0,
                "Reference count clamps at zero after signed arithmetic");
        auto cross_format=memory.allocate(1,okage::GsPixelStorage::Psmt8,16,16);
        require(first&&cross_format&&cross_format->base_block!=first->base_block,
                "PSMCT32 and PSMT8 share conflicting GS block occupancy");
        require(memory.release(cross_format->id),"Release cross-format allocation");
        auto aligned=memory.allocate(2,okage::GsPixelStorage::Psmct32,8,8);
        require(aligned&&aligned->base_block%32==0,"Class2 placement aligns to32 slots");
        bool double_release=false;
        memory.release(first->id);
        try { memory.release(first->id); } catch(const std::out_of_range&) { double_release=true; }
        require(double_release,"Released allocation handle is not live");
        require(memory.release(aligned->id),"Release aligned test allocation");
        auto reused=memory.allocate(0,okage::GsPixelStorage::Psmct32,8,8);
        require(reused&&reused->id==4,"Freed records return to FIFO tail in release order");
        require(memory.release(reused->id),"Release reused FIFO record");

        okage::Ps2TextureMemory retry;
        retry.initialize_empty_allocator();
        auto blocker=retry.allocate(0,okage::GsPixelStorage::Psmct32,8,8);
        auto aligned_retry=retry.allocate(2,okage::GsPixelStorage::Psmct32,8,8);
        require(blocker&&aligned_retry&&aligned_retry->base_block==32&&
                retry.search_cursors()[0]==1,
                "Search cursor records first free scan position before aligned footprint retries");
        require(aligned_retry->buffer_width_64==1,
                "PSMCT32 TBW counts padded pixels, not row bytes");
        require(retry.release(aligned_retry->id)&&retry.release(blocker->id),
                "Release cursor-order fixtures");
        auto full=memory.allocate(0,okage::GsPixelStorage::Psmct32,1024,1024);
        require(full&&memory.block_address(full->id,1023,1023).block_index==0x3fff,
                "1024x1024 PSMCT32 reaches final 256-byte VRAM block");
        require(memory.release(full->id),"Release full-VRAM test allocation");

        okage::Ps2TextureMemory records;
        records.initialize_empty_allocator();
        for (std::uint32_t i=0;i<4096;++i) {
            auto a=records.allocate(0,okage::GsPixelStorage::Psmct32,8,8);
            require(a&&a->id==i,"Source allocator record limit/FIFO order");
        }
        require(records.available_count()==0&&
                !records.allocate(0,okage::GsPixelStorage::Psmct32,8,8),
                "No allocation record beyond the initialized 4096-record pool");
        require(records.free_allocation(7),"Free a zero-reference allocation record");
        auto reused_record=records.allocate(0,okage::GsPixelStorage::Psmct32,8,8);
        require(reused_record&&reused_record->id==7&&records.available_count()==0,
                "Freed allocation record is appended and reused in FIFO order");
        bool bad_write=false;
        try { memory.write_bytes(okage::Ps2TextureMemory::vram_bytes-1,bytes); }
        catch(const std::out_of_range&) { bad_write=true; }
        require(bad_write,"Writes cannot escape 4MiB GS memory");
        std::cout<<"texture memory tests passed\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& e) {
        std::cerr<<"texture memory tests failed: "<<e.what()<<'\n';
        return EXIT_FAILURE;
    }
}
