#include "okage/font.hpp"
#include "okage/text.hpp"
#include <iostream>
#include <stdexcept>

void require(bool value) {if(!value)throw std::runtime_error("Font check failed");}
template<class F> void rejects(F f) {try {f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected font rejection");}
int main() {try {
    // One two-pixel-wide glyph A, four rows high, baseline 3 and left bearing 1.
    std::vector<std::uint8_t> data={3,0,4,0,65,65,0,0,
        2,2,3,1,1,65,0,0,0,0,0,0,2,0,0,0,0,4,2,4,
        4,0,2,0,0x21,0x43,0x65,0x0f};
    const auto font=okage::read_fnt(data);
    require(font.glyphs.size()==1 && font.find(64)==nullptr && font.find(66)==nullptr);
    require(font.find(65)->metrics[8]==32 && font.atlas.rgba[3]==255 && font.atlas.rgba[7]==240);
    require(font.atlas.rgba[27]==32 && font.atlas.rgba[31]==0);
    okage::DialogueText text({"A"});text.fonts=std::span(&font,1);text.state.flags=0x8001;
    text.state.x=10;text.state.y=20;text.state.period=0;text.seek({0,0,true});okage::TextClock clock;text.frame(clock);
    require(text.state.x==13 && text.glyphs.size()==1);
    const auto& g=text.glyphs[0];require(g.x==33 && g.y==53 && g.width==2 && g.height==4);
    auto image=okage::rasterize_text(text.fonts,text.glyphs,64,64);
    require(image.rgba[(53*64+33)*4+3]==255 && image.rgba[(53*64+34)*4+3]==240);
    auto clipped=text.glyphs;clipped[0].x=-1;clipped[0].y=-1;
    image=okage::rasterize_text(text.fonts,clipped,2,2);require(image.rgba[3]==208);
    for(std::size_t n=0;n<data.size();++n)rejects([&]{okage::read_fnt(okage::Bytes(data.data(),n));});
    auto bad=data;bad.push_back(0);rejects([&]{okage::read_fnt(bad);});
    bad=data;bad[4]=66;rejects([&]{okage::read_fnt(bad);});
    bad=data;bad[26]=3;rejects([&]{okage::read_fnt(bad);});
    bad=data;bad[0]=7;rejects([&]{okage::read_fnt(bad);});
    auto corrupt=font;corrupt.glyphs[0].metrics[6]=1000;
    rejects([&]{okage::rasterize_text(std::span(&corrupt,1),text.glyphs,64,64);});
    std::cout<<"Font bounds, metrics, layout and composition passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
