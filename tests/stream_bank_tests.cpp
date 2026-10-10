#include "okage/stream_bank.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
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
    //12df80 sound41 reads cue gain0x80 with LB (-128), while its FORM
    //channels are127.176738 MULT/SRA7 produces -127, never +127 or -126.
    const auto signed_tone=okage::stream_tone_signed(bank,0,1,-128);
    require(signed_tone.left==-127&&signed_tone.right==-127&&signed_tone.pitch==tone.pitch*2+1);
    auto asymmetric=bank;asymmetric.bytes[40]=0x80;asymmetric.bytes[41]=63;
    const auto negative_form=okage::stream_tone_signed(asymmetric,0,1,-65);
    require(negative_form.left==65&&negative_form.right==-32);
    const auto positive_form=okage::stream_tone_signed(asymmetric,0,1,65);
    require(positive_form.left==-65&&positive_form.right==31);
    for(unsigned volume:{0u,1u,64u,127u,128u}) {
        const auto old=okage::stream_tone(bank,0,1,volume);
        const auto added=okage::stream_tone_signed(bank,0,1,int(volume));
        require(old.offset==added.offset&&old.size==added.size&&old.sample_rate==added.sample_rate&&
                old.pitch==added.pitch&&old.left==added.left&&old.right==added.right&&old.loop_offset==added.loop_offset);
        okage::StreamPlayer original(bank),explicit_signed(bank);
        original.play(0,1,volume);explicit_signed.play_signed(0,1,int(volume));
        std::vector<std::int32_t> a(400),c(400);original.mix(a);explicit_signed.mix(c);require(a==c);
    }
    okage::StreamPlayer negative(bank);negative.play_signed(0,1,-128);
    std::vector<std::int32_t> inverted(400);negative.mix(inverted);require(negative.active()==0);
    const auto prepared_signal=okage::prepare_stream_sample(okage::Bytes(bank.bytes).subspan(signed_tone.offset,signed_tone.size));
    okage::SampleVoice signal(prepared_signal,okage::StreamTone::adsr1,okage::StreamTone::adsr2);
    signal.parameters(signed_tone.pitch,-128,-128);
    const auto floor_q15=[](std::int64_t value) {return value>=0?value/32768:-((-value+32767)/32768);};
    bool nonzero_negative=false,floor_differs=false;
    for(unsigned i=0;i<inverted.size()/2;++i) {
        const auto unscaled=-signal.tick()[0];
        const auto expected=floor_q15(std::int64_t(unscaled)*-32512);
        require(inverted[i*2]==expected&&inverted[i*2+1]==expected);
        require(mix[i*2]==floor_q15(std::int64_t(unscaled)*32512));
        nonzero_negative|=inverted[i*2]<0;
        floor_differs|=inverted[i*2]!=-mix[i*2];
    }
    require(nonzero_negative&&floor_differs);
    okage::StreamPlayer combined(bank);combined.play(0,1,128);combined.play_signed(0,1,-128);
    std::vector<std::int32_t> sum(400,7);combined.mix(sum);
    for(unsigned i=0;i<sum.size();++i)require(sum[i]==7+mix[i]+inverted[i]);
    okage::StreamPlayer guarded(bank);guarded.play(0,1,128);
    for(int volume:{-129,129,std::numeric_limits<int>::min(),std::numeric_limits<int>::max()}) {
        rejects([&]{okage::stream_tone_signed(bank,0,1,volume);});
        rejects([&]{guarded.play_signed(0,1,volume);});require(guarded.active()==1);
    }
    rejects([&]{okage::stream_tone(bank,0,1,std::numeric_limits<unsigned>::max());});
    rejects([&]{guarded.play(0,1,std::numeric_limits<unsigned>::max());});require(guarded.active()==1);
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
    {
        okage::StreamPlayer pool(loop_bank);
        const auto a=pool.play(0,1,128),b=pool.play(0,1,128),c=pool.play_signed(0,1,100);
        require(a==255&&b==254&&c==253&&pool.active()==3);
        pool.stop_voice(b);require(pool.active()==2);
        require(pool.play(0,1,128)==254);              // released id is the free head
        pool.stop_voice(7);require(pool.active()==3);  // inactive id: no-op
        pool.stop();require(pool.active()==0);
        require(pool.play(0,1,128)==255);
        pool.stop();
        okage::StreamPlayer finished(bank);
        const auto f1=finished.play(0,1,128),f2=finished.play(0,1,128);require(f1==255&&f2==254);
        std::vector<std::int32_t> drain(400);finished.mix(drain);require(finished.active()==0);
        require(finished.play(0,1,128)==255);          // finished: newest (254) released first
        okage::StreamPlayer which(loop_bank);
        which.play(0,1,128);which.play(0,1,128);which.play(0,1,128);
        which.stop_voice(254);which.stop();
        const auto w1=which.play(0,1,128),w2=which.play(0,1,128),w3=which.play(0,1,128),w4=which.play(0,1,128);
        require(w1==255&&w2==253&&w3==254&&w4==252);
        which.stop();
        // The 48-voice guard is reached before the 256-record pool can empty.
        okage::StreamPlayer full(loop_bank);
        for(int i=0;i<48;++i)full.play(0,1,128);
        rejects([&]{full.play(0,1,128);});
        require(full.active()==48);
    }
    std::cout<<"RXWS records, finite/looping playback, bounds and malformed input passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
