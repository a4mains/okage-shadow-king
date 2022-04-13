#include "okage/field.hpp"
#include "okage/event.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <stdexcept>
void require(bool value,unsigned where=__builtin_LINE()){
    if(!value)throw std::runtime_error("Field test failed at line "+std::to_string(where));
}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected field rejection");}
int main(){try {
    {
        okage::SceneClearColor clear;
        require(clear.rgb==std::array<std::uint8_t,3>{64,64,64});
        std::vector<std::uint8_t> stg(72);
        auto put=[&](unsigned at,unsigned v){for(unsigned j=0;j<4;++j)stg[at+j]=v>>(8*j);};
        put(0,0x475453);put(8,32);put(12,2);
        put(32,0x60);put(36,9);put(40,10);put(44,11);
        // Raw low bytes, ordered repeated records, not floating conversion.
        put(52,0x60);put(56,0x1234);put(60,0xffffffff);put(64,256);put(68,0x7fc00000);
        clear.load_stage(stg);require(clear.rgb==std::array<std::uint8_t,3>{52,255,0});
        put(12,0);clear.load_stage(stg);require(clear.rgb==std::array<std::uint8_t,3>{52,255,0});
        put(12,3);rejects([&]{clear.load_stage(stg);});
        require(clear.rgb==std::array<std::uint8_t,3>{52,255,0});
        rejects([&]{clear.load_stage({});});
        okage::Animation camera(3);
        camera.set_channel(0,{13.09f,-10,-209,1});camera.set_channel(1,{.9757f,.038f,0,1});
        camera.tick();
        require(camera.values()==std::vector<okage::Float4>{{13.09f,-10,-209,1},{.9757f,.038f,0,1},{1,1,1,1}});
        rejects([&]{camera.start(0);});rejects([&]{okage::Animation invalid(0);});

    }
    {
        const auto angles=okage::make_atan_table();
        const auto floats=[](std::array<std::uint32_t,4> bits){okage::Float4 out;for(unsigned i=0;i<4;++i)out[i]=__builtin_bit_cast(float, (bits[i]));return out;};
        const std::array<std::array<std::uint32_t,4>,2> positions={{{0x4146147b,0,0xc0db3333,0x3f800000},{0x41175a05,0xba83126f,0xc0980332,0x3f800000}}};
        const std::array<std::array<std::uint32_t,4>,2> velocities={{{0xbccf0cb7,0,0x3c96b0de,0xbf800000},{0xbcdcc6cb,0x357e3343,0xbc85d0ad,0xbf800000}}};
        for(unsigned i=0;i<2;++i) {
            okage::GameRandom random{i?0xe9ce4642u:0x26c79124u};okage::WanderingNpc npc;
            const auto rotation=npc.begin_leg(floats(positions[i]),0,[&]{return random.next();},angles);
            require(npc.counter==(i?1056:2112));
            require(random.state==(i?0x497e9b90u:0xe9ce4642u));
            for(unsigned k=0;k<4;++k)require(__builtin_bit_cast(std::uint32_t, (npc.velocity[k]))==velocities[i][k]);
            require(__builtin_bit_cast(std::uint32_t, (rotation[1]))==(i?0x3e273000u:0x3eb34800u));
            npc.finish_move(floats(positions[i]),floats(positions[i]));require(npc.counter==-1);
        }
        // Sub-unit horizontal distance truncates before multiplication by32.
        okage::WanderingNpc short_leg;unsigned draws=0;
        short_leg.begin_leg({.5f,0,0,1},0,[&]{++draws;return 50u;},angles);
        require(short_leg.counter==0&&draws==2);
        rejects([&]{short_leg.finish_move({0,0,0,1},{1,0,0,1});});
    }
    {
        okage::ScreenFilter filter;
        filter.tick(80);require(filter.alpha==0);
        filter.select(0x49,1);filter.tick(80.9f);require(filter.alpha==80);
        filter.select(0,0);filter.tick(0);require(!filter.updating&&filter.alpha==80);
        filter.select(0x49,1);filter.tick(200);require(filter.alpha==127);
        filter.tick(.9f);require(filter.alpha==0);
        rejects([&]{filter.select(0x48,1);});rejects([&]{filter.select(0x49,2);});
        rejects([&]{filter.tick(-1);});rejects([&]{filter.tick(__builtin_bit_cast(float, (0x7fc00000u)));});
        okage::FrameFeedback history;
        okage::Texture previous{1,2,0,0,{255,0,1,255,17,18,19,255}};
        okage::Texture current{1,1,0,0,{0,255,0,255}};
        rejects([&]{history.apply(current,11,80);});
        history.retain(previous,10,1);
        auto repeat=current;history.apply(current,11,80);history.apply(repeat,11,80);
        require(current.rgba==std::vector<std::uint8_t>({159,95,0,255})&&repeat.rgba==current.rgba);
        rejects([&]{history.apply(repeat,12,80);}); // No fabricated skipped-frame history.
        rejects([&]{history.retain(current,10,1);});
        history.retain(current,11,1);
        okage::Texture next{1,1,0,0,{255,255,255,255}};history.apply(next,12,100);
        require(next.rgba==std::vector<std::uint8_t>({180,130,55,255}));
        // Actual service dispatch and group-2 component8 drive the setting.
        std::vector<std::uint8_t> script(0xc00);
        auto put=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)script[at+i]=value>>(8*i);};
        put(0,0x544d58);put(4,script.size()-32);put(16,0xffffffff);
        put(0xb88,0xffe00007);put(0xb8c,0x30);
        put(0xb9c,0x0080002e);put(0xba0,__builtin_bit_cast(unsigned, (80.f)));
        put(0xba4,0x0010493d);put(0xba8,0x00100002); // Enable, wait one frame.
        put(0xbac,0x0000003d);put(0xbb0,0x0080002e);put(0xbb4,0);put(0xbb8,0x20000a);
        put(0xbbc,0x00100002);put(0xbc0,0x0010493d);put(0xbc4,0x0ff00002);
        okage::OpeningField animated(script,[](int){},[](int){return true;});
        animated.tick();require(animated.screen_filter.updating&&animated.screen_filter.alpha==80);
        animated.tick();require(!animated.screen_filter.updating&&animated.screen_filter.alpha==80);
        animated.tick();require(!animated.screen_filter.updating&&animated.screen_filter.alpha==80);
        animated.tick();require(animated.screen_filter.updating&&animated.screen_filter.alpha==0);
    }
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
    for(int selector:{-1,0,1,2}) {
        word(0xbe4,(std::uint32_t(selector)<<20)|0xabc60u);word(0xbe8,0xff800236);
        okage::OpeningField input(b,[](int){},[](int){return true;});
        for(unsigned i=0;i<180;++i)input.tick();
        require(input.pc()==0xbe4&&input.pending_opcode==0x60);
        unsigned calls=0;
        input.dialogue_input=[&](bool enabled){require(enabled==(selector==1));++calls;};
        input.tick();require(calls==1&&input.pc()==0xbe8&&input.pending_opcode==0x36);
        input.tick();require(calls==1);
    }
    for(int mode:{-1,0,1,2}) {
        word(0xbe4,(std::uint32_t(mode)<<20)|(50u<<8)|0x4f);word(0xbe8,0xff800236);
        okage::OpeningField fading(b,[](int){},[](int){return true;});
        for(unsigned i=0;i<180;++i)fading.tick();
        require(fading.pc()==0xbe4&&fading.pending_opcode==0x4f);
        unsigned calls=0;fading.scene_fade=[&](unsigned percent,int color){require(percent==50&&color==mode);++calls;};
        fading.tick();require(calls==1&&fading.pc()==0xbe8&&fading.pending_opcode==0x36);
    }
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
    for(auto selector:{0x50000u,0x70000u,0x100000u,0x80028u,0xf0028u}) {
        word(0xbe8,selector);okage::OpeningField special(b,[](int){},[](int){return true;});
        rejects([&]{for(unsigned i=0;i<180;++i)special.tick();});
    }
    word(0xbe4,0x06900047);word(0xbe8,0x000e0007);
    word(0xbec,0x00700162);word(0xbf0,17);word(0xbf4,0);
    {
        okage::OpeningField short_write(b,[](int){},[](int){return true;});
        std::array<std::uint8_t,0x1000> storage{};storage.fill(0xa5);
        for(unsigned i=0;i<180;++i)short_write.tick();
        require(short_write.pc()==0xbe4&&short_write.pending_opcode==0x47);
        unsigned calls=0;
        short_write.set_variable=[&](auto index,auto bank,auto value){
            require(index==7&&bank==14&&value==105);++calls;
            okage::set_event_variable(storage,index,bank,value);
        };
        rejects([&]{short_write.tick();});require(calls==1&&short_write.pc()==0xbec);
        for(unsigned i=0;i<storage.size();++i)
            require(storage[i]==(i==0x7b2?105:i==0x7b3?0:0xa5));
        rejects([&]{short_write.tick();});require(calls==1&&short_write.pc()==0xbec);
    }
    for(auto selector:{0x50000u,0x70000u,0x100000u,0x80028u,0xf0028u}) {
        word(0xbe4,0x100048);word(0xbe8,selector);
        okage::OpeningField invalid_add(b,[](int){},[](int){return true;});
        unsigned calls=0;
        invalid_add.get_variable=[&](auto){++calls;return 0;};
        invalid_add.set_variable=[&](auto,auto,auto){++calls;};
        rejects([&]{for(unsigned i=0;i<180;++i)invalid_add.tick();});require(calls==0);
    }
    for(const auto& test:std::array<std::array<std::int32_t,4>,10>{{
        {2,127,1,-128},{3,-128,-1,127},{4,2147483647,1,(-2147483647-1)},
        {4,(-2147483647-1),-1,2147483647},{0,1,1,1},{1,0,-1,0},
        {8,32767,1,-32768},{14,-32768,-1,32767},{15,-1,1,0},{9,0,-1,-1}}}) {
        const auto bank=unsigned(test[0]);word(0xbe4,(std::uint32_t(test[2])<<20)|0xabc48);
        word(0xbe8,(bank<<16)|7);word(0xbec,0xff800236);
        okage::OpeningField adding(b,[](int){},[](int){return true;});
        std::array<std::uint8_t,0x1000> storage{};okage::set_event_variable(storage,7,bank,test[1]);
        unsigned gets=0,sets=0;
        for(unsigned i=0;i<180;++i)adding.tick();
        require(adding.pc()==(bank>1?0xbe4u:0xbecu));
        adding.get_variable=[&](auto selector){++gets;return okage::event_variable(storage,selector);};
        adding.set_variable=[&](auto index,auto kind,auto value){++sets;okage::set_event_variable(storage,index,kind,value);};
        adding.tick();require(adding.pc()==0xbec&&adding.pending_opcode==0x36);
        require(gets==unsigned(bank>1)&&sets==unsigned(bank>1));
        require(okage::event_variable(storage,(bank<<16)|7)==test[3]);
        adding.tick();require(sets==unsigned(bank>1));
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
    std::vector<std::uint8_t> motion(56);
    auto motion_word=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)motion.at(at+i)=value>>(8*i);};
    motion_word(0,0x544d58);motion_word(4,24);motion_word(16,0xffffffff);
    motion_word(40,0x200002);
    okage::ModelFile model;model.models.emplace_back();
    okage::FieldActor door(std::move(model),motion,{});
    word(0xbe4,0x01200136);word(0xbe8,0x01200009);word(0xbec,0xff800236);
    okage::OpeningField map_event(b,[](int){},[](int){return true;});
    const std::array<okage::ActorBinding,1> binding={{{18,&door}}};
    map_event.bind_map_objects(binding);
    rejects([&]{map_event.bind_map_objects(binding);});
    for(unsigned i=0;i<320&&map_event.pc()!=0xbe8;++i)map_event.tick();
    require(map_event.pc()==0xbe8&&door.motion()==1&&(door.animation().script_state()&0x100));
    map_event.tick();require(map_event.pc()==0xbe8);
    map_event.tick();require(map_event.pc()==0xbe8&&(door.animation().script_state()&0x100));
    map_event.tick();require(map_event.pc()==0xbe8&&!(door.animation().script_state()&0x100));
    map_event.tick();require(map_event.pc()==0xbec&&map_event.pending_opcode==0x36);
    require(map_event.collision_bodies().empty());
    word(0xbe4,0xfda0023c);word(0xbe8,0xfda0013c);word(0xbec,0xff800236);
    okage::OpeningField missing_color(b,[](int){},[](int){return true;});
    for(unsigned i=0;i<180;++i)missing_color.tick();
    require(missing_color.pc()==0xbe4&&missing_color.pending_opcode==0x3c);
    word(0xb6c,0xff800007);word(0xb70,0x400030);word(0xb80,0x10002e);
    word(0xbe4,0xff80023c);word(0xbe8,0xff80013c);word(0xbec,0xff700236);
    const std::array<okage::ActorBinding,1> ghost_binding={{{-8,&door}}};
    door.render_flags=0x80;
    okage::OpeningField color_event(b,[](int){},[](int){return true;},ghost_binding);
    for(unsigned i=0;i<180;++i)color_event.tick();
    require(door.render_flags==0x180080&&color_event.pc()==0xbec);
    word(0xbe4,0xff80003c);word(0xbe8,0xff80033c);
    okage::OpeningField clear_color(b,[](int){},[](int){return true;},ghost_binding);
    for(unsigned i=0;i<180;++i)clear_color.tick();
    require(door.render_flags==0x80&&clear_color.pc()==0xbec);
    door.render_flags=0;
    word(0xbe4,0x0011013b);word(0xbe8,0x0011003b);word(0xbec,0x0010013b);
    okage::OpeningField static_event(b,[](int){},[](int){return true;});
    for(unsigned i=0;i<180;++i)static_event.tick();
    require(static_event.pc()==0xbe4&&static_event.pending_opcode==0x3b);
    std::vector<bool> visibility;
    static_event.map_visibility=[&](int id,bool value){require(id==1);visibility.push_back(value);return true;};
    static_event.tick();require(visibility==std::vector<bool>({true,false}));
    require(static_event.pc()==0xbec&&static_event.pending_opcode==0x3b);
    std::array<okage::CollisionBody,2> visibility_bodies;
    visibility_bodies[0].flags=0xaaaaaaaa;visibility_bodies[1].flags=0xffffffff;
    bool shown=true;
    okage::set_instance_visibility(shown,0,visibility_bodies,false);
    require(!shown&&visibility_bodies[0].flags==0xaaaaaaaa);
    okage::set_instance_visibility(shown,1,visibility_bodies,true);
    require(shown&&visibility_bodies[1].flags==0xfffffffd);
    rejects([&]{okage::set_instance_visibility(shown,2,visibility_bodies,false);});
    require(shown&&visibility_bodies[1].flags==0xfffffffd);
    word(0xb88,0);rejects([&]{okage::OpeningField bad(b,[](int){},[](int){return true;});});
    rejects([&]{okage::field_projection({});});
    b.assign(52,0);word(0,0x475453);word(8,32);word(12,1);word(32,0x44);
    word(36,__builtin_bit_cast(unsigned, (1.f)));word(40,__builtin_bit_cast(unsigned, (.47f)));word(44,__builtin_bit_cast(unsigned, (1024.f)));
    const auto default_projection=okage::field_projection(b);
    word(48,__builtin_bit_cast(unsigned, (9.99f)));require(okage::field_projection(b)==default_projection);
    float far_clip=0;
    word(48,__builtin_bit_cast(unsigned, (450.f)));const auto outdoor_projection=okage::field_projection(b,&far_clip);
    require(far_clip==450);
    require(outdoor_projection[0]==default_projection[0]&&outdoor_projection[1]==default_projection[1]);
    require(outdoor_projection[2]!=default_projection[2]&&outdoor_projection[3]!=default_projection[3]);
    word(48,__builtin_bit_cast(unsigned, (10.f)));require(okage::field_projection(b)!=default_projection);
    {
        word(48,__builtin_bit_cast(unsigned, (450.f)));
        okage::FieldProjection projection;projection.load_stage(b);
        require(projection.current().depth_far==450&&projection.current().clip_far==450);
        require(projection.current().fog_near==4096&&projection.effect_near()==3);
        rejects([&]{projection.begin_effects();});
        projection.begin_field();
        require(projection.frame().shadow.depth_far==450);
        require(projection.frame().ordinary.depth_far==2048&&projection.frame().ordinary.clip_far==450);
        rejects([&]{projection.load_stage(b);});
        projection.begin_effects();
        require(projection.frame().field_end.clip_far==2048);
        require(projection.current().half_width==1024&&projection.current().half_height==512);
        projection.select_effect_near(7);
        require(projection.current().depth_near==3&&projection.current().clip_near==7);
        projection.begin_deferred();
        require(projection.current().half_width==320&&projection.current().half_height==112);
        require(projection.current().fog_far==0&&projection.current().clip_near==7);
        projection.select_effect_near(9);projection.end_effects();
        const auto ending=okage::field_projection_words(projection.frame().ending);
        rejects([&]{projection.select_effect_near(3);});
        projection.begin_field();
        require(okage::field_projection_words(projection.frame().shadow)==ending);
        require(projection.current().clip_near==3&&projection.effect_near()==9);
        projection.begin_effects();require(projection.current().clip_near==9);
        rejects([&]{projection.select_effect_near(450);});
        rejects([&]{projection.select_effect_near(0);});
        projection.begin_deferred();projection.end_effects();
        projection.load_stage(b);
        require(projection.current().depth_far==450&&projection.effect_near()==9);
        const auto scale=projection.current().scale_y;
        projection.reset_scene();
        require(projection.current().depth_far==1024&&projection.current().clip_far==1024);
        require(projection.current().focal==1024&&projection.current().scale_y==scale&&projection.effect_near()==9);
        require(projection.current().half_width==320&&projection.current().half_height==112);
        rejects([&]{projection.begin_field();});projection.load_stage(b);
        // Apply duplicate records in file order; far<10 preserves the last far.
        auto repeated=b;repeated.resize(72);repeated[12]=2;
        for(unsigned j=0;j<20;++j)repeated[52+j]=b[32+j];
        auto put=[&](unsigned at,float v){const auto u=__builtin_bit_cast(unsigned, (v));
            for(unsigned j=0;j<4;++j)repeated.at(at+j)=u>>(j*8);};
        put(64,800);put(68,0);projection.load_stage(repeated);
        require(projection.current().focal==800&&projection.current().clip_far==450);
        repeated[52]=0x43;put(56,20);put(60,100);projection.load_stage(repeated);
        require(projection.current().fog_near==20&&projection.current().fog_far==100);
        // Exercise actual field callback order without constructing any Metal renderer.
        std::vector<std::uint8_t> script(0xc00);
        const auto put_script=[&](unsigned at,unsigned u){for(unsigned j=0;j<4;++j)script.at(at+j)=u>>(j*8);};
        put_script(0,0x544d58);put_script(4,script.size()-32);put_script(16,0xffffffff);
        put_script(0xb88,0xffe00007);put_script(0xb8c,0x30);put_script(0xb9c,0x0ff00002);
        put_script(0xbb8,0x20000a);
        okage::OpeningField native(script,[](int){},[](int){return true;});
        native.render_projection=&projection;
        const auto before=projection.frames();
        for(unsigned i=0;i<4;++i)native.tick();
        require(projection.frames()==before+4);
        require(projection.frame().shadow.depth_far==2048&&projection.frame().shadow.clip_near==9);
        require(projection.frame().ordinary.clip_near==3&&projection.frame().ordinary.fog_near==20);
        require(projection.frame().ending.fog_far==0&&projection.frame().ending.clip_far==450);
        require(std::fegetround()==rounding);
    }
    word(48,0x7fc00000);rejects([&]{okage::field_projection(b);});
    rejects([&]{okage::field_sky_follow({});});
    b.assign(72,0);word(0,0x475453);word(8,32);word(12,0);
    require(okage::field_sky_follow(b)==okage::Float4({.5f,0,.5f,0}));
    word(12,2);word(32,0x46);word(52,0x46);
    for(unsigned i=0;i<4;++i){word(36+i*4,__builtin_bit_cast(unsigned, (1.f)));word(56+i*4,__builtin_bit_cast(unsigned, (float(i)*.25f)));}
    require(okage::field_sky_follow(b)==okage::Float4({0,.25f,.5f,.75f}));
    word(56,0x7fc00000);rejects([&]{okage::field_sky_follow(b);});
    word(12,3);rejects([&]{okage::field_sky_follow(b);});
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
    {
        auto renamed=b;renamed.resize(52+14);
        // Field construction must pass the script-selected map to the loader.
        auto set=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)renamed[at+i]=value>>(i*8);};
        set(4,20);set(12,14);set(32,0x10000059);set(36,0);set(40,0x10100059);set(44,8);set(48,0);
        const char strings[]="variant\0other\0";
        std::copy_n(strings,14,renamed.begin()+52);
        const auto changed=okage::prepare_field_definition(renamed,variables,construction);
        require(changed.room=="variant"&&changed.room_directory=="other");
        require(changed.event==construction.event&&changed.message==construction.message);
    }
    b.resize(56);word(4,24);word(32,(16u<<20)|(1u<<8)|11);word(36,0x40006);word(40,0);
    word(44,(5u<<20)|(9u<<8)|0x59);word(48,0);
    construction.scene=1;construction.party=1;
    for(int disabled:{0,1,-1}) {
        std::array<std::uint8_t,0x1000> vars{};
        okage::set_event_variable(vars,3,0,1);okage::set_event_variable(vars,3,1,1);
        okage::set_event_variable(vars,5,4,disabled);okage::set_event_variable(vars,6,4,17);
        for(unsigned index=25;index<30;++index)okage::set_event_variable(vars,index,4,index==26||index==29?3:2);
        const auto entry=okage::prepare_field_entry(b,vars,construction,1);
        require(entry.party==unsigned(!disabled)&&entry.dust==(disabled?17u:9u));
        require(entry.followers==(disabled?std::vector<unsigned>{}:std::vector<unsigned>{26,29}));
        require(okage::event_variable(vars,0x40006)==bool(disabled));
        require(okage::event_variable(vars,0x40000)==1&&okage::event_variable(vars,0x40001)==1);
        require(okage::event_variable(vars,3)==0&&okage::event_variable(vars,0x10003)==1);
        require(okage::event_variable(vars,0x4001a)==3); // Roster is never forced or cleared.
    }
    {
        std::array<std::uint8_t,0x1000> vars{};
        const auto entry=okage::prepare_field_entry(b,vars,construction,0);
        require(entry.party==1&&entry.followers.empty()&&okage::event_variable(vars,0x40006)==0);
        construction.party=0;okage::set_event_variable(vars,25,4,3);
        const auto solo=okage::prepare_field_entry(b,vars,construction,0);
        require(solo.party==0&&solo.followers.empty()&&okage::event_variable(vars,0x40006)==1);
        rejects([&]{okage::prepare_field_entry(b,std::span(vars).first(0x587),construction,0);});
    }
    auto put=[](auto& data,unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)data.at(at+i)=value>>(i*8);};
    std::vector<std::uint8_t> stage(116),empty_map(69),idle(56),startup(56);
    put(stage,0,0x475453);put(stage,16,32);put(stage,20,1);put(stage,24,88);put(stage,28,1);
    put(stage,64,__builtin_bit_cast(std::uint32_t, (10.f)));put(stage,76,__builtin_bit_cast(std::uint32_t, (1.f)));
    for(unsigned i=0;i<3;++i)put(empty_map,i*4,1);put(empty_map,65,64);
    for(auto* data:{&idle,&startup}){put(*data,0,0x544d58);put(*data,4,24);put(*data,16,0xffffffff);}
    {
        auto event=startup;
        put(event,40,0x00700162);put(event,44,17);put(event,48,0x00200002);put(event,52,0);
        auto hero_motion=idle;hero_motion.resize(76);put(hero_motion,4,44);
        put(hero_motion,56,2);put(hero_motion,60,0x0040002e);
        put(hero_motion,64,__builtin_bit_cast(std::uint32_t, (99.f)));put(hero_motion,68,0);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),hero_motion,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};std::vector<unsigned> phases;
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        rejects([&]{scene.battle_return_snapshot();}); // No entered definition yet.
        std::optional<okage::FieldBattleReturnSnapshot> saved;
        bool block=true;
        scene.battle_transition_service=[&](unsigned phase,const okage::FieldBattleRequest& request) {
            require(!scene.event_running()&&scene.exploration_ready());
            require(request.formation==1&&request.stage==17&&request.return_token==0x1000);
            if(block)throw std::runtime_error("Unimplemented capture layer");
            if(phase==13)saved=scene.battle_return_snapshot();
            phases.push_back(phase);
        };
        okage::FieldDefinition entry;entry.scene=96;entry.type=1;entry.startup=1;entry.secondary_music=28;
        scene.enter(entry,stage,empty_map,0);
        require(scene.battle_request()&&scene.battle_request()->phase==0&&scene.event_running());
        require(scene.pc()==52&&phases.empty());
        for(unsigned i=0;i<4&&scene.event_running();++i) {
            scene.tick();require(phases.empty()&&scene.battle_request()->phase==0);
        }
        require(!scene.event_running()&&!scene.exploration_ready());
        scene.tick();require(scene.exploration_ready()&&scene.battle_request()->phase==0);
        scene.tick();require(scene.battle_request()->phase==11&&phases.empty());
        rejects([&]{scene.tick();});require(scene.battle_request()->phase==11&&!scene.battle_request()->dispatched);
        block=false;
        scene.tick();require(scene.battle_request()->phase==12);
        const auto before=scene.battle_return_snapshot();
        const okage::Float4 blended_from{200,300,400,7};
        scene.blend.begin(.25f,blended_from,{0,0,0,1});
        scene.tick();require(scene.battle_request()->phase==13);
        require(scene.position==blended_from&&scene.position!=before.raw_camera_position);
        const okage::Float4 player_position{13,-7,29,2.5f},player_rotation{-.125f,-0.f,.5f,-2};
        hero.animation().set_channel(0,player_rotation);hero.animation().set_channel(1,player_position);
        hero.animation().start_at(56);
        scene.tick();require(saved.has_value());
        require(saved->scene==96&&saved->secondary_music==28);
        auto equal_bits=[](const okage::Float4& a,const okage::Float4& b) {
            for(unsigned i=0;i<4;++i)require(__builtin_bit_cast(std::uint32_t, (a[i]))==__builtin_bit_cast(std::uint32_t, (b[i])));
        };
        equal_bits(saved->player_position,player_position);equal_bits(saved->player_rotation,player_rotation);
        equal_bits(saved->raw_camera_position,before.raw_camera_position);
        require(hero.animation().values()[1][0]==99&&saved->player_position[0]==13);
        require(saved->raw_camera_position!=scene.position);
        require(phases==std::vector<unsigned>({11,12,13}));
        require(scene.battle_request()->phase==13&&scene.battle_request()->dispatched);
        rejects([&]{scene.tick();});require(phases.size()==3);
    }
    {
        auto event=startup;event.resize(100);put(event,4,68);
        put(event,40,1);put(event,44,20); // Entry1 jumps to the hat request.
        put(event,48,0xff91003b);put(event,52,0x00200002); // Entry2 hides alias7.
        put(event,64,0x4d);put(event,68,0x00200002);
        put(event,72,0xff91013b);put(event,76,0xffe00036);
        put(event,80,0x00600002);put(event,84,0xff91003b);
        put(event,88,0x00100002);put(event,92,0);
        std::vector<std::uint8_t> camera(72);
        put(camera,0,0x544d58);put(camera,4,40);put(camera,16,0xffffffff);
        put(camera,32,0x30);put(camera,36,__builtin_bit_cast(unsigned, (4.f)));
        put(camera,40,__builtin_bit_cast(unsigned, (-5.f)));put(camera,44,__builtin_bit_cast(unsigned, (6.f)));
        put(camera,48,0x00400030);put(camera,64,0x00200002);put(camera,68,0);
        auto motions=idle;motions.resize(80);put(motions,4,48);
        okage::ModelFile hm,em,nm;hm.models.emplace_back();em.models.emplace_back();nm.models.emplace_back();
        okage::FieldActor hero(std::move(hm),motions,{}),hat(std::move(em),idle,{},true),npc(std::move(nm),motions,{});
        require(hat.channels()[1]==okage::Float4({1,1,1,1}));
        const std::array<okage::ActorBinding,2> bindings={{{-8,&npc},{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},bindings,false);
        std::array<std::uint8_t,0x1000> vars{};unsigned loads=0;
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        scene.create_hat_scene=[&]{++loads;return okage::FieldEventActor{&hat,camera};};
        scene.random_value=[] {return 0u;};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        okage::FieldNpcDefinition person;person.alias=-8;person.talk=-2;person.position={3,0,0,1};
        entry.npcs.push_back(person);
        scene.enter(entry,stage,empty_map,0);require(loads==0);
        scene.tick();require(loads==1&&!hat.visible&&scene.collision_bodies().size()==2);
        require(hat.channels()[1]==okage::Float4({1,1,1,1}));
        scene.tick();require(hat.visible&&scene.camera_mode()==1);
        require(scene.position==okage::Float4({4,-5,6,1}));
        require(hat.channels()[1]==okage::Float4({42.5f,__builtin_bit_cast(float, (0x401eb852u)),757.f,1}));
        require(scene.rotation==okage::camera_look_at(scene.position,scene.target,angles,sine));
        scene.tick();scene.tick();scene.tick();require(scene.camera_mode()==0);
        for(unsigned i=0;i<5;++i)scene.tick();require(!hat.visible&&loads==1);
        // A subsequent NPC interaction overwrites alias7 (0x132b00) while
        // the event actor remains owned and eligible for ordinary updates.
        for(unsigned i=0;i<16&&scene.event_running();++i)scene.tick();
        require(!scene.event_running());scene.tick();
        hat.visible=true;scene.tick({0,128,128,0x40});
        require(scene.interaction_alias()==-8&&!npc.visible&&hat.visible&&loads==1);
    }
    {
        auto event=startup;event.resize(68);put(event,4,36);
        put(event,40,0xfff00007);put(event,44,0x00400433);
        put(event,48,__builtin_bit_cast(std::uint32_t, (8.f)));
        put(event,52,__builtin_bit_cast(std::uint32_t, (1.f)));
        put(event,56,__builtin_bit_cast(std::uint32_t, (10.f)));
        put(event,60,0x00400008);put(event,64,0);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        scene.enter(entry,stage,empty_map,0);
        require(scene.event_running()&&hero.animation().values()[1][0]==2.f);
        scene.tick();require(hero.animation().values()[1][0]==4.f);
    }
    for(unsigned flags:{1u,2u,4u,8u,12u}) {
        okage::ModelFile hm,nm;hm.models.emplace_back();nm.models.emplace_back();
        okage::FieldActor hero(std::move(hm),idle,{}),npc(std::move(nm),idle,{});
        const std::array<okage::ActorBinding,2> bindings={{{-8,&npc},{-1,&hero}}};
        okage::OpeningField scene(startup,[](int){},[](int){return true;},bindings,false);
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        scene.random_value=[] {return 1u;};
        okage::FieldDefinition entry;entry.type=1;
        entry.npcs.push_back({-8,"imo",0,-30,0,{100,0,100,1},0,flags});
        scene.enter(entry,stage,empty_map,0);
        require(npc.visible==!(flags&2));
        require(npc.shadow_mode_override==((flags&8)?std::optional<unsigned>(6):
            (flags&4)?std::optional<unsigned>(2):std::nullopt));
        require(scene.collision_bodies().at(0).flags==1);
    }
    {
        auto event=startup;put(event,40,0xfff0004b);put(event,44,0xb400002);put(event,48,0);
        okage::ModelFile model;model.models.emplace_back();
        okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto selector){return okage::event_variable(vars,selector);};
        scene.set_variable=[&](auto index,auto bank,auto value){okage::set_event_variable(vars,index,bank,value);};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        scene.enter(entry,stage,empty_map,0);require(scene.camera_mode()==3);
        hero.animation().set_channel(1,{3,4,5,1});scene.tick();
        require(scene.target==okage::Float4({3,4,5,1}));
        scene.blend.begin(.5f,scene.position,{1,2,3,1});
        hero.animation().set_channel(1,{5,6,7,1});scene.tick();
        require(scene.target==okage::Float4({1,2,3,1})&&scene.blend.phase==.5f);
        hero.animation().set_channel(1,{9,10,11,1});scene.tick();
        require(scene.target==okage::Float4({5,6,7,1})&&scene.blend.phase==1);
        hero.animation().turn_component(1,0,13,2);
        const auto previous=hero.animation().values()[1];scene.tick();
        require(scene.target==previous);
        require(scene.rotation==okage::camera_look_at(scene.position,scene.target,angles,sine));
        put(event,40,0xfed0004b);
        okage::OpeningField missing(event,[](int){},[](int){return true;},player,false);
        missing.get_variable=scene.get_variable;missing.set_variable=scene.set_variable;
        rejects([&]{missing.enter(entry,stage,empty_map,0);});
    }
    // Initial gate, written index/value, initial party-disable, expected gate.
    const std::array<std::array<int,5>,6> gate_cases={{{0,5,0,0,0},{0,5,1,0,1},{2,5,1,0,2},
                                                   {0,6,1,0,1},{1,6,0,0,0},{1,6,0,1,1}}};
    for(bool waited:{false,true})for(const auto test:gate_cases) {
        put(startup,44,0x40000|test[1]);
        put(startup,40,(std::uint32_t(test[2])<<20)|0x47);put(startup,48,waited?0x200002:0);
        okage::ModelFile hero_model;hero_model.models.emplace_back();
        okage::FieldActor hero(std::move(hero_model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(startup,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};okage::set_event_variable(vars,6,4,test[0]);
        okage::set_event_variable(vars,5,4,test[3]);
        scene.get_variable=[&](auto selector){return okage::event_variable(vars,selector);};
        scene.set_variable=[&](auto index,auto bank,auto value){okage::set_event_variable(vars,index,bank,value);};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        scene.enter(entry,stage,empty_map,0);
        if(waited){require(okage::event_variable(vars,0x40006)==(test[1]==6?test[2]:1));for(unsigned i=0;i<4;++i)scene.tick();}
        require(!scene.event_running()&&okage::event_variable(vars,0x40006)==test[4]);
    }
    idle.resize(80);put(idle,4,48);
    {
        auto room=stage;
        put(room,36,__builtin_bit_cast(unsigned, (5.f)));put(room,40,__builtin_bit_cast(unsigned, (-10.f)));
        put(room,44,__builtin_bit_cast(unsigned, (-7.f)));put(room,68,__builtin_bit_cast(unsigned, (.8f)));
        put(room,72,__builtin_bit_cast(unsigned, (-3.f)));
        put(room,92,__builtin_bit_cast(unsigned, (4.f)));put(room,100,__builtin_bit_cast(unsigned, (8.f)));
        okage::ModelFile moving_model,still_model;moving_model.models.emplace_back();still_model.models.emplace_back();
        okage::FieldActor moving(std::move(moving_model),idle,{}),still(std::move(still_model),idle,{});
        const std::array<okage::ActorBinding,1> moving_binding={{{-1,&moving}}},still_binding={{{-1,&still}}};
        okage::OpeningField walk(startup,[](int){},[](int){return true;},moving_binding,false);
        okage::OpeningField stay(startup,[](int){},[](int){return true;},still_binding,false);
        std::array<std::uint8_t,0x1000> walk_vars{},stay_vars{};
        auto initialize=[&](auto& scene,auto& vars) {
            scene.get_variable=[&vars](auto s){return okage::event_variable(vars,s);};
            scene.set_variable=[&vars](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
            okage::FieldDefinition entry;entry.type=1;scene.enter(entry,room,empty_map,0);
        };
        initialize(walk,walk_vars);initialize(stay,stay_vars);
        walk.tick({0,40,128});stay.tick();
        require(walk.position==stay.position&&walk.rotation==stay.rotation&&walk.target==stay.target);
        const auto moved=moving.animation().values()[1];
        require(moved!=still.animation().values()[1]);
        require(moving.animation().pc()==48&&(moving.animation().script_state()&0x100));
        walk.tick();
        require(walk.target[0]==moved[0]&&walk.target[2]==moved[2]);
    }
    {
        okage::ModelFile hm,nm,sm;hm.models.emplace_back();nm.models.emplace_back();sm.models.emplace_back();
        okage::FieldActor hero(std::move(hm),idle,{}),npc(std::move(nm),idle,{}),stun(std::move(sm),idle,{});
        const std::array<okage::ActorBinding,3> bindings={{{-8,&npc},{-3,&stun,false},{-1,&hero}}};
        auto event=startup;put(event,40,0x40000002);put(event,44,0);
        okage::OpeningField scene(event,[](int){},[](int){return true;},bindings,false);
        require(!stun.visible&&stun.motion()==1&&stun.animation().values()[1]==okage::Float4{0,-2,0,1});
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        scene.random_value=[] {return 0u;};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        entry.npcs.push_back({-8,"npc",0,0,0,{-100,0,-100,1},0,1});
        scene.enter(entry,stage,empty_map,0);
        require(scene.collision_bodies().size()==2&&scene.collision_bodies()[1].id==1);
        require(scene.player_movement().mask&1); // Bit0 must not suppress follow.
        hero.animation().set_channel(0,{.1f,.25f,.5f,1});hero.animation().set_channel(1,{3,2,7,1});
        scene.tick();
        const auto& shadow=*scene.player_shadow();
        require(shadow.phase==0&&!shadow.stun_visible&&hero.visible&&!hero.shadow_mode_override);
        require(stun.animation().values()[0]==hero.animation().values()[0]);
        require(stun.animation().values()[1]==shadow.stun_position&&shadow.stun_position[0]==3&&shadow.stun_position[2]==7);
        require(shadow.stun_position[1]<2&&shadow.stun_position[1]>1.98f);
        auto invalid=bindings;invalid[1].collidable=true;
        rejects([&]{okage::OpeningField bad(event,[](int){},[](int){return true;},invalid,false);});
    }
    for(unsigned selector:{0u,1u,2u}) {
        auto event=startup;put(event,40,((0x100u|selector)<<20)|0xabc54u);put(event,44,0x10000a);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> bindings={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return false;},bindings,false);
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;scene.enter(entry,stage,empty_map,0);
        require(!scene.player_shadow()&&scene.pc()==44&&hero.visible);
        require(hero.shadow_mode_override.has_value()&&*hero.shadow_mode_override==selector*4);
    }
    for(int camera_command:{0,1,2}) {
        const bool frozen_camera=camera_command!=2;
        auto exterior_stage=stage;put(exterior_stage,76,0);
        const auto settings=okage::field_camera_settings(exterior_stage);
        require(settings.size()==1&&settings[0].id==0&&settings[0].height==10&&settings[0].mode==0);
        auto event=startup;event.resize(64);put(event,4,32);
        put(event,40,camera_command==1?0x53:0x100002);
        put(event,44,camera_command==2?0x53:0x200002);
        put(event,48,camera_command==2?0x200002:0);put(event,52,0);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        okage::ExteriorCamera manager;scene.exterior_camera=&manager;
        manager.snap_target=false;manager.target_y=100;
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto selector){return okage::event_variable(vars,selector);};
        scene.set_variable=[&](auto index,auto bank,auto value){okage::set_event_variable(vars,index,bank,value);};
        okage::FieldDefinition entry;entry.type=0;entry.startup=1;entry.party=1;
        scene.enter(entry,exterior_stage,empty_map,0);const auto frozen=scene.position;
        require(manager.snap_target&&manager.target_y==100);
        require(scene.event_running()&&!manager.manual_rotate);
        for(unsigned i=0;i<(camera_command==1?2u:4u);++i){scene.tick();if(frozen_camera)require(scene.position==frozen);}
        const float offset_before_handoff=manager.target_offset;
        require(!scene.event_running());scene.tick();require(manager.manual_rotate&&manager.phase==0);
        if(!frozen_camera)require(manager.target_offset<offset_before_handoff);
        const auto previous=hero.animation().values()[1];scene.tick({0x4000});
        require(!manager.snap_target&&manager.target_y==previous[1]);
        require(manager.phase>0&&manager.previous_player==previous&&scene.camera_mode()==2);
        require(manager.previous_player!=hero.animation().values()[1]);
    }
    {
        auto exterior_stage=stage;put(exterior_stage,76,0);
        auto event=startup;put(event,40,0x00400002);put(event,44,0);put(event,48,0);
        okage::ModelFile hero_model,npc_model;
        hero_model.models.emplace_back();npc_model.models.emplace_back();
        okage::FieldActor hero(std::move(hero_model),idle,{}),npc(std::move(npc_model),idle,{});
        const std::array<okage::ActorBinding,2> actors={{{-8,&npc},{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},actors,false);
        scene.random_value=[] {return 0u;};
        okage::ExteriorCamera manager;scene.exterior_camera=&manager;
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        okage::FieldDefinition entry;entry.type=0;entry.party=1;
        okage::FieldNpcDefinition person;person.alias=-8;person.talk=-1;person.position={3,0,0,1};
        entry.npcs.push_back(person);
        scene.enter(entry,exterior_stage,empty_map,0);scene.tick();
        manager.snap_target=false;manager.target_y=100;
        scene.tick({0,128,128,0x40});
        require(scene.event_running()&&scene.interaction_alias()==-8&&manager.target_y==99.5f);
        scene.tick();require(manager.target_y==99.5f);
    }
    put(startup,40,0x144);put(startup,44,0);
    {
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(startup,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto selector){return okage::event_variable(vars,selector);};
        scene.set_variable=[&](auto index,auto bank,auto value){okage::set_event_variable(vars,index,bank,value);};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;entry.music=10;
        scene.enter(entry,stage,empty_map,0);require(scene.pending_opcode==0x44&&scene.event_running());
        unsigned calls=0;scene.music_service=[&](int id,bool fade){require(id==10&&fade);++calls;return true;};
        scene.tick();require(calls==1&&!scene.event_running()&&scene.pending_opcode==0);
        entry.followers={25};
        okage::OpeningField unsupported(startup,[](int){},[](int){return true;},player,false);
        rejects([&]{unsupported.enter(entry,stage,empty_map,0);});
    }
    {
        // Opcode5f consumes one payload word and waits for an audio binding.
        auto script=startup;script.resize(52);put(script,4,script.size()-32);
        put(script,40,0x75f);put(script,44,128);put(script,48,0);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(script,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};
        scene.get_variable=[&](auto selector){return okage::event_variable(vars,selector);};
        scene.set_variable=[&](auto index,auto bank,auto value){okage::set_event_variable(vars,index,bank,value);};
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        scene.enter(entry,stage,empty_map,0);require(scene.pending_opcode==0x5f);
        unsigned calls=0;scene.music_channels=[&](int first,unsigned last,unsigned level){
            require(first==0&&last==7&&level==128);++calls;return true;};
        scene.tick();require(calls==1&&scene.pending_opcode==0);
    }
    {
        std::vector<std::uint8_t> efd(64+8*116),map(64+148+3*80),event(96);
        put(efd,0,0x20444645);put(efd,4,1);put(efd,16,25);put(efd,20,10);put(efd,24,0x8000);
        put(efd,32,8);put(efd,36,64);
        for(unsigned i=0;i<8;++i){const auto at=64+116*i;put(efd,at+8,29);put(efd,at+28,__builtin_bit_cast(std::uint32_t, (2.f)));}
        put(map,8,212);put(map,12,3);put(map,40,64);put(map,44,1);map[64]='e';
        for(unsigned i=0;i<3;++i){const auto at=212+80*i;put(map,at+12,0x19100000);put(map,at+16,__builtin_bit_cast(std::uint32_t, (float(i))));}
        put(event,0,0x544d58);put(event,4,event.size()-32);put(event,16,0xffffffff);
        put(event,40,0x102ffd5d);put(event,44,__builtin_bit_cast(std::uint32_t, (.75f)));
        put(event,48,__builtin_bit_cast(std::uint32_t, (.25f)));put(event,52,__builtin_bit_cast(std::uint32_t, (.25f)));
        put(event,56,0x1001005c);put(event,60,0x1011005c);put(event,64,0x20003f);
        put(event,68,0x100061);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};okage::EffectTextureSizes sizes{};sizes[5]={32,24};
        for(unsigned i:{13u,14u,15u})okage::set_event_variable(vars,i,4,2);
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        unsigned calls=0;scene.random_value=[&]{return ++calls;};scene.load_effects(efd);scene.load_map_effects(map,sizes);
        require(calls==3&&scene.map_effects()[0]->phase==1&&scene.map_effects()[2]->phase==3);
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        scene.enter(entry,stage,empty_map,0);
        for(unsigned i:{13u,14u,15u})require(okage::event_variable(vars,0x40000|i)==0);
        require(scene.collision_mask->bits==4&&scene.map_effects()[2]->color==okage::Float4({.75f,.25f,.25f,1}));
        // Entry itself runs the first removal pass; the next update frees them.
        require((scene.map_effects()[0]->flags&0x200)&&(scene.map_effects()[1]->flags&0x200));
        scene.tick();require(!scene.map_effects()[0]&&!scene.map_effects()[1]&&scene.map_effects()[2]);
        rejects([&]{scene.load_map_effects(map,sizes);});
        okage::OpeningField omitted(event,[](int){},[](int){return true;},player,false);
        omitted.get_variable=scene.get_variable;omitted.set_variable=scene.set_variable;
        rejects([&]{omitted.enter(entry,stage,empty_map,0);});
        put(event,40,0x100061);
        okage::OpeningField queued(event,[](int){},[](int){return true;},player,false);
        queued.get_variable=scene.get_variable;queued.set_variable=scene.set_variable;
        queued.random_value=scene.random_value;
        queued.load_effects(efd);queued.load_map_effects(map,sizes);
        queued.map_effects()[0]->flags|=0x400;
        queued.map_effects()[1]->flags|=0x4000;
        const auto dormant_phase=queued.map_effects()[1]->phase;
        queued.enter(entry,stage,empty_map,0);
        require(queued.effect_submissions().size()==2);
        require(queued.effect_submissions()[0].handle==2&&queued.effect_submissions()[1].handle==0);
        require(queued.map_effects()[1]->phase==dormant_phase);
        const auto& submitted=std::get<okage::MapEffect>(queued.effect_submissions()[1].effect);
        const auto color=submitted.color;
        queued.map_effects()[0]->request_removal();queued.map_effects()[0]->color={0,0,0,0};
        require(!(submitted.flags&0x100)&&submitted.color==color);
        queued.tick();require(queued.effect_submissions().size()==1&&queued.effect_submissions()[0].handle==2);
        require(queued.map_effects()[0]->flags&0x200);
        queued.tick();require(!queued.map_effects()[0]);
    }
    {
        std::vector<std::uint8_t> efd(64+8*116),event(40+17*16+28);
        put(efd,0,0x20444645);put(efd,4,1);put(efd,16,25);put(efd,20,10);
        put(efd,32,8);put(efd,36,64);
        for(unsigned i=0;i<8;++i){const auto at=64+116*i;
            put(efd,at+8,29);put(efd,at+28,__builtin_bit_cast(std::uint32_t, (2.f)));}
        put(event,0,0x544d58);put(event,4,event.size()-32);put(event,16,0xffffffff);
        for(unsigned i=0;i<17;++i){const auto at=40+i*16;
            put(event,at,0x0191233e); // A is deliberately nonzero and ignored.
            put(event,at+4,__builtin_bit_cast(std::uint32_t, (float(i))));
            put(event,at+8,__builtin_bit_cast(std::uint32_t, (-1.25f)));
            put(event,at+12,__builtin_bit_cast(std::uint32_t, (3.5f)));}
        put(event,312,0x1005c);put(event,316,0x00f1005c);put(event,320,0x100061);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        std::array<std::uint8_t,0x1000> vars{};okage::EffectTextureSizes sizes{};sizes[5]={32,24};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        unsigned calls=0;scene.random_value=[&]{return ++calls;};
        std::vector<std::uint8_t> no_placements(64);put(no_placements,8,64);put(no_placements,40,64);
        scene.load_effects(efd);scene.load_map_effects(no_placements,sizes);
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;
        scene.enter(entry,stage,empty_map,0);
        require(calls==0);
        for(unsigned i=0;i<17;++i){const auto& e=scene.map_effects()[i];
            require(e&&e->position==okage::Float4({float(i),-1.25f,3.5f,1}));
            require(bool(e->flags&0x200)==(i==1||i==16));
            require(e->phase==(i==1||i==16?1u:2u));}
        scene.tick();require(!scene.map_effects()[1]&&!scene.map_effects()[16]&&scene.map_effects()[0]);
    }
    {
        std::vector<std::uint8_t> efd(64+4*116),event(128),no_placements(64);
        put(efd,0,0x20444645);put(efd,4,1);put(efd,16,6);put(efd,20,70);put(efd,24,0x400);
        put(efd,32,4);put(efd,36,64);
        for(unsigned i=0;i<4;++i){const auto at=64+116*i;
            put(efd,at+4,i+1);put(efd,at+8,i%2?9:7);put(efd,at+12,0x20);
            put(efd,at+28,__builtin_bit_cast(std::uint32_t, (2.f)));}
        put(event,0,0x544d58);put(event,4,event.size()-32);put(event,16,0xffffffff);
        for(unsigned i=0;i<17;++i)put(event,40+i*4,((i%2?89u:0xfecu)<<20)|0x639);
        put(event,108,0x1005c);put(event,112,0x00f1005c);put(event,116,0x100061);
        put(no_placements,8,64);put(no_placements,40,64);
        okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
        const std::array<okage::ActorBinding,1> player={{{-1,&hero,true,89}}};
        okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
        scene.resolve_instance_alias=[](int alias)->std::optional<int>{return alias==-20?std::optional<int>(89):std::nullopt;};
        std::array<std::uint8_t,0x1000> vars{};okage::EffectTextureSizes sizes{};
        scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
        scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
        unsigned calls=0;scene.random_value=[&]{return ++calls;};
        scene.load_effects(efd);scene.load_map_effects(no_placements,sizes);
        okage::FieldDefinition entry;entry.type=1;entry.startup=1;scene.enter(entry,stage,empty_map,0);
        require(calls==0&&!scene.pending_opcode&&scene.map_effects()[0]);
        for(unsigned i=0;i<17;++i){const auto& e=scene.map_effects()[i];
            require(e&&e->actor_alias==(i%2?89:-20));
            require(bool(e->flags&0x200)==(i==1||i==16));}
        const okage::Float4 rotation{.125f,.25f,.5f,1},position{13,-7,29,1};
        hero.animation().set_channel(0,rotation);
        hero.animation().set_channel(1,position);scene.tick();
        require(!scene.map_effects()[1]&&!scene.map_effects()[16]&&scene.effect_submissions().size()==15);
        for(const auto& submission:scene.effect_submissions()){
            const auto& e=std::get<okage::MapEffect>(submission.effect);
            require(e.position==position&&e.rotation==rotation);
        }
        scene.resolve_instance_alias=[](int)->std::optional<int>{return std::nullopt;};
        rejects([&]{scene.tick();});
    }
    {
        std::vector<std::uint8_t> efd(64+8*116),event(64),no_placements(64);
        put(efd,0,0x20444645);put(efd,4,1);put(efd,16,24);put(efd,20,80);put(efd,32,8);put(efd,36,64);
        for(unsigned i=0;i<8;++i) {
            const auto at=64+116*i;put(efd,at+4,i+1);put(efd,at+8,i<3?25:i==3?3:i==4?55:31);
            put(efd,at+12,0x21);put(efd,at+24,0xffffffff);put(efd,at+28,__builtin_bit_cast(std::uint32_t, (2.f)));
        }
        put(event,0,0x544d58);put(event,4,event.size()-32);put(event,16,0xffffffff);
        put(event,40,(24u<<20)|0x3e);put(event,56,0x100061);
        put(no_placements,8,64);put(no_placements,40,64);
        okage::EffectTextureSizes sizes;for(auto& size:sizes)size={64,64};
        unsigned calls=0;const auto random=[&]{return ++calls;};
        auto prepared=std::make_shared<const okage::EffectTemplatePreparation>(okage::read_efd(efd),sizes,okage::make_sine_table(),random);
        require(calls==404);const auto* vertices=&prepared->geometry[55]->vertices;
        std::shared_ptr<const std::vector<okage::MapEffectVertex>> retained;
        for(unsigned visit=0;visit<2;++visit) {
            okage::ModelFile model;model.models.emplace_back();okage::FieldActor hero(std::move(model),idle,{});
            const std::array<okage::ActorBinding,1> player={{{-1,&hero}}};
            okage::OpeningField scene(event,[](int){},[](int){return true;},player,false);
            std::array<std::uint8_t,0x1000> vars{};
            scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
            scene.set_variable=[&](auto i,auto b,auto v){okage::set_event_variable(vars,i,b,v);};
            scene.random_value=random;scene.load_effects(efd,prepared);scene.load_map_effects(no_placements,sizes);
            okage::FieldDefinition entry;entry.type=1;entry.startup=1;scene.enter(entry,stage,empty_map,0);
            require(scene.map_effects()[0]&&scene.map_effects()[0]->shape55_geometry.get()==vertices&&calls==404);
            retained=scene.map_effects()[0]->shape55_geometry;
        }
        {
            std::vector<std::uint8_t> transition(128);
            put(transition,0,0x544d58);put(transition,4,transition.size()-32);put(transition,16,0xffffffff);
            unsigned pc=40;
            const auto command=[&](unsigned mode){put(transition,pc,(mode<<20)|0x54);pc+=4;};
            const auto barrier=[&](unsigned n){put(transition,pc,(n<<20)|0x0a);pc+=4;};
            command(5);command(1);barrier(1); // Locked request has no side effects.
            command(4);command(1);command(5);barrier(2);
            command(3);barrier(3);
            command(4);command(1);command(2);barrier(4); // Duplicate request is ignored.
            command(0);barrier(5);
            command(0x81);barrier(6);
            okage::ModelFile hm,sm;hm.models.emplace_back();sm.models.emplace_back();
            okage::FieldActor hero(std::move(hm),idle,{}),stun(std::move(sm),idle,{});
            const std::array<okage::ActorBinding,2> bindings={{{-3,&stun,false},{-1,&hero}}};
            unsigned gate=0,sounds=0,writes=0,attached_handle=84;
            okage::OpeningField scene(transition,[](int){},[&](int n){return gate>=unsigned(n);},bindings,false);
            std::array<std::uint8_t,0x1000> vars{};
            scene.get_variable=[&](auto s){return okage::event_variable(vars,s);};
            scene.sound_cue=[&](unsigned bank,int cue,float speed,unsigned gain) {
                require(bank==0&&speed==1&&sounds<2&&writes==sounds);
                require(cue==(sounds?12:23)&&gain==(sounds?64u:100u));
                require(scene.player_shadow()->phase==(sounds?4u:1u)&&scene.player_shadow()->factor==1);
                ++sounds;return true;
            };
            scene.set_variable=[&](auto i,auto b,auto v) {
                if(i==2&&b==4) {
                    require(v==(writes==1?0:1));
                    require(scene.player_shadow()->phase==(v?1u:4u)&&scene.player_shadow()->factor==1);
                    attached_handle=84;
                    for(unsigned slot=0;slot<scene.map_effects().size();++slot) {
                        const auto& effect=scene.map_effects()[slot];
                        if(effect&&effect->id==24&&effect->phase==1) {
                            require(attached_handle==84);attached_handle=slot;
                            require(effect->actor_alias==-1&&effect->position==okage::Float4{}&&effect->rotation==okage::Float4{});
                            require(effect->shape55_geometry.get()==vertices);
                        }
                    }
                    require(attached_handle<84);++writes;
                }
                okage::set_event_variable(vars,i,b,v);
            };
            scene.random_value=random;scene.load_effects(efd,prepared);scene.load_map_effects(no_placements,sizes);
            okage::FieldDefinition entry;entry.type=1;entry.startup=1;scene.enter(entry,stage,empty_map,0);
            require(scene.player_shadow()->locked&&scene.player_shadow()->phase==0&&sounds==0&&writes==0);
            require(scene.effect_submissions().empty()&&!scene.map_effects()[0]);
            require(scene.request_player_shadow(0x104)&&!scene.player_shadow()->locked);
            require(scene.request_player_shadow(0x105)&&scene.player_shadow()->locked);
            require(!hero.shadow_mode_override); // Dialogue bit0x100 is not a selector override.
            gate=1;scene.tick();
            require(sounds==1&&writes==1&&okage::event_variable(vars,0x40002)==1);
            require(scene.player_shadow()->locked&&scene.player_shadow()->phase==1&&scene.player_shadow()->factor<1);
            require(scene.map_effects()[attached_handle]->phase==2&&scene.effect_submissions().size()==1);
            require(scene.map_effects()[attached_handle]->position==hero.animation().values()[1]);
            const auto factor=scene.player_shadow()->factor;scene.tick();
            require(scene.player_shadow()->locked&&scene.player_shadow()->factor<factor);
            const auto frozen_position=stun.animation().values()[1],frozen_rotation=stun.animation().values()[0];
            hero.animation().set_channel(0,{.1f,.2f,.3f,1});hero.animation().set_channel(1,{4,2,7,1});
            gate=2;scene.tick();
            require(scene.player_shadow()->follow_disabled&&stun.animation().values()[1]==frozen_position);
            require(stun.animation().values()[0]==frozen_rotation);
            require(scene.map_effects()[attached_handle]->position==hero.animation().values()[1]);
            require(scene.map_effects()[attached_handle]->rotation==hero.animation().values()[0]);
            const auto snapshot=std::get<okage::MapEffect>(scene.effect_submissions()[0].effect);
            hero.animation().set_channel(1,{8,3,9,1});scene.tick();
            require(stun.animation().values()[1]==frozen_position&&snapshot.position==okage::Float4({4,2,7,1}));
            require(scene.map_effects()[attached_handle]->position==okage::Float4({8,3,9,1}));
            gate=3;scene.tick();
            require(!scene.player_shadow()->locked&&!scene.player_shadow()->follow_disabled&&sounds==1&&writes==1);
            require(stun.animation().values()[0]==hero.animation().values()[0]&&stun.animation().values()[1][0]==8);
            for(unsigned tick=0;tick<100&&scene.player_shadow()->phase==1;++tick)scene.tick();
            require(scene.player_shadow()->phase==2&&hero.shadow_mode_override==0xffff&&!stun.visible&&hero.visible);
            scene.tick();require(stun.visible&&stun.animation().values()[2]==scene.player_shadow()->stun_scale);
            for(unsigned tick=0;tick<100&&scene.player_shadow()->phase!=3;++tick)scene.tick();
            require(scene.player_shadow()->phase==3&&stun.visible&&stun.animation().values()[2]==okage::Float4({1,1,1,1}));
            require(scene.request_player_shadow(0x100));
            require(scene.player_shadow()->phase==4&&scene.player_shadow()->factor==1);
            require(hero.shadow_mode_override==0xffff&&sounds==2&&writes==2);
            gate=4;scene.tick();
            require(sounds==2&&writes==2&&okage::event_variable(vars,0x40002)==0&&scene.player_shadow()->phase==4);
            for(unsigned tick=0;tick<100&&scene.player_shadow()->phase==4;++tick)scene.tick();
            require(scene.player_shadow()->phase==5&&!stun.visible&&hero.shadow_mode_override==0xffff);
            scene.tick();require(hero.shadow_mode_override==4&&hero.visible);
            for(unsigned tick=0;tick<100&&scene.player_shadow()->phase!=0;++tick)scene.tick();
            require(scene.player_shadow()->phase==0&&scene.player_shadow()->projection_factor==1&&!stun.visible);
            scene.sound_cue={};gate=5;scene.tick();
            require(sounds==2&&writes==3&&scene.player_shadow()->phase==1&&okage::event_variable(vars,0x40002)==1);
            // Attached map jobs share the ordinary two-pass removal lifecycle.
            const auto removed_handle=attached_handle;
            scene.map_effects()[removed_handle]->request_removal();scene.tick();
            require(scene.map_effects()[removed_handle]->flags&0x200);
            for(const auto& submission:scene.effect_submissions())require(submission.handle!=removed_handle);
            scene.tick();require(!scene.map_effects()[removed_handle]);
        }
        prepared.reset();require(retained->size()==240); // Queued/retained geometry remains alive.
    }
    for(unsigned enemy_index:{0u,1u}) {
        okage::ModelFile enemy_model,player_model,ordinary_model;
        enemy_model.models.emplace_back();player_model.models.emplace_back();ordinary_model.models.emplace_back();
        okage::FieldActor enemy_actor(std::move(enemy_model),idle,{}),enemy_player(std::move(player_model),idle,{});
        okage::FieldActor ordinary_actor(std::move(ordinary_model),idle,{});
        std::vector<okage::ActorBinding> enemy_bindings;
        okage::FieldDefinition enemy_room;enemy_room.type=1;
        if(enemy_index) {
            enemy_bindings.push_back({-8,&ordinary_actor});
            enemy_room.npcs.push_back({-8,"npc",0,0,0,{-100,0,-100,1},0,1});
        }
        const int enemy_alias=-8-int(enemy_index);
        enemy_bindings.push_back({enemy_alias,&enemy_actor});enemy_bindings.push_back({-1,&enemy_player});
        enemy_room.npcs.push_back({enemy_alias,"bkumo",5,0,0,{100,0,100,1},0,1});
        okage::OpeningField enemy_field(idle,[](int){},[](int){return true;},enemy_bindings,false);
        std::array<std::uint8_t,0x1000> enemy_variables{};okage::set_event_variable(enemy_variables,6,4,1);
        enemy_field.get_variable=[&](auto selector){return okage::event_variable(enemy_variables,selector);};
        enemy_field.set_variable=[&](auto index,auto bank,auto value){okage::set_event_variable(enemy_variables,index,bank,value);};
        unsigned random_calls=0;enemy_field.random_value=[&]{++random_calls;return 0;};
        enemy_field.enter(enemy_room,stage,empty_map,0);require(random_calls==1+enemy_index);
        for(unsigned i=0;i<239;++i)enemy_field.tick(); // Entry already ran one callback.
        require(random_calls==1+enemy_index&&!enemy_actor.visible);
        require((enemy_field.collision_bodies()[enemy_index].flags&2)==(enemy_index?2u:0u));
        enemy_field.tick();require(random_calls==2+enemy_index&&!enemy_actor.visible);
        okage::set_event_variable(enemy_variables,6,4,0);
        enemy_field.tick({0x4000});require(random_calls==3+enemy_index&&!enemy_actor.visible);
        enemy_field.tick({0x4000});require(random_calls==5+enemy_index&&enemy_actor.visible&&enemy_actor.encounter_model);
        require(!(enemy_field.collision_bodies()[enemy_index].flags&2)&&enemy_actor.render_flags==0x100000);
        require(enemy_actor.animation().values()[3]==okage::Float4({.5f,.5f,.5f,0}));
    }
    // Field effect tint uses owned STG light0/base/ambient colors, preserving
    // record order and the combined base+quarter-ambient opcode45.
    std::vector<std::uint8_t> lighting(32+3*20);
    auto light_word=[&](unsigned at,std::uint32_t v){for(unsigned j=0;j<4;++j)lighting.at(at+j)=v>>(j*8);};
    light_word(0,0x475453);light_word(8,32);light_word(12,3);
    for(unsigned i=0;i<3;++i){light_word(32+i*20,i==0?1:i==1?0x40:0x41);
        for(unsigned c=0;c<4;++c)light_word(36+i*20+c*4,__builtin_bit_cast(std::uint32_t, (c==3?1.f:float(c+1))));}
    auto tint=okage::field_effect_tint(lighting);require(tint[0]<tint[1]&&tint[1]<tint[2]&&tint[2]==1&&tint[3]==1);
    light_word(12,2);rejects([&]{okage::field_effect_tint(lighting);});
    light_word(52,0x45);tint=okage::field_effect_tint(lighting);require(tint[2]==1);
    light_word(36,0x7fc00000);rejects([&]{okage::field_effect_tint(lighting);});
    // Independent ordered light setters: direction W is forced0, opcode45
    // sets base plus quarter ambient RGB, and later setters act independently.
    light_word(36,__builtin_bit_cast(std::uint32_t, (1.f)));
    light_word(12,0);auto ordinary=okage::field_ordinary_lighting(lighting);
    require(ordinary.directions[0]==okage::Float4({0,0,1,0}));
    require(ordinary.colors[3]==okage::Float4({.25f,.25f,.25f,1}));
    require(ordinary.base==okage::Float4({1,1,1,1}));
    light_word(12,2);light_word(32,0x11);light_word(52,0x45);
    ordinary=okage::field_ordinary_lighting(lighting);
    require(ordinary.directions[0]==okage::Float4({1,2,3,0}));
    require(ordinary.colors[3]==okage::Float4({.25f,.5f,.75f,1}));
    require(ordinary.base==okage::Float4({1,2,3,1}));
    light_word(12,3);ordinary=okage::field_ordinary_lighting(lighting);
    require(ordinary.colors[3]==okage::Float4({1,2,3,1}));
    require(ordinary.base==okage::Float4({1,2,3,1}));
    light_word(72,0x40);light_word(76,__builtin_bit_cast(unsigned, (.5f)));light_word(88,__builtin_bit_cast(unsigned, (.75f)));
    ordinary=okage::field_ordinary_lighting(lighting);
    require(ordinary.base==okage::Float4({.5f,2,3,.75f}));
    require(ordinary.colors[3]==okage::Float4({.25f,.5f,.75f,1}));
    light_word(52,0x40);light_word(72,0x45);ordinary=okage::field_ordinary_lighting(lighting);
    require(ordinary.base==okage::Float4({.5f,2,3,.75f}));
    require(ordinary.colors[3]==okage::Float4({.125f,.5f,.75f,.75f}));
    light_word(12,4);rejects([&]{okage::field_ordinary_lighting(lighting);});
    light_word(12,3);light_word(36,0x7fc00000);rejects([&]{okage::field_ordinary_lighting(lighting);});

    std::cout<<"Camera, field lifecycle, map motion, event gates and encounter spawning passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
