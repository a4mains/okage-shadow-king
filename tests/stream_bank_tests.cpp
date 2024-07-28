#include "okage/stream_bank.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <source_location>

void require(bool ok,std::source_location where=std::source_location::current()){
    if(!ok)throw std::runtime_error("RXWS assertion at line "+std::to_string(where.line()));
}
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
    std::vector<std::int32_t> mix(400);player.mix(mix);require(player.active()==0);
    require(std::any_of(mix.begin(),mix.end(),[](auto v){return v!=0;}));
    player.play(0,1,128);player.play(0,1,128);player.stop();require(player.active()==0);
    std::vector<std::int32_t> after_stop(400);player.mix(after_stop);
    require(std::all_of(after_stop.begin(),after_stop.end(),[](auto value){return value==0;}));
    player.play(0,1,128);player.mix(after_stop);require(after_stop==mix); // Bank remains usable.
    rejects([&]{player.play(-1,1,128);});rejects([&]{player.play(0,-1,128);});rejects([&]{player.play(0,1,129);});
    auto invalid=bank;invalid.bytes[45]=2;rejects([&]{okage::stream_tone(invalid,0);});
    invalid=bank;put(invalid.bytes,56,32);rejects([&]{okage::stream_tone(invalid,0);});
    // Finite FORM extent, not source flags, determines the streaming endpoint.
    invalid=bank;invalid.bytes[81]=0;okage::StreamPlayer unterminated(invalid);unterminated.play(0,1,128);
    std::vector<std::int32_t> no_marker(400);unterminated.mix(no_marker);
    require(no_marker==mix && unterminated.active()==0);
    auto padded=cue;padded.resize(112);put(padded,4,96);put(padded,56,32);put(padded,68,32);
    padded[96]=0x0c;padded[97]=7;
    okage::StreamPlayer terminated(okage::read_stream_bank(padded));terminated.play(0,1,128);
    std::vector<std::int32_t> with_tail(400);terminated.mix(with_tail);
    require(with_tail==mix && terminated.active()==0);
    // Nonzero payload after a source end flag must play, not be discarded.
    padded[96]=0;padded[98]=0x77;okage::StreamPlayer audible_tail(okage::read_stream_bank(padded));audible_tail.play(0,1,128);
    std::vector<std::int32_t> changed_tail(400);audible_tail.mix(changed_tail);require(changed_tail!=mix);
    padded[96]=0x50;okage::StreamPlayer invalid_tail(okage::read_stream_bank(padded));rejects([&]{invalid_tail.play(0,1,128);});
    rejects([&]{okage::prepare_stream_sample({});});
    rejects([&]{okage::prepare_stream_sample(okage::Bytes(cue).first(15));});
    // Check all four alignment residues and clear source end/loop/reserved flags.
    for(unsigned n=1;n<=8;++n){
        std::vector<std::uint8_t> raw(n*16);
        for(unsigned i=0;i<n;++i){raw[i*16]=0x0c;raw[i*16+1]=0xff;raw[i*16+2]=i;}
        auto prepared=okage::prepare_stream_sample(raw);const auto aligned=(n*16+63)/64*64;
        require(prepared.size()==aligned+64 && prepared[aligned+1]==1);
        for(unsigned i=0;i<n;++i)require(prepared[i*16]==0x0c && prepared[i*16+1]==0 && prepared[i*16+2]==i);
        for(unsigned i=n*16;i<prepared.size();++i)if(i!=aligned+1)require(prepared[i]==0);
    }
    auto looping=cue;looping.resize(144);put(looping,4,128);put(looping,56,64);put(looping,68,64);
    looping[38]=3;put(looping,60,16);
    for(unsigned n=0;n<4;++n) {
        looping[80+n*16]=0x28;looping[81+n*16]=0xff;
        for(unsigned j=2;j<16;++j)looping[80+n*16+j]=std::uint8_t(19*n+j);
    }
    const auto loop_bank=okage::read_stream_bank(looping);const auto loop_tone=okage::stream_tone(loop_bank,0);
    require(loop_tone.loop_offset==16);
    std::vector<std::uint8_t> linear(looping.begin()+80,looping.end());
    for(unsigned n=0;n<100;++n)linear.insert(linear.end(),looping.begin()+96,looping.end());
    linear=okage::prepare_stream_sample(linear);
    okage::SampleVoice unrolled(linear,okage::StreamTone::adsr1,okage::StreamTone::adsr2);
    unrolled.parameters(loop_tone.pitch,loop_tone.left,loop_tone.right);
    okage::StreamPlayer repeated(loop_bank);repeated.play(0,1,128);
    std::vector<std::int32_t> loop_pcm(6000*2);repeated.mix(loop_pcm);require(repeated.active()==1);
    for(unsigned n=0;n<6000;++n){const auto sample=unrolled.tick();require(loop_pcm[n*2]==sample[0]&&loop_pcm[n*2+1]==sample[1]);}
    repeated.stop();require(repeated.active()==0);
    for(unsigned offset:{1u,63u,64u,0xffffffffu}) {
        auto invalid_loop=loop_bank;put(invalid_loop.bytes,60,offset);
        rejects([&]{okage::stream_tone(invalid_loop,0);});
        rejects([&]{okage::prepare_stream_sample(okage::Bytes(looping).subspan(80),offset);});
    }
    // Zero and final-block loop starts are legal; source flags are ignored.
    for(unsigned offset:{0u,48u}) {
        const auto prepared=okage::prepare_stream_sample(okage::Bytes(looping).subspan(80),offset);
        okage::SampleVoice loop(prepared,0x8000,0x8026);loop.parameters(4096,127,127);
        for(unsigned i=0;i<1000;++i)loop.tick();require(!loop.ended()&&loop.loop_count>1);
    }
    std::cout<<"RXWS records, finite/looping playback, bounds and malformed input passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
