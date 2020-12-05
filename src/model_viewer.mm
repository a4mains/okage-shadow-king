#import <AppKit/AppKit.h>
#include "okage/pose.hpp"
#include "okage/animation.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

struct Mesh {
    std::vector<okage::Float4> positions;
    std::vector<std::vector<std::uint32_t>> polygons;
};
static std::vector<std::uint8_t> readModelFile(const std::filesystem::path& path) {
    auto size=std::filesystem::file_size(path);
    if (size>128*1024*1024) throw std::runtime_error("Model input exceeds limit");
    std::ifstream input(path,std::ios::binary);
    std::vector<std::uint8_t> bytes(size);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("Unable to read model input");
    return bytes;
}
@interface MeshView : NSView {
    std::vector<Mesh> meshes;
    okage::Float4 center;
    double radius, yaw, pitch, zoom;
    NSPoint lastDrag;
    okage::ModelFile modelFile;
    okage::SineTable sine;
    std::unique_ptr<okage::Animation> animation;
    std::vector<std::uint8_t> animationBytes;
    NSUInteger entry, frame;
    BOOL playing;
    NSTimer* timer;
}
@property(copy) NSString* caption;
- (void)loadModel:(NSString*)path;
- (void)resetCamera:(id)sender;
- (void)loadAnimation:(NSString*)path entry:(NSUInteger)selected;
- (void)stepAnimation:(id)sender;
- (void)toggleAnimation:(id)sender;
- (void)restartAnimation:(id)sender;
@end
@implementation MeshView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)resetCamera:(id)sender {
    (void)sender; yaw=0.45; pitch=0.12; zoom=1; [self setNeedsDisplay:YES];
}
- (void)loadModel:(NSString*)name {
    const std::filesystem::path path(name.fileSystemRepresentation);
    auto file=okage::read_xmd(readModelFile(path));
    sine=okage::make_sine_table();
    std::vector<Mesh> pending;
    okage::Float4 low{INFINITY,INFINITY,INFINITY,0},high{-INFINITY,-INFINITY,-INFINITY,0};
    std::size_t polygon_count=0,bone_count=0;
    for (const auto& model:file.models) {
        const auto pose=okage::rest_pose(model,sine); bone_count+=pose.size();
        for (const auto& block:model.blocks) {
            Mesh mesh; mesh.positions=okage::posed_positions(block,pose);
            for (const auto& polygon:block.polygons) {
                std::vector<std::uint32_t> corners;
                for (const auto& corner:polygon.corners) {
                    const auto index=static_cast<std::uint32_t>(corner[0]);
                    const auto& v=mesh.positions.at(index);
                    for (unsigned j=0;j<3;++j) {
                        if (!std::isfinite(v[j])) throw std::runtime_error("Non-finite posed geometry");
                        low[j]=std::min(low[j],v[j]); high[j]=std::max(high[j],v[j]);
                    }
                    corners.push_back(index);
                }
                mesh.polygons.push_back(std::move(corners)); ++polygon_count;
            }
            pending.push_back(std::move(mesh));
        }
    }
    if (!polygon_count) throw std::runtime_error("Model has no supported polygons");
    radius=0;
    for (unsigned j=0;j<3;++j) { center[j]=(low[j]+high[j])*0.5f; radius+=std::pow((high[j]-low[j])*0.5,2); }
    radius=std::max(std::sqrt(radius),0.001);
    meshes=std::move(pending);
    modelFile=std::move(file); animation.reset(); animationBytes.clear(); playing=NO; [timer invalidate]; timer=nil;
    self.caption=[NSString stringWithFormat:@"%zu bones   ·   %zu polygons   ·   Rest pose / wireframe\nDrag to orbit   ·   Scroll to zoom   ·   ⌘R to reset",
        bone_count,polygon_count];
    [self resetCamera:nil];
}
- (void)updateAnimationGeometry {
    const auto values=animation->values();
    const auto pose=okage::animated_pose(modelFile.models[0],values,sine,animation->scale_channels());
    for (std::size_t i=0;i<meshes.size();++i)
        meshes[i].positions=okage::posed_positions(modelFile.models[0].blocks[i],pose);
    self.caption=[NSString stringWithFormat:@"XMT entry %lu   ·   Tick %lu   ·   %@\nSpace: play/pause   ·   →: step   ·   ⌘B: restart   ·   Preview: 30 ticks/s (game timing unverified)",
        (unsigned long)entry,(unsigned long)frame,playing ? @"Playing" : @"Paused"];
    [self setNeedsDisplay:YES];
}
- (void)loadAnimation:(NSString*)path entry:(NSUInteger)selected {
    if (modelFile.models.size()!=1) throw std::runtime_error("Animation inspection requires one model");
    animationBytes=readModelFile(path.fileSystemRepresentation); entry=selected;
    [self restartAnimation:nil];
    [timer invalidate];
    __weak MeshView* weakSelf=self;
    timer=[NSTimer scheduledTimerWithTimeInterval:1.0/30.0 repeats:YES block:^(NSTimer* tick) {
        (void)tick; MeshView* view=weakSelf;
        if (view && view->playing) [view stepAnimation:nil];
    }];
}
- (void)restartAnimation:(id)sender {
    (void)sender;
    if (animationBytes.empty()) return;
    try {
        // Header validation is owned by Animation; inspect only the channel-mode word here.
        if (animationBytes.size()<32) throw std::runtime_error("Truncated XMT header");
        bool scale=animationBytes[20] || animationBytes[21] || animationBytes[22] || animationBytes[23];
        auto pending=std::make_unique<okage::Animation>(animationBytes,4+modelFile.models[0].bones.size()*(scale ? 3 : 2));
        pending->start(0); pending->values(); pending->start(entry);
        animation=std::move(pending); frame=0; playing=NO;
        [self updateAnimationGeometry];
    } catch (const std::exception& e) {
        playing=NO; animation.reset();
        self.caption=[NSString stringWithFormat:@"Animation stopped: %s",e.what()]; [self setNeedsDisplay:YES];
    }
}
- (void)stepAnimation:(id)sender {
    (void)sender; if (!animation) return;
    try { animation->tick(); ++frame; [self updateAnimationGeometry]; }
    catch (const std::exception& e) {
        playing=NO; animation.reset();
        self.caption=[NSString stringWithFormat:@"Animation stopped at tick %lu: %s",(unsigned long)frame,e.what()];
        [self setNeedsDisplay:YES];
    }
}
- (void)toggleAnimation:(id)sender {
    (void)sender; if (!animation) return; playing=!playing; [self updateAnimationGeometry];
}
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode==124) [self stepAnimation:nil];
    else if ([event.characters isEqualToString:@" "]) [self toggleAnimation:nil];
    else [super keyDown:event];
}
- (void)mouseDown:(NSEvent*)event { lastDrag=[self convertPoint:event.locationInWindow fromView:nil]; }
- (void)mouseDragged:(NSEvent*)event {
    const auto point=[self convertPoint:event.locationInWindow fromView:nil];
    yaw+=(point.x-lastDrag.x)*0.01; pitch=std::clamp(pitch-(point.y-lastDrag.y)*0.01,-1.5,1.5);
    lastDrag=point; [self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)event {
    zoom=std::clamp(zoom*std::exp(event.scrollingDeltaY*0.015),0.15,8.0); [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor colorWithRed:0.055 green:0.070 blue:0.095 alpha:1] setFill]; NSRectFill(self.bounds);
    if (!meshes.empty()) {
        const double scale=std::max(1.0,std::min(self.bounds.size.width,self.bounds.size.height-96))*0.43/radius*zoom;
        const double cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
        [[NSColor colorWithRed:0.57 green:0.80 blue:0.89 alpha:0.8] setStroke];
        for (const auto& mesh:meshes) {
            std::vector<NSPoint> projected; projected.reserve(mesh.positions.size());
            for (const auto& v:mesh.positions) {
                const double x=v[0]-center[0], y=-(v[1]-center[1]),z=v[2]-center[2];
                const double rx=x*cy+z*sy, rz=-x*sy+z*cy, ry=y*cp-rz*sp;
                projected.push_back(NSMakePoint(self.bounds.size.width*0.5+rx*scale,
                    self.bounds.size.height*0.5+20+ry*scale));
            }
            NSBezierPath* lines=[NSBezierPath bezierPath]; lines.lineWidth=0.6;
            for (const auto& polygon:mesh.polygons) {
                [lines moveToPoint:projected[polygon.front()]];
                for (std::size_t j=1;j<polygon.size();++j) [lines lineToPoint:projected[polygon[j]]];
                [lines closePath];
            }
            [lines stroke];
        }
    }
    NSString* message=self.caption ?: @"Open an XMD model with ⌘O";
    [message drawAtPoint:NSMakePoint(24,20) withAttributes:@{
        NSFontAttributeName:[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName:[NSColor colorWithWhite:0.75 alpha:1]}];
}
@end

@interface ModelViewer : NSObject<NSApplicationDelegate>
@property(strong) NSWindow* window;
@property(strong) MeshView* canvas;
@property(copy) NSString* initialPath;
@property(copy) NSString* animationPath;
@property NSUInteger animationEntry;
@end
@implementation ModelViewer
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1000,800)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title=@"Okage Model Research";
    self.canvas=[[MeshView alloc] initWithFrame:self.window.contentView.bounds];
    self.canvas.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    self.window.contentView=self.canvas;
    [self.window center]; [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    if (self.initialPath) [self loadPath:self.initialPath];
    if (self.animationPath) {
        try { [self.canvas loadAnimation:self.animationPath entry:self.animationEntry]; }
        catch (const std::exception& e) {
            self.canvas.caption=[NSString stringWithFormat:@"Animation input error: %s",e.what()];
        }
    }
    [self.window makeFirstResponder:self.canvas];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { (void)app; return YES; }
- (void)loadPath:(NSString*)path {
    try {
        [self.canvas loadModel:path];
        self.window.title=[@"Okage Model Research — " stringByAppendingString:path.lastPathComponent];
        self.window.representedFilename=path;
    } catch (const std::exception& e) {
        NSAlert* alert=[NSAlert new]; alert.messageText=@"Unable to display model";
        alert.informativeText=[NSString stringWithUTF8String:e.what()]; [alert runModal];
    }
}
- (void)openModel:(id)sender {
    (void)sender; NSOpenPanel* panel=[NSOpenPanel openPanel];
    panel.canChooseDirectories=NO; panel.allowsMultipleSelection=NO;
    if ([panel runModal]==NSModalResponseOK) [self loadPath:panel.URL.path];
}
- (void)resetCamera:(id)sender { [self.canvas resetCamera:sender]; }
- (void)restartAnimation:(id)sender { [self.canvas restartAnimation:sender]; }
@end

int main(int argc,const char** argv) {
    @autoreleasepool {
        NSApplication* app=[NSApplication sharedApplication]; [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        ModelViewer* delegate=[ModelViewer new]; app.delegate=delegate;
        if (argc>1) delegate.initialPath=[NSString stringWithUTF8String:std::filesystem::absolute(argv[1]).c_str()];
        if (argc>2) delegate.animationPath=[NSString stringWithUTF8String:std::filesystem::absolute(argv[2]).c_str()];
        if (argc>3) {
            try { delegate.animationEntry=std::stoul(argv[3]); }
            catch (...) { fprintf(stderr,"Invalid animation entry\n"); return 1; }
        }
        NSMenu* menu=[NSMenu new]; NSMenuItem* root=[NSMenuItem new]; [menu addItem:root];
        NSMenu* actions=[[NSMenu alloc] initWithTitle:@"Okage Model Research"]; root.submenu=actions;
        NSMenuItem* open=[actions addItemWithTitle:@"Open Model…" action:@selector(openModel:) keyEquivalent:@"o"]; open.target=delegate;
        NSMenuItem* reset=[actions addItemWithTitle:@"Reset Camera" action:@selector(resetCamera:) keyEquivalent:@"r"]; reset.target=delegate;
        NSMenuItem* restart=[actions addItemWithTitle:@"Restart Animation" action:@selector(restartAnimation:) keyEquivalent:@"b"]; restart.target=delegate;
        [actions addItem:[NSMenuItem separatorItem]]; [actions addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        app.mainMenu=menu; [app run];
    }
}
