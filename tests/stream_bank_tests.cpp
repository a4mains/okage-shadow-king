#include "okage/stream_bank.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void require(bool ok){if(!ok)throw std::runtime_error("RXWS assertion");}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}require(bad);}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(i*8);}
int main(){try{
    std::vector<std::uint8_t> b(96);put(b,0,0x53575852);put(b,4,80);put(b,8,0x200);
    put(b,16,0x4d524f46);put(b,20,16);put(b,24,0x100);put(b,32,2);b[37]=4;b[41]=8;
    put(b,48,0x59444f42);put(b,52,32);put(b,56,0x100);
    auto bank=okage::read_stream_bank(b);require(bank.chunks[0]==32 && bank.chunks[4]==64 && bank.body_size==32);
    require(bank.records.size()==2 && bank.record(0).size()==4 && bank.record(1).size()==8);
    require(bank.record(-1).empty() && bank.record(2).empty());
    b[36]=123;require(bank.record(0)[0]==0);
    for(unsigned n=0;n<b.size();++n)rejects([&]{okage::read_stream_bank(okage::Bytes(b.data(),n));});
    auto bad=b;put(bad,4,79);rejects([&]{okage::read_stream_bank(bad);});
    bad=b;put(bad,8,0x100);rejects([&]{okage::read_stream_bank(bad);});
    bad=b;put(bad,52,0xffffffff);rejects([&]{okage::read_stream_bank(bad);});
    bad=b;put(bad,32,0xffffffff);rejects([&]{okage::read_stream_bank(bad);});
    bad=b;bad[37]=0;rejects([&]{okage::read_stream_bank(bad);});
    bad=b;bad[41]=9;rejects([&]{okage::read_stream_bank(bad);});
    bad=b;bad[41]=7;require(okage::read_stream_bank(bad).record(1).size()==7); // Zero alignment padding.
    bad[47]=1;rejects([&]{okage::read_stream_bank(bad);});
    bad=b;put(bad,48,0x4d524f46);rejects([&]{okage::read_stream_bank(bad);});
    bad=b;put(bad,48,0x99999999);rejects([&]{okage::read_stream_bank(bad);});
    std::vector<std::uint8_t> cue(96);put(cue,0,0x53575852);put(cue,4,80);put(cue,8,0x200);
    put(cue,16,0x4d524f46);put(cue,20,32);put(cue,24,0x100);put(cue,32,1);
    cue[37]=28;cue[38]=2;cue[40]=cue[41]=127;cue[45]=1;cue[46]=0x44;cue[47]=0xac;
    put(cue,56,16);put(cue,60,0xffffffff);put(cue,64,0x59444f42);put(cue,68,16);put(cue,72,0x100);
    cue[81]=1;for(unsigned i=82;i<96;++i)cue[i]=0x77;
    bank=okage::read_stream_bank(cue);auto tone=okage::stream_tone(bank,0);
    require(tone.offset==80 && tone.size==16 && tone.sample_rate==44100 && tone.pitch==3763 && tone.left==127);
    tone=okage::stream_tone(bank,0,.5f,64);require(tone.pitch==1881 && tone.left==63);
    auto flagged=bank;flagged.bytes[39]=0x80;
    const auto footstep=okage::stream_tone(flagged,0,1,60);
    require(footstep.pitch==3763 && footstep.left==59 && footstep.right==59);
    okage::StreamPlayer player(bank);player.play(0,1,128);require(player.active()==1);
    std::vector<std::int32_t> mix(200);player.mix(mix);require(player.active()==0);
    require(std::any_of(mix.begin(),mix.end(),[](auto v){return v!=0;}));
    player.play(0,1,128);player.stop();require(player.active()==0);
    rejects([&]{player.play(-1,1,128);});rejects([&]{player.play(0,-1,128);});rejects([&]{player.play(0,1,129);});
    auto invalid=bank;invalid.bytes[45]=2;rejects([&]{okage::stream_tone(invalid,0);});
    invalid=bank;put(invalid.bytes,56,32);rejects([&]{okage::stream_tone(invalid,0);});
    invalid=bank;invalid.bytes[81]=0;okage::StreamPlayer unterminated(invalid);rejects([&]{unterminated.play(0,1,128);});
    auto padded=cue;padded.resize(112);put(padded,4,96);put(padded,56,32);put(padded,68,32);
    padded[96]=0x0c;padded[97]=7;
    okage::StreamPlayer terminated(okage::read_stream_bank(padded));terminated.play(0,1,128);
    std::vector<std::int32_t> with_tail(200);terminated.mix(with_tail);
    require(with_tail==mix && terminated.active()==0);
    padded[98]=1;okage::StreamPlayer invalid_tail(okage::read_stream_bank(padded));rejects([&]{invalid_tail.play(0,1,128);});
    std::cout<<"RXWS owned records, optional chunks, bounds and malformed input passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
