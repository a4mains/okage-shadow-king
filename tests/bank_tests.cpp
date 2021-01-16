#include "okage/bank.hpp"
#include <iostream>
#include <stdexcept>

void require(bool ok){if(!ok)throw std::runtime_error("Music bank assertion failed");}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}require(bad);}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
int main(){try{
    std::vector<std::uint8_t> b(12);put(b,0,0x42415658);put(b,4,0x101);
    auto chunk=[&](unsigned tag,unsigned n){auto at=b.size();b.resize(at+n+8);put(b,at,tag);put(b,at+4,n);return unsigned(at+8);};
    const auto head=chunk(0x44414548,28),table=chunk(0x46494754,8),pg=chunk(0x47504754,8),tone=chunk(0x50534754,36);
    const auto vel=chunk(0x544c4556,132),pan=chunk(0x544e4150,260);
    put(b,head+4,16);put(b,head+8,table);put(b,head+16,vel);put(b,head+20,pan);
    put(b,table,1);put(b,table+4,pg);put(b,pg,1<<16);put(b,pg+4,tone);
    b[tone+5]=127;b[tone+6]=60;b[tone+10]=127;b[tone+11]=64;put(b,tone+12,4096);
    put(b,vel,1);put(b,pan,1);for(unsigned i=0;i<128;++i){b[vel+4+i]=i;b[pan+4+i]=128;b[pan+132+i]=128;}
    put(b,8,b.size());b.resize((b.size()+15)&~15u);const auto body=b.size();b.resize(body+16);b[body+1]=1;
    auto bank=okage::read_music_bank(b);require(bank.programs.size()==1 && bank.samples.size()==1 && bank.samples.at(0).pcm.size()==28);
    const auto& t=bank.programs[0][0];okage::MusicChannel c;
    auto p=okage::music_tone_parameters(bank,t,60,127,c);require(p.left==124 && p.right==62 && p.key==60 && p.pitch_scale==4096);
    p=okage::music_tone_parameters(bank,t,60,127,c,0,64);require(p.left==0 && p.right==30);
    auto bent=t;bent.modulation[9]=2;bent.modulation[10]=12;c.pitch=127;
    p=okage::music_tone_parameters(bank,bent,60,127,c);require(p.key==71 && p.key_fine==104);
    c.pitch=0;p=okage::music_tone_parameters(bank,bent,60,127,c);require(p.key==58 && p.key_fine==0);
    require(okage::music_note_pitch(60,0,60,0)==4096 && okage::music_note_pitch(60,0,72,0)==8192 && okage::music_note_pitch(60,0,48,0)==2048);
    rejects([&]{okage::music_note_pitch(128,0,60,0);});
    auto invalid=c;invalid.left=65535;rejects([&]{okage::music_tone_parameters(bank,t,60,127,invalid);});
    for(unsigned n=0;n<b.size();++n)rejects([&]{okage::read_music_bank(okage::Bytes(b.data(),n));});
    auto bad=b;put(bad,table+4,0xffffffff);rejects([&]{okage::read_music_bank(bad);});
    bad=b;bad[tone+8]=1;rejects([&]{okage::read_music_bank(bad);});
    bad=b;put(bad,tone+20,16);rejects([&]{okage::read_music_bank(bad);});
    bad=b;bad[body+1]=0;rejects([&]{okage::read_music_bank(bad);});
    bad=b;bad[head]=1;rejects([&]{okage::read_music_bank(bad);});
    std::cout<<"XVAB tone resolution, gains, pitch, and malformed bank bounds passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
