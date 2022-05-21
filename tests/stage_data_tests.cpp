#include "okage/stage_data.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F&& f) {
    try { f(); } catch(const std::runtime_error&) { return; }
    throw std::runtime_error("expected malformed STG rejection");
}
void put32(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=static_cast<std::uint8_t>(v>>(8*i));
}
std::uint32_t get32(okage::Bytes b,std::size_t at) {
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(0x278,0xa5);
    put32(b,0,0x00475453); put32(b,8,0x20); put32(b,12,3);
    put32(b,16,0x5c); put32(b,20,2); put32(b,24,0xe8); put32(b,28,2);
    for(unsigned i=0;i<3;++i) {
        const auto at=0x20+i*20;
        put32(b,at,0x44); put32(b,at+4,0x3f800000+i);
        put32(b,at+8,0x7fc00000); put32(b,at+12,0x12340000+i); put32(b,at+16,0xffffffff);
    }
    for(unsigned i=0;i<7*7;++i)put32(b,0x5c+i*4,0x1000+i);
    return b;
}
void diagnostic(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("cannot open diagnostic STG");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
    auto stage=okage::StageData::parse(bytes);
    require(bytes.size()==868,"diagnostic B_ROOM1 size changed");
    require(stage.settings().size()==18,"diagnostic B_ROOM1 settings count changed");
    require(stage.camera_records().size()==8,"diagnostic B_ROOM1 camera count changed");
    require(stage.camera_settings().size()==9,"diagnostic B_ROOM1 intermediate camera-settings count changed");
    require(stage.entrances().empty(),"diagnostic B_ROOM1 entrance count changed");
}
}

int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--diagnostic") {
            diagnostic(argv[2]); std::cout<<"B_ROOM1 STG diagnostic passed\n"; return 0;
        }
        require(argc==1,"usage: stage-data-tests [--diagnostic owned-B_ROOM1-a.stg]");
        auto source=fixture();
        const auto original=source;
        auto stage=okage::StageData::parse(source);
        require(stage.bytes().size()==source.size(),"full STG bytes not retained");
        require(stage.header().size()==okage::StageData::original_header_size,"header extent wrong");
        require(std::equal(stage.header().begin(),stage.header().end(),original.begin()),"header copy differs");
        require(stage.settings().size()==3,"setting count/order lost");
        require(stage.settings()[0].kind==0x44&&stage.settings()[1].kind==0x44,"duplicate settings reordered");
        require(stage.settings()[2].value_bits==std::array<std::uint32_t,4>{0x3f800002,0x7fc00000,0x12340002,0xffffffff},"setting bit patterns changed");
        require(stage.camera_records().size()==2,"camera record count wrong");
        require(stage.camera_records()[1].words[0]==0x1007,"camera record order wrong");
        require(stage.camera_settings().size()==3,"intermediate record count wrong");
        require(stage.camera_settings()[2].words[0]==0x101c,"intermediate record order wrong");
        require(stage.entrances().size()==2&&stage.entrances()[1].words[0]==0x102a,"entrance record order wrong");
        source[0]=0;source[0x20]=0;source.clear();
        require(get32(stage.bytes(),0)==0x00475453&&stage.settings()[0].kind==0x44,"stage ownership depends on source buffer");

        auto bad=original; bad.resize(0x277); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,0,0); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,8,0x21); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,16,0x40); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,24,0x90); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,12,0xffffffff); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,20,31); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,28,0xffffffff); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,24,0xe4); rejects([&]{(void)okage::StageData::parse(bad);});
        bad=original; put32(bad,24,0x277); rejects([&]{(void)okage::StageData::parse(bad);});
        std::cout<<"stage data tests passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
