#import <AppKit/AppKit.h>
#include "okage/assets.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

// Research viewer only. All asset parsing/decoding is the native C++ library.
@interface TextureView : NSView
@property(strong) NSImage* texture;
@end
@implementation TextureView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect bounds = self.bounds;
    for (int y=0; y<bounds.size.height; y+=24) {
        for (int x=0; x<bounds.size.width; x+=24) {
            [[NSColor colorWithWhite:((x/24+y/24)%2 ? 0.18 : 0.23) alpha:1] setFill];
            NSRectFill(NSMakeRect(x,y,24,24));
        }
    }
    if (!self.texture) return;
    NSSize size = self.texture.size;
    double scale = std::min((bounds.size.width-48)/size.width, (bounds.size.height-48)/size.height);
    NSRect dest = NSMakeRect((bounds.size.width-size.width*scale)/2,
                            (bounds.size.height-size.height*scale)/2,size.width*scale,size.height*scale);
    [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
    [self.texture drawInRect:dest fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
}
@end

@interface Viewer : NSObject<NSApplicationDelegate, NSWindowDelegate> {
    std::vector<std::filesystem::path> paths;
    std::size_t current;
}
@property(strong) NSWindow* window;
@property(strong) TextureView* canvas;
@property(copy) NSString* initialPath;
- (void)openFile:(id)sender;
- (void)next:(id)sender;
- (void)previous:(id)sender;
@end

@implementation Viewer
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,640)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.delegate = self;
    self.window.title = @"Okage Asset Viewer — Open an XIM texture";
    self.canvas = [[TextureView alloc] initWithFrame:self.window.contentView.bounds];
    self.canvas.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
    self.window.contentView = self.canvas;
    [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (self.initialPath) [self loadPath:self.initialPath];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { (void)sender; return YES; }
- (void)loadPath:(NSString*)file {
    try {
        auto path = std::filesystem::absolute(file.fileSystemRepresentation);
        paths.clear();
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return std::tolower(c);});
            if (entry.is_regular_file() && ext == ".xim") paths.push_back(entry.path());
        }
        std::sort(paths.begin(),paths.end());
        auto found = std::find(paths.begin(),paths.end(),path);
        if (found == paths.end()) throw std::runtime_error("Choose an .xim texture");
        current = found-paths.begin();
        [self displayTexture];
    } catch (const std::exception& e) { [self showError:e.what()]; }
}
- (void)showError:(const char*)error {
    NSAlert* alert = [NSAlert new]; alert.messageText = @"Unable to display texture";
    alert.informativeText = [NSString stringWithUTF8String:error]; [alert runModal];
}
- (void)displayTexture {
    try {
        const auto& path = paths.at(current);
        const auto size = std::filesystem::file_size(path);
        if (size > 128*1024*1024) throw std::runtime_error("Texture exceeds input limit");
        std::vector<std::uint8_t> data(size);
        std::ifstream input(path,std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("Unable to read file");
        const auto decoded = okage::decode_xim(data);
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
            pixelsWide:decoded.width pixelsHigh:decoded.height bitsPerSample:8 samplesPerPixel:4
            hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:decoded.width*4 bitsPerPixel:32];
        if (!rep) throw std::runtime_error("Unable to allocate image");
        std::copy(decoded.rgba.begin(),decoded.rgba.end(),rep.bitmapData);
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(decoded.width,decoded.height)];
        [image addRepresentation:rep]; self.canvas.texture = image; [self.canvas setNeedsDisplay:YES];
        self.window.title = [NSString stringWithFormat:@"Okage Asset Viewer — %s — %u × %u — %zu/%zu",
            path.filename().c_str(),decoded.width,decoded.height,current+1,paths.size()];
        self.window.representedFilename = [NSString stringWithUTF8String:path.c_str()];
    } catch (const std::exception& e) { [self showError:e.what()]; }
}
- (void)openFile:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel]; panel.canChooseDirectories=NO; panel.allowsMultipleSelection=NO;
    if ([panel runModal] == NSModalResponseOK) [self loadPath:panel.URL.path];
}
- (void)next:(id)sender { (void)sender; if (!paths.empty()) { current=(current+1)%paths.size(); [self displayTexture]; } }
- (void)previous:(id)sender { (void)sender; if (!paths.empty()) { current=(current+paths.size()-1)%paths.size(); [self displayTexture]; } }
@end

int main(int argc, const char** argv) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        Viewer* delegate = [Viewer new];
        if (argc > 1) delegate.initialPath = [NSString stringWithUTF8String:argv[1]];
        app.delegate = delegate;
        NSMenu* menu = [NSMenu new];
        NSMenuItem* root = [NSMenuItem new]; [menu addItem:root];
        NSMenu* actions = [[NSMenu alloc] initWithTitle:@"Okage Asset Viewer"];
        root.submenu = actions;
        for (NSArray* item in @[@[@"Open Texture…",NSStringFromSelector(@selector(openFile:)),@"o"],
                               @[@"Next Texture",NSStringFromSelector(@selector(next:)),@"]"],
                               @[@"Previous Texture",NSStringFromSelector(@selector(previous:)),@"["]]) {
            NSMenuItem* action = [[NSMenuItem alloc] initWithTitle:item[0] action:NSSelectorFromString(item[1]) keyEquivalent:item[2]];
            action.target=delegate; [actions addItem:action];
        }
        [actions addItem:[NSMenuItem separatorItem]];
        [actions addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        app.mainMenu=menu;
        [app run];
    }
}
