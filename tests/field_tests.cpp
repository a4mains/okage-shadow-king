#include "okage/field.hpp"
#include "okage/event.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
void require(bool value){if(!value)throw std::runtime_error("Field test failed");}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected field rejection");}
int main(){try {
    const auto angles=okage::make_atan_table();const auto sine=okage::make_sine_table();
    require(okage::camera_angle(0,0,angles)==0);
    require(okage::camera_angle(1,0,angles)==.25f);
    require(okage::camera_angle(-1,0,angles)==.75f);
    const auto look=okage::camera_look_at({7.75f,-6.71f,-20.12f,1},{-12.35f,-2,-4.35f,1},angles,sine);
    require(look[0]==.97100830078125f && look[1]==.8558349609375f); // Original execution.
    okage::CameraBlend blend;okage::Float4 p{10,4,2,1},t{1,2,3,1};
    blend.begin(.5f,{0,0,0,1},{0,0,0,1});blend.apply(p,t);require(p[0]==0 && blend.phase==.5f);
    p={10,4,2,1};t={1,2,3,1};blend.apply(p,t);require(p[0]==5 && t[0]==.5f && blend.phase==1);
    const int rounding=std::fegetround();okage::camera_angle(1,3,angles);require(std::fegetround()==rounding);
    std::vector<std::uint8_t> b(0xc00);
    auto word=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(i*8);};
    auto point=[&](unsigned at,okage::Float4 v){for(unsigned i=0;i<3;++i)word(at+4*i,__builtin_bit_cast(std::uint32_t, (v[i])));};
    word(0,0x544d58);word(4,b.size()-32);word(16,0xffffffff);
    word(0xb88,0xffe00007);word(0xb8c,0x30);point(0xb90,{2,3,4,1});
    word(0xb9c,0x10004b);point(0xba0,{0,0,0,1});word(0xbac,0x3c00002);
    word(0xbb0,0x4c);word(0xbb4,10100);word(0xbb8,0x20000a);word(0xbbc,0xffe00007);
    word(0xbc0,0x5a00033);point(0xbc4,{8,6,4,1});word(0xbd0,0x5a00052);
    word(0xbd4,0x10004b);point(0xbd8,{1,2,3,1});word(0xbe4,0xff800236);
    unsigned frame=0,starts=0,polls=0;
    okage::OpeningField field(b,[&](int id){require(id==10100);++starts;},[&](int n){require(n==2);++polls;return frame>=70;});
    for(frame=1;frame<=180;++frame){field.tick();if(frame<70)require(field.position[0]==2);}
    require(starts==1 && polls==10 && field.pc()==0xbe4 && field.pending_opcode==0x36);
    require(field.pending_actor==-8 && field.pending_animation==2 && field.position[0]==8 && field.target[0]==1);
    // A selector whose low byte is a valid group opcode must be consumed as
    // data. Keep this write pending until storage is bound, then resume it.
    word(0xbe4,0x100047);word(0xbe8,0x10007);word(0xbec,0xff800236);
    okage::OpeningField writing(b,[](int){},[](int){return true;});
    for(unsigned i=0;i<180;++i)writing.tick();
    require(writing.pc()==0xbe4 && writing.pending_opcode==0x47);
    std::array<std::uint8_t,0x1000> variables{};unsigned writes=0;
    writing.set_variable=[&](unsigned index,unsigned bank,std::int32_t value){
        require(index==7 && bank==1 && value==1);++writes;
        okage::set_event_variable(variables,index,bank,value);
    };
    writing.tick();require(writes==1 && writing.pc()==0xbec && writing.pending_opcode==0x36);
    require(okage::event_variable(variables,0x10007)==1 && variables[0x110]==1);
    writing.tick();require(writes==1);
    for(auto selector:{0x40006u,0x80000u}) {
        word(0xbe8,selector);okage::OpeningField special(b,[](int){},[](int){return true;});
        rejects([&]{for(unsigned i=0;i<180;++i)special.tick();});
    }
    word(0xbe4,0x4b);word(0xbe8,0xff800236);
    okage::OpeningField cameraRelease(b,[](int){},[](int){return true;});
    require(cameraRelease.camera_mode()==2 && cameraRelease.event_running());
    cameraRelease.tick();
    const auto before=cameraRelease.rotation;
    for(unsigned i=0;i<180;++i)cameraRelease.tick();
    require(cameraRelease.camera_mode()==0 && cameraRelease.pc()==0xbe8);
    require(cameraRelease.rotation==before);
    // Run signed conditions in the timed event context, with both branch paths.
    word(0xbe4,(24u<<20)|(4u<<8)|11);word(0xbe8,0x20000);word(0xbec,0);
    word(0xbf0,0x100047);word(0xbf4,0x10007);word(0xbf8,0xff800236);word(0xbfc,0xff800236);
    for(int input:{-1,0}) {
        unsigned count=0;okage::OpeningField conditional(b,[](int){},[](int){return true;});
        conditional.get_variable=[&](auto selector){require(selector==0x20000);return input;};
        conditional.set_variable=[&](auto index,auto bank,auto value){require(index==7&&bank==1&&value==1);++count;};
        for(unsigned i=0;i<180;++i)conditional.tick();
        require(count==unsigned(input<0));require(conditional.pending_opcode==0x36);
        require(conditional.pc()==(input<0?0xbf8u:0xbfcu));
    }
    word(0xbe4,0x04600243);word(0xbe8,0);
    okage::OpeningField exiting(b,[](int){},[](int){return true;});
    std::vector<unsigned> fades;exiting.transition_service=[&](unsigned n){fades.push_back(n);};
    for(unsigned i=0;i<180&&!exiting.transition();++i)exiting.tick();
    require(exiting.transition()&&exiting.transition()->scene==70&&exiting.transition()->entrance==2);
    require(!exiting.event_running()&&exiting.transition()->countdown==2&&!exiting.transition()->ready);
    exiting.tick();require(exiting.transition()->countdown==1&&!exiting.transition()->ready);
    exiting.tick();require(exiting.transition()->countdown==0&&!exiting.transition()->ready);
    exiting.tick();require(exiting.transition()->ready&&fades==std::vector<unsigned>({1,0}));
    rejects([&]{exiting.tick();});
    word(0xbe4,0x02500067);word(0xbe8,0xff800236);
    okage::OpeningField sound(b,[](int){},[](int){return true;});
    for(unsigned i=0;i<180;++i)sound.tick();
    require(sound.pc()==0xbe4&&sound.pending_opcode==0x67);
    unsigned cues=0;sound.sound_cue=[&](unsigned bank,int cue,float speed,unsigned gain){
        require(bank==0&&cue==37&&speed==1&&gain==128);++cues;return true;
    };
    sound.tick();require(cues==1&&sound.pc()==0xbe8);
    word(0xbe4,0xfff00067);
    okage::OpeningField stop(b,[](int){},[](int){return true;});
    stop.sound_cue=[&](auto,auto,auto,auto){throw std::runtime_error("Stop routed as play");return false;};
    for(unsigned i=0;i<180;++i)stop.tick();
    require(stop.pc()==0xbe4&&stop.pending_opcode==0x67);
    word(0xb88,0);rejects([&]{okage::OpeningField bad(b,[](int){},[](int){return true;});});
    rejects([&]{okage::field_projection({});});
    b.assign(88,0);word(0,0x475453);word(16,32);word(20,1);word(24,88);
    point(36,{0,0,0,1});point(64,{10,.5f,-2,1});word(76,__builtin_bit_cast(std::uint32_t, (1.f)));
    const auto settings=okage::field_anchored_camera(b);
    const auto anchored=okage::anchored_camera({0,0,10,1},settings,angles,sine);
    require(anchored.position==okage::Float4({0,0,15,1})&&anchored.target==okage::Float4({0,-2,10,1}));
    require(std::fegetround()==rounding);
    word(24,87);rejects([&]{okage::field_anchored_camera(b);});word(24,88);
    word(20,31);rejects([&]{okage::field_anchored_camera(b);});word(20,1);
    word(76,0);rejects([&]{okage::field_anchored_camera(b);});word(76,__builtin_bit_cast(std::uint32_t, (1.f)));
    word(60,0x10000);rejects([&]{okage::field_anchored_camera(b);});word(60,0);
    word(64,0x7fc00000);rejects([&]{okage::field_anchored_camera(b);});
    b.assign(88,0);word(0,0x475453);word(24,32);word(28,2);
    point(36,{1,-2,3,1});point(48,{0,.25f,0,1});
    point(64,{-4,-5,6,1});point(76,{0,-.5f,0,1});
    const auto entrance=okage::field_entrance(b,1);
    require(entrance.position==okage::Float4({-4,-5,6,1})&&entrance.rotation==okage::Float4({0,-.5f,0,1}));
    rejects([&]{okage::field_entrance(b,2);});
    word(64,0x7fc00000);rejects([&]{okage::field_entrance(b,1);});word(64,0);
    word(24,33);rejects([&]{okage::field_entrance(b,0);});word(24,32);
    b.resize(87);rejects([&]{okage::field_entrance(b,1);});
    // Independently specified minimal field database and malformed boundaries.
    const std::string strings("scene0\0room\0e_room\0actor\0",25);
    b.assign(48+84+strings.size(),0);word(0,0x424458);word(4,84);word(8,strings.size());word(12,1);word(16,0xffffffff);
    word(32,0);word(36,0);word(40,21);word(52,1);word(68,7);word(76,12);
    word(96,19);word(100,0);word(104,std::uint32_t(-30));word(108,0);
    point(112,{1,2,3,1});word(124,__builtin_bit_cast(std::uint32_t, (.5f)));
    std::copy(strings.begin(),strings.end(),b.begin()+132);
    const auto definition=okage::read_field_definition(b,"room");
    require(definition.type==1&&definition.event=="e_room"&&definition.npcs.size()==1);
    const auto by_scene=okage::read_field_definition(b,0);
    require(by_scene.scene==0&&by_scene.room=="room"&&by_scene.event==definition.event);
    rejects([&]{okage::read_field_definition(b,1);});
    rejects([&]{okage::read_field_definition(b,-1);});
    const auto& npc=definition.npcs[0];require(npc.alias==-8&&npc.model=="actor"&&npc.talk==-30&&npc.state==0);
    require(npc.position==okage::Float4({1,2,3,1})&&npc.facing==.5f);
    rejects([&]{okage::read_field_definition(b,"missing");});
    for(unsigned size=0;size<b.size();++size)rejects([&]{okage::read_field_definition(okage::Bytes(b.data(),size),"room");});
    word(104,0);word(112,0x7fc00000);rejects([&]{okage::read_field_definition(b,"room");});word(112,0);
    word(128,19);rejects([&]{okage::read_field_definition(b,"room");});word(128,0);
    word(32,1000);rejects([&]{okage::read_field_definition(b,"room");});
    b.assign(40,0);word(0,0x544d58);word(4,8);word(16,0xffffffff);
    auto construction=definition;construction.field_flags=7;
    auto state=okage::field_setup_state(b,variables,construction);
    auto get=[&](unsigned at){return std::uint32_t(state[at])|(std::uint32_t(state[at+1])<<8)|
        (std::uint32_t(state[at+2])<<16)|(std::uint32_t(state[at+3])<<24);};
    require(get(0x414)==0&&get(0x428)==0xffffffff&&get(0x41c)==0x40000000&&get(0x40c)==7);
    construction.type=0;state=okage::field_setup_state(b,variables,construction);
    require(get(0x414)==17&&get(0x42c)==33&&state[0x1a8]==1&&state[0x2a7]==1);
    word(32,(5u<<20)|(9u<<8)|0x59);
    state=okage::field_setup_state(b,variables,construction);require(get(0x414)==9);
    const auto prepared=okage::prepare_field_definition(b,variables,construction);
    require(prepared.type==0&&prepared.dust==9&&prepared.field_flags==7);
    std::cout<<"Camera look-at, blend, field dialogue gate and pending actor service passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
