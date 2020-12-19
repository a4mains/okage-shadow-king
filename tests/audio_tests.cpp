#include "okage/audio.hpp"
#include <iostream>
#include <stdexcept>

void require(bool ok) {if(!ok)throw std::runtime_error("Audio assertion failed");}
template<class F> void rejects(F f) {bool bad=false;try {f();}catch(const std::runtime_error&){bad=true;}require(bad);}
int main() {
    try {
        std::vector<std::uint8_t> block(16,0);block[1]=7;block[2]=0x87;
        const auto result=okage::decode_spu_sample(block);
        require(result.pcm.size()==28 && result.consumed==16 && result.end_flags==7 && result.loop_sample==0);
        require(result.pcm[0]==28672 && result.pcm[1]==-32768 && result.pcm[2]==0);
        // The signed residual is not rounded toward zero before prediction.
        block[0]=12;block[2]=0x8f;
        const auto shifted=okage::decode_spu_sample(block);
        require(shifted.pcm[0]==-1 && shifted.pcm[1]==-8);
        auto missing=block;missing[1]=0;rejects([&]{okage::decode_spu_sample(missing);});
        auto bad=block;bad[0]=0x50;rejects([&]{okage::decode_spu_sample(bad);});
        bad=block;bad[1]=0x80;rejects([&]{okage::decode_spu_sample(bad);});
        for(std::size_t n=0;n<16;++n)rejects([&]{okage::decode_spu_sample(okage::Bytes(block.data(),n));});
        auto two=block;two[1]=4;two.insert(two.end(),block.begin(),block.end());two[17]=3;
        const auto loop=okage::decode_spu_sample(two);
        require(loop.pcm.size()==56 && loop.loop_sample==0 && loop.end_flags==3);
        std::vector<std::uint8_t> saturated(32,0x77);
        saturated[0]=0x40;saturated[1]=0;
        std::fill(saturated.begin()+16,saturated.end(),0);saturated[16]=0x40;saturated[17]=1;
        const auto clipped=okage::decode_spu_sample(saturated);
        require(clipped.pcm[0]==28672 && clipped.pcm[1]==32767 && clipped.pcm[28]==31743);
        std::cout<<"SPU nibble order, rounding, end/loop metadata and malformed blocks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
