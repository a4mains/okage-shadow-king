#include "okage/frame_composition.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace okage {
namespace {
void extent(const Texture& image,unsigned height) {
    if(image.width!=640||image.height!=height||image.rgba.size()!=std::size_t(640)*height*4)
        throw std::runtime_error("Frame composition texture extent");
}
void transition(Texture& image,const Texture& previous,unsigned alpha) {
    for(std::size_t i=0;i<image.rgba.size();i+=4)for(unsigned c=0;c<3;++c)
        image.rgba[i+c]=(unsigned(previous.rgba[i+c])*alpha+unsigned(image.rgba[i+c])*(128-alpha))/128;
}
}
ComposedFrame FrameComposition::compose(Texture scene) const {
    extent(dialogue,224);
    // Legacy text-only presentation bypasses every scene/compositor operation.
    if(!game_view)return {frame,retain_game,dialogue,std::nullopt};
    extent(scene,224);extent(narration,224);
    if(!std::isfinite(dialogue_alpha)||dialogue_alpha<0||dialogue_alpha>1)
        throw std::runtime_error("Frame composition dialogue alpha");
    const auto choice_top=choice_cursor?(336LL+27LL*(*choice_cursor))/2:0;
    const auto choice_bottom=choice_cursor?(363LL+27LL*(*choice_cursor))/2:0;
    if(choice_cursor&&(*choice_cursor<0||choice_top<0||choice_bottom>224))
        throw std::runtime_error("Frame composition choice rectangle extent");
    if(transition_alpha>128)throw std::runtime_error("Frame composition transition alpha");
    if(live_enabled&&dialogue_alpha>0&&!live_dialogue)
        throw std::runtime_error("Frame composition missing live dialogue");
    if(live_enabled&&live_dialogue)extent(*live_dialogue,448);
    if(transition_alpha) {
        if(!transition_diagnostic)throw std::runtime_error("Frame composition missing diagnostic transition");
        extent(*transition_diagnostic,448);
        if(live_enabled) {
            if(!transition_live)throw std::runtime_error("Frame composition missing live transition");
            extent(*transition_live,448);
        }
    }
    if(apply_fade)fade.apply(scene);
    // Flat diagnostic approximation of RUW widget3, preserving Session order.
    if(choice_cursor) {
        for(int y=int(choice_top);y<int(choice_bottom);++y)for(int x=162;x<599;++x)for(unsigned c=0;c<3;++c) {
            auto& pixel=scene.rgba[(y*640+x)*4+c];pixel=(255u*48+unsigned(pixel)*80)/128;
        }
    }
    for(std::size_t i=0;i<narration.rgba.size();i+=4) {
        const unsigned alpha=narration.rgba[i+3];
        for(unsigned c=0;c<3;++c)
            scene.rgba[i+c]=(unsigned(narration.rgba[i+c])*alpha+unsigned(scene.rgba[i+c])*(255-alpha)+127)/255;
    }
    if(capture_pass) {capture_pass(scene);extent(scene,224);}
    ComposedFrame result{frame,retain_game,{},std::nullopt};
    if(live_enabled) {
        Texture image{640,448,0,0,std::vector<std::uint8_t>(640*448*4)};
        for(unsigned y=0;y<448;++y)
            std::copy_n(scene.rgba.data()+(y/2)*640*4,640*4,image.rgba.data()+y*640*4);
        for(std::size_t i=3;i<image.rgba.size();i+=4)image.rgba[i]=255;
        if(dialogue_alpha>0) {
            const auto& overlay=*live_dialogue;
            for(unsigned y=332;y<431;++y)for(unsigned x=25;x<610;++x) {
                const auto i=(y*640+x)*4;
                const float ink=overlay.rgba[i+3]/255.f*dialogue_alpha;
                for(unsigned c=0;c<3;++c) {
                    const float background=image.rgba[i+c]*(1-.75f*dialogue_alpha)+16.f*.75f*dialogue_alpha;
                    image.rgba[i+c]=std::uint8_t(overlay.rgba[i+c]*ink+background*(1-ink));
                }
            }
        }
        if(transition_alpha)transition(image,*transition_live,transition_alpha);
        result.live=std::move(image);
    }
    scene.height=448;scene.rgba.resize(640*448*4);
    for(std::size_t i=0;i<dialogue.rgba.size();i+=4) {
        const float a=(dialogue.rgba[i+3]/255.f)*dialogue_alpha;
        for(unsigned c=0;c<3;++c)
            scene.rgba[640*224*4+i+c]=std::uint8_t(dialogue.rgba[i+c]*a+16.f*(1-a));
        scene.rgba[640*224*4+i+3]=255;
    }
    if(transition_alpha)transition(scene,*transition_diagnostic,transition_alpha);
    result.diagnostic=std::move(scene);
    return result;
}
}
