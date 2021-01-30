#import <AppKit/AppKit.h>
#include "okage/dialogue.hpp"
#include "okage/event.hpp"
#include "okage/field.hpp"
#include "okage/music.hpp"
#include "okage/audio_output.hpp"
#include "okage/stream_bank.hpp"
#include "okage/scene_renderer.hpp"
#include <bit>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>

namespace fs=std::filesystem;
static std::vector<std::uint8_t> readData(const fs::path& path) {
    const auto n=fs::file_size(path);if(n>128*1024*1024)throw std::runtime_error("Runtime input limit");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read runtime input");return b;
}
struct DirectionReplay {
    std::vector<std::pair<unsigned,std::uint16_t>> entries;
    void load(const fs::path& path){
        std::ifstream in(path);if(!in)throw std::runtime_error("Cannot read direction replay");
        std::string line;unsigned previous=0;
        while(std::getline(in,line)){
            if(line.empty() || line[0]=='#')continue;
            std::istringstream row(line);unsigned frame,mask;std::string extra;
            if(!(row>>frame>>std::hex>>mask) || (row>>extra) || frame<=previous || frame>36000 || (mask&~0x5000u))
                throw std::runtime_error("Direction replay requires increasing FRAME and hexadecimal up/down mask");
            entries.push_back({frame,std::uint16_t(mask)});previous=frame;
        }
    }
    std::uint16_t held(unsigned frame) const {
        const auto found=std::upper_bound(entries.begin(),entries.end(),frame,[](unsigned f,const auto& entry){return f<entry.first;});
        return found==entries.begin()?0:std::prev(found)->second;
    }
};
struct Session {
    std::array<okage::Font,3> fonts;
    okage::MessageBank messages;
    okage::DialogueText text;
    okage::TextClock clock;
    okage::DialogueController controller;
    okage::DialoguePadRepeat pad_repeat;
    okage::DialogueOpacity opacity;
    okage::DialogueClosure closure;
    okage::SceneFade scene_fade;
    okage::Narration narration;
    okage::ChoiceMenu choices;
    std::vector<std::uint32_t> choice_trace;
    std::array<std::uint8_t,0x1000> variables{};
    okage::DialogueWaitState wait{5,1};
    unsigned frame=0,release_frame=0,close_frame=0,close_release_frame=0;
    std::unique_ptr<okage::DryMusicPlayer> music;
    std::unique_ptr<okage::AudioOutput> audio_output;
    okage::MusicVolumeRamp music_volume;
    std::vector<std::int16_t> audio_capture;
    std::vector<std::uint32_t> music_trace;
    bool record_audio=false;
    fs::path sound_directory;
    std::map<unsigned,std::unique_ptr<okage::StreamPlayer>> sound_banks;
    std::vector<std::uint32_t> sound_trace;
    void load_music(const fs::path& sequence,const fs::path& bank,bool record,const fs::path& sounds){
        music=std::make_unique<okage::DryMusicPlayer>(readData(sequence),okage::read_music_bank(readData(bank)));
        record_audio=record;sound_directory=sounds;
        if(field)field->audio_service=[this](unsigned operation,int argument){
            if(operation==1 && !sound_directory.empty()){
                if(argument<0 || argument>9999)throw std::runtime_error("Sound set number outside import scope");
                wait.mode=15;wait.active=0;
                const auto path=sound_directory/("SET"+std::to_string(argument)+".XWS");
                auto bank=okage::read_stream_bank(readData(path));
                const auto count=bank.records.size();sound_banks.insert_or_assign(1,std::make_unique<okage::StreamPlayer>(std::move(bank)));
                // Native disk loading is synchronous. The actual owned bank must
                // be registered before the subsequent -1 completion wait can pass.
                wait.mode=5;wait.active=1;
                std::cout<<"Native sound set "<<argument<<": "<<count<<" RXWS records loaded\n";return true;
            }
            if(operation==2){sound_banks.erase(1);return true;}
            if((operation!=4 && operation!=5) || argument<0)return false;
            music_volume.request(operation==4 ? 128 : 80,unsigned(argument));return true;
        };
        if(field)field->sound_cue=[this](unsigned bank,int index,float speed,unsigned volume){
            auto found=sound_banks.find(bank);if(found==sound_banks.end())return false;
            found->second->play(index,speed,volume);
            sound_trace.insert(sound_trace.end(),{frame,bank,unsigned(index),__builtin_bit_cast(unsigned, (speed)),volume});return true;
        };
        std::cout<<"Native dry music enabled: original sequence/bank, reverb and SFX pending\n";
    }

