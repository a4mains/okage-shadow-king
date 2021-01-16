#include "okage/sequence.hpp"
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}require(bad,"Malformed sequence accepted");}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
std::vector<std::uint8_t> fixture(std::initializer_list<std::uint8_t> events){
    std::vector<std::uint8_t> b(52);put(b,0,0x51455358);put(b,4,0x101);
    put(b,12,0x44414548);put(b,16,8);put(b,20,1|(480<<16));put(b,24,36);
    put(b,28,0x4b435254);put(b,32,8);put(b,36,0x7f7f);put(b,40,52);
    put(b,44,0x55514553);put(b,48,events.size());b.insert(b.end(),events);put(b,8,b.size());return b;
}
int main(){try{
    // One tick is 16 sequence pulses at the default 500000 tempo/division480.
    auto b=fixture({0,0x90,60,100,16,62,110,16,0x80,60,0,0,62,0,0,0xff,0x2f,0});
    okage::MusicSequence s(b);
    require(s.cursor()==53 && s.delta_scale()==256 && s.clock_step()==4096,"Initial clock");
    auto events=s.tick();require(events.size()==1 && events[0].key==60 && s.remaining()==0,"First note timing");
    events=s.tick();require(events.size()==1 && events[0].key==62,"Running status");
    events=s.tick();require(events.size()==2 && events[0].kind==okage::MusicEvent::Kind::note_off && s.ended(),"Note release/end");
    require(s.tick().empty(),"Ended sequence produces events");
    // Tempo uses integer scale; 600000 produces307, so a16-pulse delay leaves816.
    okage::MusicSequence tempo(fixture({0,0xff,0x51,3,9,0x27,0xc0,0,0x90,60,100,16,0x80,60,0,0,0xff,0x2f,0}));
    tempo.tick();require(tempo.delta_scale()==307 && tempo.remaining()==816,"Tempo rounding");
    require(tempo.tick().empty() && tempo.remaining()==-3280,"Fractional countdown");
    require(tempo.tick().size()==1 && tempo.ended(),"Fractional note release");
    // Loop start/end are NRPN MSB20/30; zero limit repeats. Delay keeps it bounded per tick.
    okage::MusicSequence loop(fixture({0,0xb0,99,20,0,6,0,0,0x90,60,100,16,0xb0,99,30,0,0xff,0x2f,0}));
    for(unsigned tick=0;tick<5;++tick){
        const auto notes=loop.tick();unsigned count=0;for(const auto& e:notes)count+=e.kind==okage::MusicEvent::Kind::note_on;
        require(count==1 && !loop.ended() && loop.loop_depth()==1,"NRPN loop iteration");
    }
    okage::MusicSequence controls(fixture({0,0xb2,7,99,0,10,20,0,11,88,0,0xc2,3,0,0xe2,127,65,16,0xff,0x2f,0}));
    controls.tick();const auto c=controls.channels[2];
    require(c.volume==99 && c.pan==20 && c.expression==88 && c.program==3 && c.pitch==65 && c.dirty==10,"Controller/pitch semantics");
    controls.tick();require(!controls.channels[2].dirty,"Channel dirty clearing");
    for(unsigned n=0;n<b.size();++n)rejects([&]{okage::MusicSequence truncated(okage::Bytes(b.data(),n));});
    auto bad=b;put(bad,48,0xffffffff);rejects([&]{okage::MusicSequence x(bad);});
    rejects([&]{okage::MusicSequence x(fixture({0,60,100,0}));x.tick();});
    rejects([&]{okage::MusicSequence x(fixture({0,0x90,60}));x.tick();});
    rejects([&]{okage::MusicSequence x(fixture({0,0xff,0x51,3,0,0,0}));x.tick();});
    rejects([&]{okage::MusicSequence x(fixture({0,0x90,60,100,0xff,0xff,0xff,0xff,0x7f}));x.tick();});
    rejects([&]{okage::MusicSequence x(fixture({0,0xb0,64,127,0}));x.tick();});
    std::cout<<"XSEQ timing, running status, loop, controllers and malformed input passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
