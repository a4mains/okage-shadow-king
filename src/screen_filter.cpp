#include "okage/screen_filter.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace okage {
void ScreenFilter::select(unsigned id,int mode) {
    if(mode==0){updating=false;return;}
    if(mode!=1 || id!=0x49)throw std::runtime_error("Unrecovered animated screen setting");
    setting=id;updating=true;
}
void ScreenFilter::tick(float value) {
    if(!updating)return;
    if(!std::isfinite(value)||value<0||value>=2147483648.f)
        throw std::runtime_error("Unrecovered screen feedback parameter");
    alpha=std::min(unsigned(value),127u);
}
void FrameFeedback::apply(Texture& image,unsigned frame,unsigned alpha) const {
    if(alpha>127)throw std::runtime_error("Screen feedback alpha bounds");
    if(!alpha)return;
    if(!previous_ || frame==0 || frame_!=frame-1 || previous_->width!=image.width || previous_->height!=image.height)
        throw std::runtime_error("Screen feedback requires the preceding rendered native frame");
    const auto size=std::size_t(image.width)*image.height*4;
    if(image.rgba.size()!=size || previous_->rgba.size()!=size)throw std::runtime_error("Screen feedback image bounds");
    for(std::size_t i=0;i<size;i+=4)for(unsigned c=0;c<3;++c)
        image.rgba[i+c]=(unsigned(previous_->rgba[i+c])*alpha+unsigned(image.rgba[i+c])*(128-alpha))/128;
}
void FrameFeedback::retain(const Texture& image,unsigned frame,unsigned game_height) {
    if(!game_height || game_height>image.height || image.rgba.size()!=std::size_t(image.width)*image.height*4)
        throw std::runtime_error("Screen feedback history bounds");
    if(previous_ && frame<=frame_)throw std::runtime_error("Screen feedback history must advance once per frame");
    previous_=image;previous_->height=game_height;previous_->rgba.resize(std::size_t(image.width)*game_height*4);frame_=frame;
}
}
