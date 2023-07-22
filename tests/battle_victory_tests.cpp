#include "okage/battle_victory.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace okage;
namespace {
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
void put(BattleState& r,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)r.bytes[at+i]=v>>(i*8);}
unsigned word(const BattleState& r,unsigned at){unsigned v=0;for(unsigned i=0;i<4;++i)v|=unsigned(r.bytes[at+i])<<(i*8);return v;}
void scalar(BattleState& r,unsigned at,float f){put(r,at,std::bit_cast<unsigned>(f));}
float scalar(const BattleState& r,unsigned at){return std::bit_cast<float>(word(r,at));}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught,"Expected guard");}
struct Fixture {
 BattleState root;SceneFade fade;std::array<std::uint8_t,8> reward{};
 std::array<BattleEntityBinding,4> entities{};std::vector<int> calls;unsigned draws=0,first=0,second=0;
 BattleVictoryServices services{
  [&]{return ++draws==1?first:second;},[&](unsigned id){calls.push_back(id);},
  [&]{calls.push_back(100);},[&](unsigned id){calls.push_back(200+id);},
  [&](unsigned layer,int speed){check(layer==1&&speed==-128,"UI fade arguments");calls.push_back(300);}};
 Fixture(){scalar(root,0x2c,1);fade.current=fade.target=1;}
 void tick(){battle_victory_update(root,entities,fade,reward,services);}
};
void countdown(){
 Fixture f;f.tick();check(word(f.root,0x24)==1&&scalar(f.root,0x81bc)==299,"Phase0 falls through countdown");
 for(unsigned i=0;i<299;++i)f.tick();check(scalar(f.root,0x81bc)==0&&f.draws==0,"Equality0 waits");
 f.tick();check(word(f.root,0x24)==2&&word(f.root,0x81b8)==2&&scalar(f.root,0x81bc)==300,"Negative boundary publishes outcome before services");
 check(f.draws==1&&f.calls==std::vector<int>({40,100,204,300}),"Default camera then music/jingle/UI exact order");
 scalar(f.root,0x81bc,0);put(f.root,0x8080,1);f.reward[2]=0xff;f.reward[3]=0xff;f.tick();
 check(word(f.root,0x24)==3&&f.reward[2]==0x60&&f.reward[3]==0xea,"Conditional unsigned doubled reward clamps60000");
 check(f.fade.step==std::bit_cast<float>(0x3ca3d70au)&&f.fade.target==0,"Source scene fade request");
 f.tick();check(!(word(f.root,0x1c)&0x80000000u),"Retirement waits for actual fade");
 put(f.root,0x1c,0x123);f.fade.current=0;f.tick();check(word(f.root,0x1c)==0x80000123,"Delete flag preserves scheduler bits");
}
void cameras(){
 for(unsigned selected=0;selected<4;++selected){
  Fixture f;std::array<BattleActor,4> actors;std::array<std::uint8_t,4> primary{1,6,4,1};std::array<std::uint8_t,2> counter{2,0};
  for(unsigned i=0;i<4;++i){actors[i].stat_bindings[1]=std::span(primary).subspan(i,1);actors[i].stat_bindings[2]=counter;f.entities[i].actor=&actors[i];}
  put(f.root,0x24,1);scalar(f.root,0x81bc,0);f.first=32;f.second=selected*32;f.tick();
  check(f.draws==2&&f.calls.front()==int(selected==1||selected==2?40:43+selected),"Live four-slot camera choice including Stan and incapacitated bodies");
 }
 Fixture f;put(f.root,0x24,1);scalar(f.root,0x81bc,0);f.first=0xffffffff;f.second=0xffffffff;f.tick();
 check(f.draws==2&&f.calls.front()==40,"High-bit samples preserve signed shift and null-body choices");
}
void failures(){
 for(unsigned stop=0;stop<4;++stop){
  Fixture f;put(f.root,0x24,1);scalar(f.root,0x81bc,0);
  if(stop==0)f.services.camera={};if(stop==1)f.services.stop_music={};if(stop==2)f.services.jingle={};if(stop==3)f.services.fade_ui={};
  rejects([&]{f.tick();});check(f.calls.size()==stop&&f.draws==1&&word(f.root,0x24)==2&&word(f.root,0x81b8)==2,"Failure retains preceding phase/RNG/service order");
 }
 Fixture f;put(f.root,0x24,2);scalar(f.root,0x81bc,0);put(f.root,0x8080,1);
 rejects([&]{battle_victory_update(f.root,f.entities,f.fade,{},f.services);});
 check(f.fade.target==0&&word(f.root,0x24)==2&&scalar(f.root,0x81bc)==-1,"Bad reward fails after fade and timer before phase store");
}
void fractional(){
 Fixture f;put(f.root,0x24,1);scalar(f.root,0x81bc,300);scalar(f.root,0x2c,.1f);
 std::fenv_t saved;std::fegetenv(&saved);std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID);f.tick();
 check(std::fegetround()==FE_UPWARD&&std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID,"Timer restores complete caller floating environment");std::fesetenv(&saved);
 check(word(f.root,0x81bc)==0x4395f333,"Single toward-zero subtraction");
 for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::denorm_min()}) {
  Fixture g;put(g.root,0x24,2);scalar(g.root,0x81bc,bad);const auto before=g.root.bytes;
  rejects([&]{g.tick();});check(g.root.bytes==before&&g.fade.target==1,"Unsupported timer fails before fade/phase publication");
 }
 for(unsigned state:{4u,0xffffffffu}){Fixture g;put(g.root,0x24,state);const auto before=g.root.bytes;g.tick();check(g.root.bytes==before&&g.calls.empty(),"Unknown phase source no-op");}
 for(unsigned flag:{0u,2u}){Fixture g;put(g.root,0x24,2);put(g.root,0x8080,flag);scalar(g.root,0x81bc,0);g.reward[2]=17;g.tick();check(g.reward[2]==17,"Reward multiplier exact flag1 only");}
}
}
int main(){try{countdown();cameras();failures();fractional();std::cout<<"Battle victory lifecycle tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
