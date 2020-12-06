#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("texture animation assertion failed"); }
template<class F> void rejects(F action) {
    bool failed=false; try {action();} catch (const std::runtime_error&) {failed=true;} require(failed);
}
int main() {
    try {
        const auto xta=okage::read_xta("\t1\r\n\t2\r\n\r\n\t=EYE.XIM\r\n=MOUTH.XIM\r\n");
        require(xta.groups==std::vector<std::int16_t>{1,2});
        require(xta.variants==std::vector<std::string>{"eye.xim","mouth.xim"});
        okage::ModelFile model; model.textures={"body","eye","mouth"}; model.models.emplace_back();
        model.models[0].blocks.resize(2);
        for (int texture:{0,1,2,1}) model.models[0].blocks[0].textures.push_back({static_cast<std::int16_t>(texture),0,0,0,{}});
        model.models[0].blocks[1].textures.push_back({1,0,0,0,{}});
        okage::TextureBindings bindings(model,xta);
        const auto initial=bindings.images();
        bindings.select(0,1); require(bindings.images()==std::vector<std::int32_t>{0,3,2,3,3});
        bindings.select(1,2); require(bindings.images()==std::vector<std::int32_t>{0,3,4,3,3});
        bindings.select(0,0); require(bindings.images()==std::vector<std::int32_t>{0,1,4,1,1});
        bindings.restore(); require(bindings.images()==initial);
        rejects([&]{bindings.select(2,0);}); rejects([&]{bindings.select(0,3);});
        require(bindings.images()==initial);
        for (auto text:{"=../escape.xim", "=/absolute.xim", "=a/b.xim", "=a\\b.xim", "=", "-1", "32768", ";dummy;", "1junk"})
            rejects([&]{okage::read_xta(text);});
        rejects([&]{okage::TextureBindings b(model,okage::read_xta("4"));});
        // Recovered op 0x34: unsigned middle argument is variant; signed high
        // argument is group. A missing callback is an error, never a no-op.
        std::vector<std::uint8_t> script(40);
        auto put=[&](std::size_t at,std::uint32_t word) {for (unsigned j=0;j<4;++j) script[at+j]=word>>(8*j);};
        put(0,0x544d58);put(4,8);put(16,0xffffffff);put(32,0x00100234);
        okage::Animation animation(script,1);
        rejects([&]{animation.start(0);});
        animation.set_texture_callback([&](auto group,auto variant){bindings.select(group,variant);});
        animation.start(0); require(bindings.images()==std::vector<std::int32_t>{0,1,4,1,1});
        std::cout<<"Texture groups, duplicate bindings, restore, callback operands and invalid input passed\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