    bool dialogue_active=true;
    std::vector<std::unique_ptr<okage::FieldActor>> actor_objects;
    std::vector<okage::ActorBinding> actor_bindings;
    std::unique_ptr<okage::OpeningField> field;
    std::unique_ptr<okage::SceneRenderer> renderer;
    okage::Float4 projection{};
    std::vector<std::uint32_t> scene_trace,dialogue_trace;
    std::vector<std::uint32_t> effect_trace;
    bool effects_loaded=false;
    std::map<int,std::vector<std::uint32_t>> actor_traces;
    int message_id=10100;unsigned messages_started=0;
    std::uint64_t service_request_count=0;
    Session(const fs::path& directory,const fs::path& pme,const std::string& player_name):
        fonts{okage::read_fnt(readData(directory/"IQ24.FNT")),okage::read_fnt(readData(directory/"BMUI.FNT")),okage::read_fnt(readData(directory/"BM.FNT"))},
        messages(okage::read_pme(readData(pme))),text({}) {
        if(player_name.empty() || player_name.size()>12 || std::any_of(player_name.begin(),player_name.end(),[](unsigned char c){return c<32 || c>=127 || c=='\\';}))
            throw std::runtime_error("Diagnostic player name requires 1..12 printable ASCII characters without text escapes");
        if(!messages.bind("############",player_name))throw std::runtime_error("Player-name tag missing from FIELD.PME");
        text=okage::DialogueText(messages,10100);
        text.fonts=fonts;text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        clock.glyph_flags=0x10;opacity.set(1,8);
        scene_fade.current=scene_fade.target=1; // Already lit diagnostic entry.
    }
    void load_scene(const fs::path& map,const fs::path& event,const std::vector<std::pair<int,fs::path>>& assets) {
        std::vector<okage::SceneActor> scene_actors;
        for(const auto& [alias,path]:assets) {
            auto motion=path,xta=path;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            const auto data=readData(xta);
            auto object=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(path)),readData(motion),
                okage::read_xta(std::string_view(reinterpret_cast<const char*>(data.data()),data.size())));
            actor_bindings.push_back({alias,object.get()});scene_actors.push_back({object.get(),path});
            actor_objects.push_back(std::move(object));
        }
        renderer=std::make_unique<okage::SceneRenderer>(map,scene_actors);
        auto stg=map;stg.replace_extension(".stg");projection=okage::field_projection(readData(stg));
        dialogue_active=false;
        field=std::make_unique<okage::OpeningField>(readData(event),[this](int id){
            if(dialogue_active && !closure.finished())throw std::runtime_error("Field replaces an unfinished message");
            if(id<10100 || id>10107)throw std::runtime_error("Field message initialization outside recovered scope");
            message_id=id;++messages_started;
            text=okage::DialogueText(messages,id);text.fonts=fonts;
            text.state.flags=0x8041;text.state.font=2;text.state.period=1;
            clock={};clock.glyph_flags=0x10;controller={};closure={};opacity.set(1,8);
            controller.set_variable=[this](unsigned index,unsigned bank,std::int32_t value){okage::set_event_variable(variables,index,bank,value);};
            wait.countdown=wait.observed=wait.accumulated=0;
            dialogue_active=true;
        },[this](int request){
            if(!dialogue_active && request!=-1)throw std::runtime_error("Field wait before dialogue initialization");
            const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
            const bool released=okage::poll_dialogue_wait(wait,request,
                {true,__builtin_bit_cast(std::int32_t, (closure.message_flags)),layers,std::int32_t(text.state.progress)})==0;
            if(released && !release_frame)release_frame=frame;
            if(released && request==0 && !close_release_frame)close_release_frame=frame;
            return released;
        },actor_bindings);
        std::cout<<"Native room: "<<renderer->placements()<<" placements; "<<renderer->omitted_effects()
            <<" effects omitted; "<<renderer->actors()<<" native actors; shadows unavailable; diagnostic dialogue panel\n";
    }
    void load_effects(const fs::path& data,const fs::path& textures) {
        if(!field||!renderer)throw std::runtime_error("Effects require a loaded scene");
        renderer->load_effect_textures(textures);field->load_effects(readData(data));effects_loaded=true;
    }
    void tick(okage::DialogueButtons input) {
        ++frame;
        input.repeat=pad_repeat.update(input.held,input.pressed);
        scene_fade.tick();
        const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
        if(field)field->tick();
        if(music){
            music_volume.tick();music->volume(music_volume.level(),music_volume.level());
            std::array<std::int16_t,1600> pcm{};music->render(pcm);
            std::array<std::int32_t,1600> stream{};
            for(auto& [bank,player]:sound_banks)player->mix(stream);
            for(unsigned i=0;i<pcm.size();++i)pcm[i]=std::clamp(int(pcm[i])+stream[i],-32768,32767);
            if(audio_output)audio_output->submit(pcm);
            if(record_audio){audio_capture.insert(audio_capture.end(),pcm.begin(),pcm.end());
                music_trace.insert(music_trace.end(),{frame,unsigned(music_volume.current),unsigned(music_volume.target),unsigned(music_volume.step),music->peak_voices()});}
        }
        if(!field && !release_frame && okage::poll_dialogue_wait(wait,2,{true,0,layers,std::int32_t(text.state.progress)})==0)release_frame=frame;
        if(dialogue_active && !closure.finished()) {
            opacity.tick();closure.decoration.tick();text.frame(clock,opacity.visible());
        }
        if(dialogue_active && !closure.finished() && controller.stage>=6)closure.update(controller.stage);
        else if(dialogue_active && controller.stage==5) {
            const auto effects=controller.update(text,input);
            if(effects.accepted_choice>=0)choices.accepted=effects.accepted_choice;
            if(effects.reset_text_clock){clock.revealed=0;clock.scroll=0;clock.scroll_target=0;clock.scroll_step=clock.scroll_base_step;}
            if(effects.scene_fade)scene_fade.request(*effects.scene_fade);
            if(effects.close)opacity.set(128,-4);
            if(effects.cue>=0 || effects.stop_voice || effects.stop_effect)++service_request_count;
        }
        if(closure.finished() && !close_frame)close_frame=frame;
        choices.update(dialogue_active && !closure.finished() ? text.state.replacement : "",text.choice_cursor,controller.choice_armed);
        choice_trace.insert(choice_trace.end(),{frame,unsigned(message_id),unsigned(text.position().fragment),unsigned(text.position().cursor),
            unsigned(text.choice_cursor),unsigned(text.choice_result),controller.choice_active?1u:0u,controller.choice_armed?1u:0u,
            input.held,input.pressed,input.repeat,choices.alpha});
        narration.update(dialogue_active && !closure.finished() ? text.state.description : "",fonts[2]);
        dialogue_trace.insert(dialogue_trace.end(),{frame,std::uint32_t(message_id),text.state.flags,text.state.phase,
            text.state.period,text.state.delay,text.state.progress,controller.advance_count,controller.layer_advance,
            controller.layer_flags,controller.stage,__builtin_bit_cast(std::uint32_t, (opacity.delta))});
        if(field) {
            scene_trace.insert(scene_trace.end(),{frame,field->pc(),field->pending_opcode,release_frame});
            for(const auto& values:{field->position,field->target,field->rotation})
                for(float v:values)scene_trace.push_back(__builtin_bit_cast(std::uint32_t, (v)));
            if(effects_loaded) {
                const auto& all=field->effects();unsigned count=0;for(const auto& e:all)if(e)++count;
                effect_trace.insert(effect_trace.end(),{frame,field->pc(),count});
                for(unsigned i=0;i<all.size();++i)if(const auto& e=all[i]) {
                    effect_trace.insert(effect_trace.end(),{i,unsigned(e->actor_alias),e->animation.phase,e->animation.flags});
                    for(float f:e->position)effect_trace.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                    for(const auto& s:e->animation.sprites) {
                        for(const auto& v:{s.position,s.scale})for(float f:v)effect_trace.push_back(__builtin_bit_cast(std::uint32_t, (f)));
                        effect_trace.push_back(s.texture);effect_trace.push_back(s.uv[0]|(std::uint32_t(s.uv[1])<<16));effect_trace.push_back(s.uv[2]|(std::uint32_t(s.uv[3])<<16));
                    }
                }
            }
        }
        for(const auto& binding:actor_bindings) {
            const auto* actor=binding.actor;auto& actor_trace=actor_traces[binding.alias];
            actor_trace.insert(actor_trace.end(),{frame,field->pc(),actor->animation().pc(),actor->animation().script_state(),
                actor->motion(),std::uint32_t(actor->channels().size()),std::uint32_t(actor->textures().images().size()),actor->visible ? 1u : 0u});
            for(const auto& channel:actor->channels())for(float v:channel)actor_trace.push_back(__builtin_bit_cast(std::uint32_t, (v)));
            for(auto image:actor->textures().images())actor_trace.push_back(__builtin_bit_cast(std::uint32_t, (image)));
        }
    }
    okage::Texture surface() const {
        auto text_image=okage::rasterize_text(fonts,text.glyphs,640,224);
        if(!renderer)return text_image;
        const auto& r=field->rotation;
        auto image=renderer->render(field->view(),projection,effects_loaded?&field->effects():nullptr,{-r[0],-r[1],-r[2],0});
        const auto brightness=128-scene_fade.alpha();
        for(std::size_t i=0;i<image.rgba.size();i+=4)for(unsigned c=0;c<3;++c)image.rgba[i+c]=unsigned(image.rgba[i+c])*brightness/128;
        if(choices.highlighted){
            const int top=(336+27*choices.cursor)/2,bottom=(363+27*choices.cursor)/2;
            for(int y=top;y<bottom;++y)for(int x=162;x<599;++x)for(unsigned c=0;c<3;++c){
                auto& pixel=image.rgba[(y*640+x)*4+c];pixel=(255u*48+unsigned(pixel)*80)/128;
            }
        }
        auto glyphs=narration.glyphs(fonts[2]);const auto choiceGlyphs=choices.glyphs(fonts[2]);glyphs.insert(glyphs.end(),choiceGlyphs.begin(),choiceGlyphs.end());
        const auto overlay=okage::rasterize_text(fonts,glyphs,640,224);
        for(std::size_t i=0;i<overlay.rgba.size();i+=4){
            const unsigned alpha=overlay.rgba[i+3];
            for(unsigned c=0;c<3;++c)image.rgba[i+c]=(unsigned(overlay.rgba[i+c])*alpha+unsigned(image.rgba[i+c])*(255-alpha)+127)/255;
        }
        image.height=448;image.rgba.resize(640*448*4);
        for(std::size_t i=0;i<text_image.rgba.size();i+=4) {
            const float a=dialogue_active && controller.window_type!=2 ? (text_image.rgba[i+3]/255.f)*std::clamp(opacity.value/128.f,0.f,1.f) : 0;
            for(unsigned c=0;c<3;++c)image.rgba[640*224*4+i+c]=std::uint8_t(text_image.rgba[i+c]*a+16.f*(1-a));
            image.rgba[640*224*4+i+3]=255;
        }
        return image;
    }
};
static NSBitmapImageRep* bitmap(const okage::Texture& image) {
    NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image.width pixelsHigh:image.height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:image.width*4 bitsPerPixel:32];
    if(!rep)throw std::runtime_error("Cannot allocate native text bitmap");
    std::memcpy(rep.bitmapData,image.rgba.data(),image.rgba.size());return rep;
}
static void capture(const Session& session,const fs::path& path) {
    auto statePath=fs::path(path.string()+".json");
    if(fs::exists(path)||fs::is_symlink(path)||fs::exists(statePath)||fs::is_symlink(statePath))throw std::runtime_error("Refusing existing native capture");
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());
    const auto data=[bitmap(session.surface()) representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    std::ofstream out(path,std::ios::binary);if(!data || !out.write(static_cast<const char*>(data.bytes),data.length))throw std::runtime_error("Cannot write native capture");
    std::ofstream state(statePath);
    if(session.effects_loaded) {
        std::ofstream effects(path.string()+".effects.bin",std::ios::binary);
        if(!effects.write(reinterpret_cast<const char*>(session.effect_trace.data()),session.effect_trace.size()*4))throw std::runtime_error("Cannot write effect trace");
    }
    state<<"{\n  \"frames\": "<<session.frame<<",\n  \"message_id\": "<<session.message_id
         <<",\n  \"messages_started\": "<<session.messages_started<<",\n  \"text_progress\": "<<session.text.state.progress
         <<",\n  \"advance_count\": "<<session.controller.advance_count<<",\n  \"wait_release_frame\": "<<session.release_frame
         <<",\n  \"controller_stage\": "<<session.controller.stage<<",\n  \"unimplemented_service_requests\": "<<session.service_request_count
         <<",\n  \"message_flags\": "<<session.closure.message_flags
         <<",\n  \"window_type\": "<<session.controller.window_type
         <<",\n  \"scene_brightness\": "<<session.scene_fade.current
         <<",\n  \"narration_cues\": "<<session.sound_trace.size()/5
         <<",\n  \"progression_flag_2\": "<<okage::event_variable(session.variables,2)
         <<",\n  \"progression_flag_3\": "<<okage::event_variable(session.variables,3)
         <<",\n  \"loaded_stream_banks\": "<<session.sound_banks.size()
         <<",\n  \"choice_cursor\": "<<session.text.choice_cursor
         <<",\n  \"choice_result\": "<<session.text.choice_result
         <<",\n  \"choice_armed\": "<<(session.controller.choice_armed ? "true" : "false")
         <<",\n  \"close_frame\": "<<session.close_frame<<",\n  \"close_wait_release_frame\": "<<session.close_release_frame<<"\n}\n";
    if(!state)throw std::runtime_error("Cannot write native capture state");
    if(session.music){
        const auto pcmPath=path.string()+".audio.pcm",tracePath=path.string()+".music.bin",statsPath=path.string()+".audio.json",soundPath=path.string()+".sound.bin";
        for(const auto& p:{pcmPath,tracePath,statsPath,soundPath})if(fs::exists(p)||fs::is_symlink(p))throw std::runtime_error("Refusing existing audio capture");
        std::ofstream pcm(pcmPath,std::ios::binary),trace(tracePath,std::ios::binary),stats(statsPath),sound(soundPath,std::ios::binary);
        pcm.write(reinterpret_cast<const char*>(session.audio_capture.data()),session.audio_capture.size()*2);
        trace.write(reinterpret_cast<const char*>(session.music_trace.data()),session.music_trace.size()*4);
        sound.write(reinterpret_cast<const char*>(session.sound_trace.data()),session.sound_trace.size()*4);
        stats<<"{\n  \"dry_output\": true,\n  \"frames\": "<<session.audio_capture.size()/2<<",\n  \"rate\": 48000,\n  \"volume\": "<<session.music_volume.level()
             <<",\n  \"peak_voices\": "<<session.music->peak_voices()<<",\n  \"device_frames_enqueued\": "<<(session.audio_output ? session.audio_output->delivered_frames() : 0)
             <<",\n  \"device_underrun_frames\": "<<(session.audio_output ? session.audio_output->underrun_frames() : 0)<<"\n}\n";
        if(!pcm || !trace || !stats || !sound)throw std::runtime_error("Cannot write native audio capture");
    }

    const auto choicePath=fs::path(path.string()+".choices.bin");
    if(fs::exists(choicePath)||fs::is_symlink(choicePath))throw std::runtime_error("Refusing existing choice trace");
    std::ofstream choiceOut(choicePath,std::ios::binary);
    if(!choiceOut.write(reinterpret_cast<const char*>(session.choice_trace.data()),session.choice_trace.size()*4))throw std::runtime_error("Cannot save choice trace");
    const auto dialoguePath=fs::path(path.string()+".dialogue.bin");
    if(fs::exists(dialoguePath)||fs::is_symlink(dialoguePath))throw std::runtime_error("Refusing existing dialogue trace");
    std::ofstream dialogueOut(dialoguePath,std::ios::binary);
    if(!dialogueOut.write(reinterpret_cast<const char*>(session.dialogue_trace.data()),session.dialogue_trace.size()*4))throw std::runtime_error("Cannot save dialogue trace");
    for(const auto& [alias,trace]:session.actor_traces) {
        const auto actorPath=fs::path(path.string()+(alias==-8 ? ".actor" : ".actor"+std::to_string(alias))+".bin");
        if(fs::exists(actorPath)||fs::is_symlink(actorPath))throw std::runtime_error("Refusing existing actor trace");
        std::ofstream out(actorPath,std::ios::binary);
        if(!out.write(reinterpret_cast<const char*>(trace.data()),trace.size()*4))throw std::runtime_error("Cannot save actor trace");
    }
    if(session.field) {
        const auto tracePath=fs::path(path.string()+".camera.bin");
        if(fs::exists(tracePath)||fs::is_symlink(tracePath))throw std::runtime_error("Refusing existing camera trace");
        std::ofstream trace(tracePath,std::ios::binary);
        if(!trace.write(reinterpret_cast<const char*>(session.scene_trace.data()),session.scene_trace.size()*4))throw std::runtime_error("Cannot save native camera trace");
        std::cout<<"Field PC=0x"<<std::hex<<session.field->pc()<<" pending opcode=0x"<<session.field->pending_opcode<<std::dec
            <<" actor="<<session.field->pending_actor<<" animation="<<session.field->pending_animation<<'\n';
    }
}

