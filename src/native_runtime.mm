#import <AppKit/AppKit.h>
#include "okage/dialogue.hpp"
#include "okage/event.hpp"
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
    std::uint64_t service_request_count=0;
    Session(const fs::path& directory,const fs::path& pme):
        fonts{okage::read_fnt(readData(directory/"IQ24.FNT")),okage::read_fnt(readData(directory/"BMUI.FNT")),okage::read_fnt(readData(directory/"BM.FNT"))},
        text(okage::read_pme(readData(pme)).message(10100)) {
        text.fonts=fonts;text.state.flags=0x8041;text.state.font=2;text.state.period=1;
        clock.glyph_flags=0x10;opacity.set(1,8);
    }
    void tick(okage::DialogueButtons input) {
        ++frame;
        const std::array<std::uint32_t,2> layers={0x50,controller.layer_flags};
        if(!release_frame && okage::poll_dialogue_wait(wait,2,{true,0,layers,std::int32_t(text.state.progress)})==0)release_frame=frame;
        opacity.tick();text.frame(clock,opacity.visible());
        if(controller.stage==5) {
            const auto effects=controller.update(text,input);
            if(effects.close)opacity.set(128,-4);
            if(effects.cue>=0 || effects.stop_voice || effects.stop_effect)++service_request_count;
        }
    }
    okage::Texture surface() const {return okage::rasterize_text(fonts,text.glyphs,640,224);}
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
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,336)
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
        NSImage* image=[[NSImage alloc] initWithSize:NSMakeSize(640,224)];[image addRepresentation:bitmap(session->surface())];
        self.view.image=image;self.view.opacity=session->opacity.value;self.view.needsDisplay=YES;
        self.window.title=[NSString stringWithFormat:@"Okage native dialogue — Return/Space — progress %u — event %@",
            session->text.state.progress,session->release_frame ? @"released" : @"waiting"];
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
    }
}
- (void)windowDidResignKey:(NSNotification*)notification {(void)notification;self.view.held=0;self.view.pressed=0;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {(void)app;return YES;}
@end
int main(int argc,const char** argv) {
    @autoreleasepool {try {
        if(argc<3)throw std::runtime_error("Usage: okage-native FONT_DIRECTORY FIELD.PME [--frames N | --ui-test-frames N] [--press FRAME,FRAME --capture FILE.png]");
        unsigned frames=0,uiFrames=0;std::vector<unsigned> presses;fs::path output;
        for(int i=3;i<argc;++i) {
            const std::string arg=argv[i];if(++i==argc)throw std::runtime_error("Missing native runtime option value");
            if(arg=="--frames")frames=std::stoul(argv[i]);
            else if(arg=="--ui-test-frames")uiFrames=std::stoul(argv[i]);
            else if(arg=="--capture")output=argv[i];
            else if(arg=="--press") {std::istringstream in(argv[i]);std::string n;while(std::getline(in,n,','))presses.push_back(std::stoul(n));}
            else throw std::runtime_error("Unknown native runtime option");
        }
        if(frames>36000 || uiFrames>36000 || (frames && uiFrames) || (uiFrames && output.empty()))throw std::runtime_error("Native diagnostic frame/capture bounds");
        if(frames) {
            Session session(argv[1],argv[2]);
            for(unsigned i=1;i<=frames;++i) {const std::uint16_t mask=std::find(presses.begin(),presses.end(),i)!=presses.end()?0x40:0;session.tick({mask,mask});}
            if(!output.empty())capture(session,output);
            std::cout<<"Native frames="<<session.frame<<" progress="<<session.text.state.progress<<" wait_release="<<session.release_frame<<'\n';return 0;
        }
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NativeApp* app=[NativeApp new];app->session=std::make_unique<Session>(argv[1],argv[2]);
        app->testFrames=uiFrames;app->testPresses=presses;app->testCapture=output;
        [NSApp setDelegate:app];[NSApp run];
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
