#import <AppKit/AppKit.h>
#include "okage/dialogue.hpp"
#include "okage/event.hpp"
#include "okage/field.hpp"
#include "okage/scene_renderer.hpp"
#include <bit>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

namespace fs=std::filesystem;
static std::vector<std::uint8_t> readData(const fs::path& path) {
    const auto n=fs::file_size(path);if(n>128*1024*1024)throw std::runtime_error("Runtime input limit");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size()))throw std::runtime_error("Cannot read runtime input");return b;
}
struct Session {
    std::array<okage::Font,3> fonts;
    okage::DialogueText text;
    okage::TextClock clock;
    okage::DialogueController controller;
    okage::DialogueOpacity opacity;
    okage::DialogueWaitState wait{5,1};
    unsigned frame=0,release_frame=0;
    bool dialogue_active=true;
    std::unique_ptr<okage::FieldActor> woman;
    std::unique_ptr<okage::OpeningField> field;
    std::unique_ptr<okage::SceneRenderer> renderer;
    okage::Float4 projection{};
    std::vector<std::uint32_t> scene_trace,actor_trace;
    std::uint64_t service_request_count=0;
    Session(const fs::path& directory,const fs::path& pme):
        fonts{okage::read_fnt(readData(directory/"IQ24.FNT")),okage::read_fnt(readData(directory/"BMUI.FNT")),okage::read_fnt(readData(directory/"BM.FNT"))},
        text(okage::read_pme(readData(pme)).message(10100)) {
        text.fonts=fonts;text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        clock.glyph_flags=0x10;opacity.set(1,8);
    }
    void load_scene(const fs::path& map,const fs::path& event,const fs::path& actor={}) {
        if(!actor.empty()) {
            auto motion=actor,xta=actor;motion.replace_extension(".xmt");xta.replace_extension(".xta");
            const auto text=readData(xta);
            woman=std::make_unique<okage::FieldActor>(okage::read_xmd(readData(actor)),readData(motion),
                okage::read_xta(std::string_view(reinterpret_cast<const char*>(text.data()),text.size())));
        }
        renderer=std::make_unique<okage::SceneRenderer>(map,woman.get(),actor.parent_path());
        auto stg=map;stg.replace_extension(".stg");projection=okage::field_projection(readData(stg));
        dialogue_active=false;
        field=std::make_unique<okage::OpeningField>(readData(event),[this](int id){
            if(id!=10100 || dialogue_active)throw std::runtime_error("Additional field dialogue not yet implemented");
            dialogue_active=true;
        },[this](int request){
            if(!dialogue_active)throw std::runtime_error("Field wait before dialogue initialization");
            const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
            const bool released=okage::poll_dialogue_wait(wait,request,{true,0,layers,std::int32_t(text.state.progress)})==0;
            if(released && !release_frame)release_frame=frame;return released;
        },woman.get());
        std::cout<<"Native room: "<<renderer->placements()<<" placements; "<<renderer->omitted_effects()
            <<" effects omitted; "<<renderer->actors()<<" native actors; audio/shadows unavailable; diagnostic dialogue panel\n";
    }
    void tick(okage::DialogueButtons input) {
        ++frame;
        const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
        if(field)field->tick();
        if(!field && !release_frame && okage::poll_dialogue_wait(wait,2,{true,0,layers,std::int32_t(text.state.progress)})==0)release_frame=frame;
        if(dialogue_active) {opacity.tick();text.frame(clock,opacity.visible());}
        if(dialogue_active && controller.stage==5) {
            const auto effects=controller.update(text,input);
            if(effects.close)opacity.set(128,-4);
            if(effects.cue>=0 || effects.stop_voice || effects.stop_effect)++service_request_count;
        }
        if(field) {
            scene_trace.insert(scene_trace.end(),{frame,field->pc(),field->pending_opcode,release_frame});
            for(const auto& values:{field->position,field->target,field->rotation})
                for(float v:values)scene_trace.push_back(__builtin_bit_cast(std::uint32_t, (v)));
        }
        if(woman) {
            actor_trace.insert(actor_trace.end(),{frame,field->pc(),woman->animation().pc(),woman->animation().script_state(),
                woman->motion(),std::uint32_t(woman->channels().size()),std::uint32_t(woman->textures().images().size()),0});
            for(const auto& channel:woman->channels())for(float v:channel)actor_trace.push_back(__builtin_bit_cast(std::uint32_t, (v)));
            for(auto image:woman->textures().images())actor_trace.push_back(__builtin_bit_cast(std::uint32_t, (image)));
        }
    }
    okage::Texture surface() const {
        auto text_image=okage::rasterize_text(fonts,text.glyphs,640,224);
        if(!renderer)return text_image;
        auto image=renderer->render(field->view(),projection);
        image.height=448;image.rgba.resize(640*448*4);
        for(std::size_t i=0;i<text_image.rgba.size();i+=4) {
            const float a=dialogue_active ? (text_image.rgba[i+3]/255.f)*std::clamp(opacity.value/128.f,0.f,1.f) : 0;
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
    state<<"{\n  \"frames\": "<<session.frame<<",\n  \"text_progress\": "<<session.text.state.progress
         <<",\n  \"advance_count\": "<<session.controller.advance_count<<",\n  \"wait_release_frame\": "<<session.release_frame
         <<",\n  \"controller_stage\": "<<session.controller.stage<<",\n  \"unimplemented_service_requests\": "<<session.service_request_count<<"\n}\n";
    if(!state)throw std::runtime_error("Cannot write native capture state");
    if(session.woman) {
        const auto actorPath=fs::path(path.string()+".actor.bin");
        if(fs::exists(actorPath)||fs::is_symlink(actorPath))throw std::runtime_error("Refusing existing actor trace");
        std::ofstream out(actorPath,std::ios::binary);
        if(!out.write(reinterpret_cast<const char*>(session.actor_trace.data()),session.actor_trace.size()*4))throw std::runtime_error("Cannot save actor trace");
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
    if(event.keyCode==36 || event.keyCode==49) {self.pressed|=0x40;self.held|=0x40;++self.injectedKeyDowns;}
    else [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    if(event.keyCode==36 || event.keyCode==49)self.held&=~0x40;
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
        self.window.title=[NSString stringWithFormat:@"Okage native dialogue — Return/Space — progress %u — event %@",
            session->text.state.progress,session->release_frame ? @"released" : @"waiting"];
        if(session->field && session->field->pending_opcode)
            self.window.title=[self.window.title stringByAppendingString:@" — actor service pending (partial scene)"];
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
        if(argc<3)throw std::runtime_error("Usage: okage-native FONT_DIRECTORY FIELD.PME [--frames N | --ui-test-frames N] [--press FRAME,FRAME --capture FILE.png] [--scene MAP.xmp --event EVENT.xmt --actor ACTOR.xmd]");
        unsigned frames=0,uiFrames=0;std::vector<unsigned> presses;fs::path output,map,event,actor;
        for(int i=3;i<argc;++i) {
            const std::string arg=argv[i];if(++i==argc)throw std::runtime_error("Missing native runtime option value");
            if(arg=="--frames")frames=std::stoul(argv[i]);
            else if(arg=="--ui-test-frames")uiFrames=std::stoul(argv[i]);
            else if(arg=="--capture")output=argv[i];
            else if(arg=="--scene")map=argv[i];
            else if(arg=="--event")event=argv[i];
            else if(arg=="--actor")actor=argv[i];
            else if(arg=="--press") {std::istringstream in(argv[i]);std::string n;while(std::getline(in,n,','))presses.push_back(std::stoul(n));}
            else throw std::runtime_error("Unknown native runtime option");
        }
        if(map.empty()!=event.empty() || (!actor.empty() && map.empty()))throw std::runtime_error("Scene requires both map and event assets");
        if(frames>36000 || uiFrames>36000 || (frames && uiFrames) || (uiFrames && output.empty()))throw std::runtime_error("Native diagnostic frame/capture bounds");
        if(frames) {
            Session session(argv[1],argv[2]);if(!map.empty())session.load_scene(map,event,actor);
            for(unsigned i=1;i<=frames;++i) {const std::uint16_t mask=std::find(presses.begin(),presses.end(),i)!=presses.end()?0x40:0;session.tick({mask,mask});}
            if(!output.empty())capture(session,output);
            std::cout<<"Native frames="<<session.frame<<" progress="<<session.text.state.progress<<" wait_release="<<session.release_frame<<'\n';return 0;
        }
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NativeApp* app=[NativeApp new];app->session=std::make_unique<Session>(argv[1],argv[2]);if(!map.empty())app->session->load_scene(map,event,actor);
        app->testFrames=uiFrames;app->testPresses=presses;app->testCapture=output;
        [NSApp setDelegate:app];[NSApp run];return app->failureStatus;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