@interface NativeTextView : NSView
@property(strong) NSImage* image;
@property float opacity;
@property unsigned short held;
@property unsigned short pressed;
@property unsigned injectedKeyDowns;
@end
@implementation NativeTextView
- (BOOL)acceptsFirstResponder {return YES;}
- (BOOL)isFlipped {return YES;}
- (void)keyDown:(NSEvent*)event {
    if(event.isARepeat)return;
    const unsigned mask=(event.keyCode==36 || event.keyCode==49)?0x40:event.keyCode==126?0x1000:event.keyCode==125?0x4000:0;
    if(mask){self.pressed|=mask;self.held|=mask;++self.injectedKeyDowns;}
    else [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    const unsigned mask=(event.keyCode==36 || event.keyCode==49)?0x40:event.keyCode==126?0x1000:event.keyCode==125?0x4000:0;
    if(mask)self.held&=~mask;
    else [super keyUp:event];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;[[NSColor colorWithCalibratedRed:.04 green:.06 blue:.09 alpha:1] setFill];NSRectFill(self.bounds);
    [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
    [self.image drawInRect:self.bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
        fraction:std::clamp(self.opacity/128.f,0.f,1.f) respectFlipped:YES hints:nil];
}
@end
@interface NativeApp : NSObject<NSApplicationDelegate,NSWindowDelegate> {
@public std::unique_ptr<Session> session;
@public unsigned testFrames;
@public int failureStatus;
@public std::vector<unsigned> testPresses;
@public DirectionReplay directions;
@public std::uint16_t replayDirections;
@public fs::path testCapture;
}
@property(strong) NSWindow* window;
@property(strong) NativeTextView* view;
@property(strong) NSTimer* timer;
@property double previousTime;
@property double accumulator;
@end
@implementation NativeApp
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,session->field ? 672 : 336)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    self.window.delegate=self;
    if(session->music)session->audio_output=std::make_unique<okage::AudioOutput>();
    self.view=[[NativeTextView alloc] initWithFrame:self.window.contentView.bounds];self.view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    self.window.contentView=self.view;[self.window center];[self.window makeKeyAndOrderFront:nil];[self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];self.previousTime=NSProcessInfo.processInfo.systemUptime;
    self.timer=[NSTimer scheduledTimerWithTimeInterval:1./120 target:self selector:@selector(update:) userInfo:nil repeats:YES];
}
- (void)update:(NSTimer*)timer {
    (void)timer;
    try {
        const double now=NSProcessInfo.processInfo.systemUptime;
        self.accumulator+=std::min(.25,now-self.previousTime);self.previousTime=now;
        bool changed=false;
        while(self.accumulator>=1./60) {
            if(testFrames){
                const auto next=directions.held(session->frame+1);
                for(const auto [mask,code]:{std::pair<unsigned,unsigned>{0x1000,126},{0x4000,125}}){
                    if((next^replayDirections)&mask){
                        const auto type=(next&mask)?NSEventTypeKeyDown:NSEventTypeKeyUp;
                        NSString* chars=[NSString stringWithFormat:@"%C",unichar(code==126?NSUpArrowFunctionKey:NSDownArrowFunctionKey)];
                        NSEvent* event=[NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:now
                            windowNumber:self.window.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
                        [self.window sendEvent:event];
                    }
                }
                replayDirections=next;
            }
            const bool press=testFrames && std::find(testPresses.begin(),testPresses.end(),session->frame+1)!=testPresses.end();
            if(press) {
                NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:now
                    windowNumber:self.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
                [self.window sendEvent:event];
            }
            session->tick({self.view.held,self.view.pressed});self.view.pressed=0;
            if(press) {
                NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:now
                    windowNumber:self.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
                [self.window sendEvent:event];
            }
            self.accumulator-=1./60;changed=true;
            if(testFrames && session->frame==testFrames)break;
        }
        if(!changed)return;
        NSImage* image=[[NSImage alloc] initWithSize:NSMakeSize(640,session->field ? 448 : 224)];[image addRepresentation:bitmap(session->surface())];
        self.view.image=image;self.view.opacity=session->field ? 128 : session->opacity.value;self.view.needsDisplay=YES;
        self.window.title=[NSString stringWithFormat:@"Okage native dialogue — ↑/↓, Return/Space — progress %u — event %@",
            session->text.state.progress,session->release_frame ? @"released" : @"waiting"];
        if(session->field && session->field->pending_opcode)
            self.window.title=[self.window.title stringByAppendingString:@" — service pending (partial scene)"];
        if(testFrames && session->frame==testFrames) {
            [self.timer invalidate];capture(*session,testCapture);
            [self.view display];
            NSBitmapImageRep* rep=[self.view bitmapImageRepForCachingDisplayInRect:self.view.bounds];
            [self.view cacheDisplayInRect:self.view.bounds toBitmapImageRep:rep];
            NSData* png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            const auto path=testCapture.string()+".view.png";
            if(fs::exists(path)||fs::is_symlink(path))throw std::runtime_error("Refusing existing native view capture");
            std::ofstream out(path,std::ios::binary);
            if(!png || !out.write(static_cast<const char*>(png.bytes),png.length))throw std::runtime_error("Cannot save native view");
            std::cout<<"AppKit keyDown events="<<self.view.injectedKeyDowns<<" native progress="<<session->text.state.progress<<" wait_release="<<session->release_frame<<std::endl;
            [NSApp terminate:nil];
        }
    }catch(const std::exception& e) {
        [self.timer invalidate];self.window.title=[NSString stringWithUTF8String:e.what()];std::cerr<<e.what()<<'\n';
        if(testFrames){failureStatus=1;[NSApp stop:nil];}
    }
}
- (void)windowDidResignKey:(NSNotification*)notification {(void)notification;self.view.held=0;self.view.pressed=0;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {(void)app;return YES;}
@end
int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc<3)throw std::runtime_error("Usage: okage-native FONT_DIRECTORY FIELD.PME [--frames N | --ui-test-frames N] [--press FRAME,FRAME --directions INPUT --capture FILE.png] [--scene MAP.xmp --event EVENT.xmt --actor ACTOR.xmd --actor-bind ALIAS=MODEL.xmd --player-name NAME] [--music-dry SEQUENCE.xsq --music-bank BANK.xv --sound-dir SOUND_DIRECTORY] [--effect-data TES.EFD --effect-textures ENV_TEX_DIRECTORY]");
        unsigned frames=0,uiFrames=0;DirectionReplay directions;std::vector<unsigned> presses;fs::path output,map,event,musicSequence,musicBank,soundDirectory,effectData,effectTextures;std::vector<std::pair<int,fs::path>> actors;std::string playerName="Ari";
        for(int i=3;i<argc;++i) {
            const std::string arg=argv[i];if(++i==argc)throw std::runtime_error("Missing native runtime option value");
            if(arg=="--frames")frames=std::stoul(argv[i]);
            else if(arg=="--ui-test-frames")uiFrames=std::stoul(argv[i]);
            else if(arg=="--directions")directions.load(argv[i]);
            else if(arg=="--capture")output=argv[i];
            else if(arg=="--scene")map=argv[i];
            else if(arg=="--event")event=argv[i];
            else if(arg=="--music-dry")musicSequence=argv[i];
            else if(arg=="--music-bank")musicBank=argv[i];
            else if(arg=="--sound-dir")soundDirectory=argv[i];
            else if(arg=="--effect-data")effectData=argv[i];
            else if(arg=="--effect-textures")effectTextures=argv[i];
            else if(arg=="--actor")actors.push_back({-8,argv[i]});
            else if(arg=="--actor-bind") {
                const std::string value=argv[i];const auto eq=value.find('=');std::size_t consumed=0;
                if(eq==value.npos || eq+1==value.size())throw std::runtime_error("Actor binding requires ALIAS=MODEL.xmd");
                const auto alias=std::stoi(value.substr(0,eq),&consumed);
                if(consumed!=eq)throw std::runtime_error("Invalid actor alias");
                actors.push_back({alias,value.substr(eq+1)});
            }
            else if(arg=="--player-name")playerName=argv[i];
            else if(arg=="--press") {std::istringstream in(argv[i]);std::string n;while(std::getline(in,n,','))presses.push_back(std::stoul(n));}
            else throw std::runtime_error("Unknown native runtime option");
        }
        if(musicSequence.empty()!=musicBank.empty())throw std::runtime_error("Music requires both sequence and bank");
        if(!soundDirectory.empty() && musicSequence.empty())throw std::runtime_error("Sound directory requires native music setup");
        if(effectData.empty()!=effectTextures.empty()||(!effectData.empty()&&map.empty()))throw std::runtime_error("Effects require data, texture directory and scene");
        if(map.empty()!=event.empty() || (!actors.empty() && map.empty()))throw std::runtime_error("Scene requires both map and event assets");
        if(frames>36000 || uiFrames>36000 || (frames && uiFrames) || (uiFrames && output.empty()))throw std::runtime_error("Native diagnostic frame/capture bounds");
        if(frames) {
            Session session(argv[1],argv[2],playerName);if(!map.empty())session.load_scene(map,event,actors);
            if(!musicSequence.empty())session.load_music(musicSequence,musicBank,!output.empty(),soundDirectory);
            if(!effectData.empty())session.load_effects(effectData,effectTextures);
            std::uint16_t previous=0;
            for(unsigned i=1;i<=frames;++i) {
                const std::uint16_t mask=std::find(presses.begin(),presses.end(),i)!=presses.end()?0x40:0;
                const auto held=directions.held(i);session.tick({std::uint16_t(mask|held),std::uint16_t(mask|(held&~previous))});previous=held;
            }
            if(!output.empty())capture(session,output);
            std::cout<<"Native frames="<<session.frame<<" progress="<<session.text.state.progress<<" wait_release="<<session.release_frame<<'\n';return 0;
        }
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NativeApp* app=[NativeApp new];app->session=std::make_unique<Session>(argv[1],argv[2],playerName);if(!map.empty())app->session->load_scene(map,event,actors);
        if(!musicSequence.empty())app->session->load_music(musicSequence,musicBank,!output.empty(),soundDirectory);
        if(!effectData.empty())app->session->load_effects(effectData,effectTextures);
        app->testFrames=uiFrames;app->testPresses=presses;app->directions=directions;app->testCapture=output;
        [NSApp setDelegate:app];[NSApp run];return app->failureStatus;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
