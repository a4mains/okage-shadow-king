// Research-only adapter to the pinned emulator's offscreen renderer.
#import <AppKit/AppKit.h>
#include "reference_graphics.hpp"
#include "reference_pixel.hpp"
#include "gs/GSH_Vulkan/GSH_Vulkan.h"
#include <fstream>
#include <iostream>
#include <array>

namespace {
class OffscreenGS final : public CGSH_Vulkan {
    ResearchTraffic& traffic;
    struct RegisterWrite {uint8 index;uint64 value;};
    static constexpr unsigned historyCapacity=65536;
    std::vector<RegisterWrite> history;
    uint64 historyCount=0;
public:
    explicit OffscreenGS(ResearchTraffic& value):traffic(value) {if(traffic.retain_register_tail)history.resize(historyCapacity);}
    void WriteHistory(const std::filesystem::path& path) {
        if(history.empty())throw std::runtime_error("GS register retention was not enabled");
        std::ofstream out(path);if(!out)throw std::runtime_error("Cannot create GS register history");
        const uint64 count=std::min<uint64>(historyCount,historyCapacity),first=historyCount-count;
        out<<"{\"schema\":\"okage-gs-register-tail-v1\",\"scope\":\"Ordered writes observed by the pinned Vulkan GS handler; no VBlank alignment or initial-state completeness claim\",\"total_writes\":"<<historyCount<<",\"omitted_prefix_writes\":"<<first<<",\"writes\":[";
        for(uint64 i=first;i<historyCount;++i){const auto& r=history[i%historyCapacity];if(i!=first)out<<',';out<<'['<<i<<','<<unsigned(r.index)<<','<<r.value<<']';}
        out<<"]}\n";if(!out)throw std::runtime_error("GS register history write failure");
    }
    void ProcessHostToLocalTransfer() override {++traffic.uploads;CGSH_Vulkan::ProcessHostToLocalTransfer();}
protected:
    void InitializeImpl() override {m_instance=CreateInstance(false);CGSH_Vulkan::InitializeImpl();}
    void PresentBackbuffer() override {}
    void WriteRegisterImpl(uint8 reg,uint64 value) override {
        if(!history.empty()){history[historyCount%historyCapacity]={reg,value};++historyCount;}
        ++traffic.registers;
        if (reg==GS_REG_XYZ2 || reg==GS_REG_XYZF2) ++traffic.vertices;
        CGSH_Vulkan::WriteRegisterImpl(reg,value);
    }
};
void vacant(const std::filesystem::path& path) {
    if (std::filesystem::exists(path) || std::filesystem::is_symlink(path))
        throw std::runtime_error("Refusing existing capture: "+path.string());
}
}
CGSHandler::FactoryFunction vulkanReference(ResearchTraffic& traffic) {
    return [&traffic](){return new OffscreenGS(traffic);};
}
void captureReferenceRegisters(CGSHandler* base,const std::filesystem::path& path) {
    auto* gs=dynamic_cast<OffscreenGS*>(base);
    if(!gs)throw std::runtime_error("GS register history requires offscreen Vulkan research backend");
    vacant(path);gs->FlushWriteBuffer();
    gs->SendGSCall([&]{gs->WriteHistory(path);},true);
}
std::vector<uint8> captureShadowPixels(CGSHandler* base) {
    auto* gs=dynamic_cast<CGSH_Vulkan*>(base);
    if(!gs)throw std::runtime_error("Shadow raster requires Vulkan research backend");
    gs->FlushWriteBuffer();gs->SendGSCall([](){},true);
    const auto bitmap=gs->GetFramebuffer(uint64(10)<<16);
    if(bitmap.GetWidth()<640||bitmap.GetHeight()<224||bitmap.GetBitsPerPixel()!=32)
        throw std::runtime_error("Shadow framebuffer extent");
    std::vector<uint8> pixels(640*224*4);
    for(unsigned y=0;y<224;++y)for(unsigned x=0;x<640;++x) {
        const auto* source=bitmap.GetPixels()+y*bitmap.GetPitch()+x*4;
        auto* dest=pixels.data()+(y*640+x)*4;
        dest[0]=source[2];dest[1]=source[1];dest[2]=source[0];dest[3]=source[3];
    }
    return pixels;
}
void captureReference(CGSHandler* base,const std::filesystem::path& prefix) {
    @autoreleasepool {
        auto* gs=dynamic_cast<CGSH_Vulkan*>(base);
        if (!gs) throw std::runtime_error("A rendered capture requires the Vulkan reference backend");
        gs->FlushWriteBuffer(); gs->SendGSCall([](){},true);
        const auto display=gs->GetCurrentDisplayInfo();
        if (!display.width || !display.height) throw std::runtime_error("No active reference display");
        const auto metadata=prefix.string()+".json";vacant(metadata);
        for (unsigned layer=0;layer<2;++layer) if (display.layers[layer].enabled)
            vacant(prefix.string()+".layer"+std::to_string(layer)+".png");
        std::ofstream out(metadata);
        if (!out) throw std::runtime_error("Cannot create reference display metadata");
        out<<"{\n  \"width\": "<<display.width<<", \"height\": "<<display.height<<",\n  \"layers\": [\n";
        bool first=true;
        for (unsigned i=0;i<2;++i) {
            const auto& layer=display.layers[i];if (!layer.enabled) continue;
            const uint64 frame=(layer.bufPtr/8192)|(uint64(layer.bufWidth/64)<<16)|(uint64(layer.psm)<<24);
            const auto bitmap=gs->GetFramebuffer(frame);
            if (!layer.width || !layer.height || layer.width>bitmap.GetWidth() || layer.height>bitmap.GetHeight())
                throw std::runtime_error("Reference display exceeds framebuffer capture");
            auto png=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:layer.width pixelsHigh:layer.height bitsPerSample:8
                samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                bytesPerRow:layer.width*4 bitsPerPixel:32];
            if (!png) throw std::runtime_error("Cannot allocate reference PNG");
            for (unsigned y=0;y<layer.height;++y) for (unsigned x=0;x<layer.width;++x) {
                if(bitmap.GetBitsPerPixel()!=32)throw std::runtime_error("Unsupported Vulkan bitmap format");
                const auto rgba=reference_rgba(bitmap.GetPixels()+y*bitmap.GetPitch()+x*4);
                auto* pixel=png.bitmapData+(y*layer.width+x)*4;
                std::copy(rgba.begin(),rgba.end(),pixel);
            }
            const auto path=prefix.string()+".layer"+std::to_string(i)+".png";
            auto data=[png representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            NSError* error=nil;
            if (![data writeToFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataWritingWithoutOverwriting error:&error])
                throw std::runtime_error(error.localizedDescription.UTF8String);
            if (!first) out<<",\n";first=false;
            out<<"    {\"layer\": "<<i<<", \"width\": "<<layer.width<<", \"height\": "<<layer.height
               <<", \"offset_x\": "<<layer.offsetX<<", \"offset_y\": "<<layer.offsetY
               <<", \"buffer_address\": "<<layer.bufPtr<<", \"buffer_width\": "<<layer.bufWidth
               <<", \"psm\": "<<layer.psm<<", \"constant_alpha\": "<<unsigned(layer.constantAlpha)
               <<", \"use_constant_alpha\": "<<(layer.useConstantAlpha ? "true" : "false")<<"}";
            std::cout<<"Captured rendered reference layer "<<i<<": "<<layer.width<<"x"<<layer.height<<" to "<<path<<std::endl;
        }
        out<<"\n  ],\n  \"scope\": \"Rendered framebuffer layers before display compositing/deinterlacing; BGRA framebuffer converted to RGBA; PNG alpha made opaque\"\n}\n";
    }
}
