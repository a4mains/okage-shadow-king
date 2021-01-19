#include "okage/music.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}require(bad,"Invalid audio input accepted");}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
std::vector<std::uint8_t> sequence(){
    // One long note, a pan change, and note release across arbitrary render chunks.
    const std::vector<std::uint8_t> events={0,0x90,60,100,64,0xb0,10,100,64,0x80,60,0,0,0xff,0x2f,0};
    std::vector<std::uint8_t> b(52);put(b,0,0x51455358);put(b,4,0x101);
    put(b,12,0x44414548);put(b,16,8);put(b,20,1|(480<<16));put(b,24,36);
    put(b,28,0x4b435254);put(b,32,8);put(b,36,0x7f7f);put(b,40,52);
    put(b,44,0x55514553);put(b,48,events.size());b.insert(b.end(),events.begin(),events.end());put(b,8,b.size());return b;
}
okage::MusicBank bank(){
    okage::MusicBank b;okage::MusicTone t;t.flags=1;t.center=60;t.key_high=127;
    t.left=t.right=64;t.pitch_scale=4096;t.adsr1=0xff;t.adsr2=0x1f00;
    b.programs={{t}};b.velocities.resize(1);b.pans.resize(1);
    for(unsigned i=0;i<128;++i){b.velocities[0][i]=i;b.pans[0][i]=128-i;b.pans[0][128+i]=i;}
    auto& blocks=b.sample_blocks[0];blocks.resize(32,0x17);blocks[0]=0x17;blocks[1]=4;blocks[16]=0x27;blocks[17]=3;
    return b;
}
int main(){try{
    const auto score=sequence();okage::DryMusicPlayer whole(score,bank()),chunked(score,bank());
    std::vector<std::int16_t> expected(20000),actual(expected.size());whole.render(expected);
    const std::array<unsigned,5> chunks={2,14,802,6,1600};unsigned at=0,index=0;
    while(at<actual.size()){const auto count=std::min<std::size_t>(chunks[index++%chunks.size()],actual.size()-at);
        chunked.render(std::span(actual).subspan(at,count));at+=count;}
    require(actual==expected,"Render chunk boundaries change PCM");
    require(std::any_of(actual.begin(),actual.end(),[](auto x){return x!=0;}),"Silent test note");
    require(std::all_of(actual.end()-1000,actual.end(),[](auto x){return x==0;}),"Note release left audible tail");
    require(whole.sample_loops()>2 && whole.sample_loops()==chunked.sample_loops(),"Streaming loop progress");
    require(whole.driver_ticks()==13 && chunked.driver_ticks()==13,"Fractional driver tick advancement");
    okage::DryMusicPlayer muted(score,bank());muted.volume(0,0);std::vector<std::int16_t> zeros(6000);muted.render(zeros);
    require(std::all_of(zeros.begin(),zeros.end(),[](auto x){return x==0;}) && muted.sample_loops()>0,"Mute must preserve sample progression");
    rejects([&]{muted.render(std::span(zeros).first(3));});rejects([&]{muted.volume(129,0);});
    auto blocks=bank().sample_blocks.at(0);okage::SampleVoice voice(blocks,0xff,0x1f00);voice.parameters(4096,127,-127);
    bool audible=false;for(unsigned i=0;i<200;++i){const auto s=voice.tick();audible|=s[0]!=0;
        require(s[0]+s[1]>=-1 && s[0]+s[1]<=0,"Signed stereo gains/rounding");}
    require(audible && voice.loop_count>2,"Looped voice did not advance");voice.release();
    for(unsigned i=0;i<10;++i)voice.tick();require(voice.ended(),"Release did not stop voice");
    require(voice.tick()==std::array<std::int32_t,2>{},"Stopped voice output");
    rejects([&]{okage::SampleVoice v({},0,0);});rejects([&]{okage::SampleVoice v(okage::Bytes(blocks.data(),31),0,0);});
    blocks[0]=0x50;rejects([&]{okage::SampleVoice v(blocks,0xff,0);v.tick();});
    blocks[0]=0x17;blocks[1]=blocks[17]=0;
    rejects([&]{okage::SampleVoice v(blocks,0xff,0x1f00);for(unsigned i=0;i<100;++i)v.tick();});
    okage::MusicVolumeRamp ramp;ramp.request(80,64);
    for(unsigned i=0;i<12;++i)ramp.tick();require(ramp.level()==80 && !ramp.stopped,"Opening fade");
    ramp.tick();require(ramp.level()==80,"Settled fade");ramp.request(79,65);ramp.tick();
    require(ramp.current==80*65536-65*4096,"Target overshoot was incorrectly clamped");
    ramp.tick();require(ramp.level()==80,"Overshoot return");ramp.request(1,32767);ramp.tick();
    require(ramp.current==0 && ramp.target==0 && ramp.stopped,"Stop resets target");
    ramp.request(128,0);ramp.tick();require(ramp.current==0 && !ramp.stopped,"Zero-rate request");
    rejects([&]{ramp.request(129,1);});rejects([&]{ramp.request(80,32768);});
    std::cout<<"Music streaming/chunking, release, routing, malformed input and fade regressions passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
