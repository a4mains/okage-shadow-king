// Optional owned-media check; no device playback, reference save or emulator.
#include "okage/pcm_stream.hpp"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    if(size>32*1024*1024)throw std::runtime_error("Owned PCM input limit");
    std::vector<std::uint8_t> bytes(size);
    std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("PCM input read");
    return bytes;
}
std::int16_t sample(okage::Bytes bytes,std::size_t at) {
    const auto word=std::uint16_t(bytes[at])|std::uint16_t(bytes[at+1])<<8;
    return std::bit_cast<std::int16_t>(std::uint16_t(word));
}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
int main(int argc,char** argv) {try {
    if(argc!=4)throw std::runtime_error("Usage: pcm-stream-owned-tests THEMA_1.XWH THEMA_1.XWB PCM_GAIN_Q12.bin");
    const auto header=read(argv[1]),body=read(argv[2]),table=read(argv[3]);
    require(table.size()==514,"Owned gain table extent");
    std::array<std::int16_t,257> gains{};
    for(unsigned i=0;i<gains.size();++i)gains[i]=sample(table,i*2);
    okage::TitlePcmStream whole(header,body,gains),partitioned(header,body,gains);
    require(whole.frame_count()==5796569&&whole.loop_start_frame()==866941,"Owned title stream bounds");
    require(body.size()==whole.frame_count()*4+12,"Owned title trailing extent");
    const std::size_t frames=whole.frame_count()+4096;
    std::vector<std::int16_t> output(frames*2),pieces(frames*2);
    whole.mix(output);
    // Alternate blocks cross both source 1024-frame pages and the loop edge.
    constexpr std::array<std::size_t,5> blocks{1,799,1024,1537,4093};
    for(std::size_t at=0,i=0;at<frames;++i) {
        const auto count=std::min(blocks[i%blocks.size()],frames-at);
        partitioned.mix(std::span(pieces).subspan(at*2,count*2));at+=count;
    }
    require(output==pieces,"Owned PCM host block independence");
    for(std::size_t i=0;i<frames;++i) {
        const auto source=i<whole.frame_count()?i:whole.loop_start_frame()+i-whole.frame_count();
        require(output[i*2]==sample(body,source*4)&&output[i*2+1]==sample(body,source*4+2),
                "Owned title unity samples differ from bounded little-endian input");
    }
    require(whole.cursor_frame()==whole.loop_start_frame()+4096&&whole.cursor_frame()==partitioned.cursor_frame(),
            "Owned title loop cursor");
    std::cout<<"Owned title PCM passed: "<<frames<<" stereo frames, full bounded body plus 4096 loop frames; arbitrary host blocks agree. No playback/timing claim.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
